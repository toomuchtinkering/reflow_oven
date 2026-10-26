# About screen: OTA slot switch + electronics temperature — design

Date: 2026-06-10
Status: approved (brainstorming session)

## Goal

Two additions to the Settings → About sub-screen:

1. **Boot-other-slot fallback.** A UI control that reboots the device into the
   firmware image in the inactive OTA slot, as a recovery path when an OTA
   update ships a bad image and the web/OTA route is unusable.
2. **Electronics temperature.** A live readout of the MAX31855 cold-junction
   temperature, to observe how warm the electronics get during a reflow run.

Decisions made during brainstorming:

- Temperature source is the **MAX31855 cold junction only** (±2 °C, measures
  the thermocouple-amp board). The classic-ESP32 die sensor (undocumented ROM
  `temprature_sens_read()`, large self-heating offset, returns a constant 128
  on some chips) is **not** used.
- **Live value only** — no peak-since-boot tracking.
- Slot switch uses the **existing idle-reservation** (`control_begin_ota()`),
  not a new control-task command.

## Resource budget (hard constraint)

The pid-tuning firmware runs at ~10 KB free heap, and tight resources already
broke an OTA upload once. This feature must stay near-zero cost:

- **Heap:** no new tasks, no buffers, no allocations at runtime. The only new
  state is one `float` in the existing `oven_state_t` and a few pointers in
  BSS.
- **LVGL pool (64 KB):** the About sub-screen already exists and is cached;
  this adds ~3 labels and 1 button to it — well under 1 KB of LVGL objects.
- **Flash:** pure decoder + UI glue, roughly 1–2 KB of code. No new
  components, no new dependencies.

Any implementation choice that needs a new task, a heap buffer, or a new
library is out of scope and wrong.

## 1. Cold-junction data path

**New pure module `src/drivers/tc_decode.{c,h}`** (no IDF includes, compiles
in the `native` test env, which excludes `drivers/thermocouple.c`):

```c
typedef struct {
    float   tc_c;      /* thermocouple temp, 0.25 °C/LSB (bits 31:18) */
    float   cj_c;      /* cold-junction temp, 0.0625 °C/LSB (bits 15:4, signed 12-bit) */
    uint8_t fault_flags; /* FAULT_TC_OPEN / FAULT_TC_SCG / FAULT_TC_SCV mapping */
} tc_frame_t;

void tc_decode(uint32_t frame, tc_frame_t *out);
```

The decode logic currently inlined in `thermocouple_read()` moves here
unchanged; cold-junction decode is added (sign-extend bit 15 over the 12-bit
field). `fault_flags` mapping keeps the existing convention, including the
bit-16 "any fault" fallback to `FAULT_TC_OPEN`.

**`thermocouple_read()`** gains an out-param: `bool thermocouple_read(float
*temp_c, float *cj_c, uint8_t *fault_flags, uint32_t *age_ms)`. The
cold-junction value is written whenever the SPI transfer succeeds — **also on
thermocouple faults** (the MAX31855 reports a valid cold junction with the
probe unplugged). Only an SPI-transport failure leaves it unwritten.

**`oven_state_t`** gains `float cj_temp_c`. `control_task` publishes it each
tick; on SPI failure the previous value is retained (same convention as
`temp_c`). The UI snapshot copies it for free (whole-struct memcpy).

## 2. About screen — live temperature

New line in the Hardware section: `  TC board  41.2 °C`.

- The About screen keeps its build-once-and-cache design. The temperature
  label's pointer is kept in module static state.
- New `screen_about_update(const oven_state_t *snap)` is called from the 4 Hz
  `ui_refresh_timer` in `ui_task.c`, after the existing `screen_*_update`
  calls. It rewrites only the temperature label, and only while the About
  sub-screen is actually visible — same guard pattern as
  `screen_network_update()`. `cj_temp_c` starts as `NAN` (set by
  `control_task` at init, before the first publish); the label shows `--`
  while the value `isnan()`, so a legitimate 0.0 °C reading is not mistaken
  for "no data".

## 3. About screen — "Boot other slot" button

A full-width button at the bottom of the About sub-screen, styled like the
settings card rows: label `Boot other slot (vX.Y.Z)` with the version taken
from the other slot's app descriptor (already probed for the "Other" line).

Flow on tap:

1. **Disabled state:** if `esp_ota_get_partition_description(next, …)` failed
   at screen-create time, the button is created disabled (faint text, label
   `Boot other slot (empty)`), no event handler fires.
2. **Confirm:** `settings_confirm("Boot other slot", "Reboot into firmware
   vX.Y.Z on <label>?", on_ok, NULL)` — the existing modal.
3. **Idle guard:** `on_ok` calls `control_begin_ota()` (existing handshake,
   blocks ≤ ~0.6 s worst case — acceptable in an LVGL event callback). If it
   returns false → modal info box "Oven busy — stop the run first." and stop.
4. **Switch:** `esp_ota_set_boot_partition(other)`. This validates the image
   (a half-written or corrupt slot fails here). On error →
   `control_end_ota()`, modal "Switch failed." and stop.
5. **Reboot:** `esp_restart()`. The reservation is intentionally not released
   (same rationale as the OTA upload path). The switched-to image boots as
   `ESP_OTA_IMG_NEW` → `PENDING_VERIFY` and goes through the existing
   `ota_validator` self-check, so rollback protection keeps working.

The info-only modals in steps 3–4 reuse `settings_confirm()` with a no-op
confirm (or a small `settings_info()` helper if cleaner — implementer's
choice, as long as no new modal machinery is invented).

## 4. Error handling summary

| Failure | Behavior |
|---|---|
| Other slot empty / unreadable descriptor | Button rendered disabled |
| Reflow or step test running | "Oven busy" modal; nothing changes |
| Image fails validation in `esp_ota_set_boot_partition` | "Switch failed" modal; reservation released |
| TC probe unplugged / TC fault | Cold-junction readout stays live; existing fault handling untouched |
| SPI transport failure | `cj_temp_c` retains last value (same as `temp_c`) |

## 5. Testing

- **TDD (native env)** on `tc_decode`: thermocouple decode matches the
  current inline logic (incl. negative temps), cold-junction decode incl.
  sign extension and 0.0625 °C scaling, fault-bit mapping incl. the bit-16
  fallback, CJ validity on faulted frames.
- **On hardware:** flash via the OTA workflow; verify the TC-board label is
  plausible and rises during a heat-up; verify slot switch round-trips
  (A→B→A), the disabled state with an erased slot is correct, and the "Oven
  busy" path triggers during a step test.

## Out of scope

- ESP32 die-temperature sensor.
- Peak/min tracking, history, or graphing of the electronics temperature.
- Exposing cold-junction temperature via the web/telemetry API.
- Any change to OTA upload, rollback, or validator behavior.

# Reflow Oven Controller — MVP Design

Date: 2026-05-16
Status: Approved — ready for implementation plan

## 1. Scope

The MVP firmware turns the LCDwiki E32R35T board into a stand-alone reflow oven controller: it reads a K-type thermocouple, drives an SSR-controlled heater along a five-stage profile (heat-up → soak → heat-up → reflow → cool-down) using a PID loop, and lets the user view live progress, edit the profile and PID gains, and start / stop runs from a touchscreen UI.

Behavior follows the [STM32 + Nextion reference design](../../../dev_ressources/reference_design/reference_design.md) feature-for-feature, with three concrete safety gaps closed:

- MAX31855 fault-bit detection (open thermocouple, short to GND/VCC)
- Sensor-stale detection (no fresh reading for too long)
- Hard over-temperature cutoff

Out of scope for MVP (deferred to v2):

- ~~Buzzer feedback at stage transitions / completion~~ — implemented 2026-05-23, see [audio-signals design](2026-05-23-audio-signals-design.md).
- Persistent emergency-stop button on every screen
- Multiple named profile presets
- WiFi / OTA / MQTT
- SD-card profile import / export

## 2. Hardware

Target: **LCDwiki E32R35T** — 3.5″ ESP32-WROOM-32E display module, ST7796U 320 × 480 LCD over SPI, XPT2046 resistive touch.

External components:

- **MAX31855 K-type thermocouple module** (AliExpress carrier board) — read-only SPI device, **3.3 V supply**. The chip itself runs on 3.3 V; despite what some AliExpress listings say, this particular carrier board has **no onboard LDO**, so you must feed it regulated 3.3 V, not 5 V.
- **SSR-25DA** — zero-cross AC solid-state relay, DC control input (3–32 V).
- **Oven**: built-in element of a **Koenic KMO 4341** 20 L convection oven, 1500 W (specs in `dev_ressources/oven/specs.md`). Convection (Umluft) gives faster, more even thermal response than a purely radiant oven; expect different PID gains from the reference's defaults.
- 10 kΩ pull-down resistor on the SSR control line (kept inline with the SSR control wire).

Display orientation: **portrait — 320 wide × 480 tall**.

### 2.1 Pin map

The ESP32 has two general-purpose SPI hosts: **HSPI** (SPI2) with native pins SCLK=14 / MOSI=13 / MISO=12 and **VSPI** (SPI3) with native pins SCLK=18 / MOSI=23 / MISO=19. The board's onboard LCD + touch share HSPI. The MAX31855 uses VSPI on its own — the "SPI peripheral" header (P3) exposes VSPI's native pins (also shared with the onboard SD slot, which is unused in MVP, so the bus is fully free for the thermocouple).

| Function | ESP32 pin | Notes |
|---|---|---|
| LCD CS / DC / SCK / MOSI / MISO | IO15 / IO2 / IO14 / IO13 / IO12 | Onboard, fixed by board. **HSPI host** (native pins). |
| LCD backlight | IO27 | High = on. |
| XPT2046 touch CS | IO33 | Shares HSPI bus with LCD; coordinated by `esp_lcd`. |
| XPT2046 touch IRQ | IO36 | Input-only pin. Board provides a 10 kΩ pull-up (R32 in schematic) — no external pull-up needed. |
| MAX31855 CS | IO21 | "SPI peripheral" header (P3). |
| MAX31855 SCK | IO18 | Same header. **VSPI native SCLK**. |
| MAX31855 MISO | IO19 | Same header. **VSPI native MISO**. (MOSI / IO23 unused — MAX31855 is read-only.) |
| **SSR control** | **IO32** | "I²C peripheral" header (P4). Not a strapping pin. |
| RGB status LED (R/G/B) | IO22 / IO16 / IO17 | Onboard, low = on. Used as idle/running/fault indicator. |
| MicroSD | unused in MVP | Frees the SD slot's HSPI fan-out; no contention with MAX31855 (which is on VSPI). |

### 2.2 External wiring

**Cable layout**: the I²C peripheral header (P4) does double duty in MVP — it provides the 3.3 V rail and GND to the MAX31855 module **and** the SSR control + SSR GND. The SPI peripheral header (P3) carries only the four SPI signal lines (no power, no GND). This is not the most elegant layout — it shares one connector across two unrelated peripherals — but it's the only place on this board that exposes 3.3 V on a connector at all.

| From | To | Header | Notes |
|---|---|---|---|
| MAX31855 VCC | +3.3 V (VCC3V3 pad) | I²C peripheral header (P4) | The MAX31855 IC needs 3.3 V; the carrier has no LDO. |
| MAX31855 GND | GND pad | I²C peripheral header (P4) | Same GND pad shared with SSR DC− below. |
| MAX31855 CS | IO21 | SPI peripheral header (P3) | |
| MAX31855 SCK | IO18 | SPI peripheral header (P3) | |
| MAX31855 SO | IO19 | SPI peripheral header (P3) | MISO. |
| SSR DC+ | IO32 (IIC_SDA pad) | I²C peripheral header (P4) | |
| SSR DC− | GND pad | I²C peripheral header (P4) | Shared with MAX31855 GND. |
| 10 kΩ resistor | between SSR DC+ and GND | inline with control wire | Keeps SSR off during reset / unflashed boot / brown-out. With the SSR-25DA's ~3 V minimum control threshold, 10 kΩ to GND holds the input well below 0.4 V even with maximum ESP32 GPIO leakage. |

Do not connect mains-side AC wiring until after the SSR has been verified with a multimeter (see §11.2 step 6).

### 2.3 MAX31855 SPI parameters

- Mode 0 (CPOL = 0, CPHA = 0).
- MSB first.
- 32-bit reads (lower 32 bits of the chip's 32-bit data word).
- Clock ≤ 5 MHz (datasheet limit). 1–4 MHz is fine.
- CS active low; conversion takes ~70 ms — do not poll faster than that or you'll re-read the same conversion.

## 3. Software architecture

### 3.1 Modules

```
src/
├── main.c                 app_main; creates tasks, queues, snapshot
├── control/
│   ├── pid                standard-form PID + conditional-integration anti-windup  (host-testable)
│   ├── profile_curve      profile_t → setpoint(t)                                  (host-testable)
│   ├── reflow_sm          state machine; takes elapsed_ms as input                 (host-testable)
│   └── control_task       FreeRTOS task; ties pid/profile_curve/reflow_sm to drivers (NOT host-testable)
├── drivers/
│   ├── display            esp_lcd_st7796 panel setup
│   ├── touch              esp_lcd_touch_xpt2046 setup
│   ├── thermocouple       MAX31855 SPI driver; returns {temp_c, fault_flags, age_ms}
│   └── ssr                single GPIO + slow-PWM scheduler (no timer/ISR)
├── storage/
│   └── nvs_store          versioned read/write of profile + PID
└── ui/
    ├── ui_task            LVGL host
    ├── screen_home / _profile / _pid / _keypad
    └── theme              LVGL colors / fonts
include/
└── config.h               single source of truth — pins, timing, safety, defaults, bounds
test/                      Unity tests for host-testable modules (PIO `native` env)
├── test_pid/
├── test_profile_curve/
├── test_reflow_sm/
├── test_ssr_scheduler/
└── test_nvs_schema/
tools/
└── bringup/               one self-contained mini PIO project per §11.2 step (1–7)
    ├── 01_blink/
    ├── 02_display/
    ├── 03_touch/
    ├── 04_lvgl_hello/
    ├── 05_thermocouple/
    ├── 06_ssr/
    └── 07_safety/
```

The host-testable modules — `pid`, `profile_curve`, `reflow_sm`, plus the SSR scheduler logic in `drivers/ssr` and the schema serialisation in `storage/nvs_store` — are pure C with no ESP-IDF or FreeRTOS dependencies (NVS schema serialisation is just struct ↔ blob conversion; the actual flash I/O is wrapped behind a thin layer). `reflow_sm` takes elapsed time as a function parameter (not via `xTaskGetTickCount`) so the test harness can inject a fake clock. `control_task` is the only module under `control/` that imports ESP-IDF.

### 3.2 Tasks

- **`control_task`** — pinned to core 1, FreeRTOS priority 5, tick = `CONTROL_TICK_MS` (250 ms). Owns the SSR exclusively. No timer or ISR ever writes the SSR pin — only this task, only from step 6 of §5.1 (and the safety-trip force-off in steps 2-4).
- **`ui_task`** — runs on core 0, priority 2. Hosts LVGL. Translates touch events into queue messages.

### 3.3 Inter-task communication

Two primitives only:

- **`cmd_queue`** — FreeRTOS queue, depth 8. UI → Control. Message types:

  ```c
  typedef enum { CMD_START, CMD_STOP, CMD_ACK_FAULT,
                 CMD_APPLY_PROFILE, CMD_APPLY_PID } cmd_type_t;
  typedef struct {
      cmd_type_t type;
      union { profile_t profile; pid_gains_t pid; } payload;
  } cmd_msg_t;
  ```

- **`oven_state_t`** — single struct, mutex-protected, single writer (control_task) / multiple readers (ui_task). UI snapshots it on its LVGL tick; never holds the lock across LVGL work.

  ```c
  typedef enum { MODE_IDLE, MODE_RUNNING, MODE_FAULT } oven_mode_t;
  typedef enum { STAGE_NONE,        // paired with MODE_IDLE and MODE_FAULT
                 STAGE_HEAT_UP_1, STAGE_SOAK,
                 STAGE_HEAT_UP_2, STAGE_REFLOW,
                 STAGE_COOL_DOWN } stage_t;
  typedef struct {
      float       temp_c;
      float       setpoint_c;       // 0.0 in IDLE / FAULT / COOL_DOWN
      stage_t     stage;
      uint32_t    elapsed_ms;       // 0 outside RUNNING
      uint8_t     duty_percent;
      uint8_t     fault_flags;      // bitmask: open / scg / scv / stale / over_temp
      oven_mode_t mode;
      const float *curve;           // currently-published curve (planned profile)
      uint16_t     curve_len;
      uint32_t    phase_end_ms[STAGE_COUNT_];   // per-stage end times (ms from start)
      uint16_t    profile_applied_seq;          // bumped after nvs_store_save_profile
      uint16_t    pid_applied_seq;              // bumped after nvs_store_save_pid
  } oven_state_t;
  ```

`phase_end_ms[]` is a per-tick copy of the active `profile_curve_t`'s
end-of-stage marks. It exists so the UI can render the §7.2 stage chip
("Stage SOAK · 42 / 100 s") without dereferencing the curve pointer,
which has the buffer-swap caveat called out below.

`profile_applied_seq` and `pid_applied_seq` are monotonically
increasing counters bumped by `control_task` **after** the corresponding
NVS save succeeds (see §7.4 / §7.6 / §8). The UI watches them as a
one-way ACK channel — when the counter advances past a UI-remembered
baseline, the screens fire the appropriate "✓ saved" toast.

**Curve buffer ownership.** Two static buffers (`curve_a`, `curve_b`) live in `control/profile_curve`. **Only `control_task` writes `oven_state_t.curve`** — single-writer invariant has no exceptions. The control task generates a new curve into the *idle* buffer, then publishes by atomically swapping the pointer under the mutex. UI readers always see a consistent (curve, curve_len) pair.

UI never touches the SSR or the PID. Controller never touches LVGL.

(An earlier draft of this spec included a `CMD_PREVIEW_PROFILE` that swapped the active curve to a previewed-but-unsaved profile. It was removed on 2026-05-23 because the swap meant a subsequent START would run the previewed — not the saved — profile, with no UI indicator distinguishing the two. SAVE covers the same use case unambiguously: validate, apply, persist, and the home screen immediately reflects the new planned curve.)

### 3.4 Resource budget

ESP32-WROOM-32E has 520 KB SRAM (heap + system + drivers + LVGL all share this) and 4 MB external QSPI flash. Bluetooth is **permanently disabled** in `menuconfig` for this project (we have no use for it, ever). WiFi is off in MVP but in scope for v2; the budget below covers both cases.

**SRAM**:

| Allocation | MVP (WiFi off) | v2 (WiFi on) | Notes |
|---|---|---|---|
| LVGL display buffer (partial, double) | ~60 KB | ~60 KB | 320 × 48 px × 2 bytes × 2 buffers |
| LVGL widget memory pool | ~32 KB | ~32 KB | LVGL default, adjustable |
| LVGL font / theme cache | ~20 KB | ~20 KB | |
| Curve buffers (2 × 1500 × float) | ~12 KB | ~12 KB | A + B for atomic swap (§3.3). Size = `PROFILE_CURVE_MAX_SAMPLES` × 4 B × 2 buffers; raised from 1200 during Phase 2 to cover the longest profile under `PROFILE_CURVE_STEP_MS=500 ms`. |
| FreeRTOS task stacks (control + ui + system) | ~16 KB | ~16 KB | |
| ESP-IDF system overhead | ~50 KB | ~50 KB | |
| WiFi stack (LWIP + driver buffers) | — | ~50 KB | Adds with TLS/MQTT later (~+20 KB) |
| App data / BSS | ~10 KB | ~10 KB | |
| **Used** | **~200 KB** | **~250 KB** | |
| **Heap headroom** | **~320 KB** | **~270 KB** | Both comfortable |

**Critical assumption — partial LVGL display buffer, not full-screen.** A full-screen frame at 320 × 480 × 16 bpp is 307 KB; double-buffered full-screen would need 614 KB > total SRAM, so the design *cannot* use full-screen buffers on this MCU at all. We use the standard LVGL convention of a ~1/10-screen partial buffer with DMA flush (configured via `esp_lvgl_port`'s buffer-height setting, default ≈ 48 lines on portrait 320×480). This holds for both MVP and v2.

**Flash**:

| | MVP | v2 (WiFi + OTA) |
|---|---|---|
| ESP-IDF base | ~600 KB | ~600 KB |
| LVGL + drivers | ~250 KB | ~250 KB |
| Application | ~50 KB | ~80 KB (adds MQTT, OTA) |
| WiFi driver | — | ~80 KB |
| **Total app slot** | **~900 KB** | **~1010 KB** |
| Default factory partition (1.3 MB) | fits with headroom | fits |
| Dual-app OTA partition scheme | n/a | each slot ~1.5 MB in 4 MB flash, fits |

**CPU**: 240 MHz dual-core. Control loop is a few microseconds per 250 ms tick (< 0.1 % of one core). LVGL render at 30 FPS on a portrait 320 × 480 partial buffer is roughly 5–15 % of one core. `control_task` is pinned to core 1, `ui_task` to core 0, so they don't steal from each other; WiFi (when added in v2) runs on core 0 by default and shares headroom with the UI task — still well under capacity.

## 4. Control loop

### 4.1 PID

Standard form:

```
output = Kp · e + Ki · ∫e dt - Kd · d(temp)/dt
```

Note **derivative on measurement** (the negative `d(temp)/dt` term), not derivative on error. This avoids "derivative kick" at every curve sample boundary, since the curve has 500 ms resolution and the setpoint changes in steps.

Anti-windup is **conditional integration**: the integrator only updates if doing so wouldn't push deeper into saturation. `dt` is the fixed control period `CONTROL_TICK_MS / 1000` (0.25 s) — measured `dt` is a v2 refinement; for MVP nominal `dt` is fine since the FreeRTOS tick is deterministic.

Pseudocode:

```
error       = setpoint - temp
p_term      = Kp * error
d_term      = -Kd * (temp - prev_temp) / dt        // derivative on measurement
i_tentative = integrator + Ki * error * dt
raw         = p_term + i_tentative + d_term

if raw > 100 and error > 0:        // saturated high, error pushing higher
    output = 100
elif raw < 0 and error < 0:        // saturated low, error pushing lower
    output = 0
else:
    integrator = i_tentative
    output = clamp(raw, 0, 100)

prev_temp = temp
```

**First-tick initialisation** when the controller transitions IDLE → RUNNING: `integrator = 0`, `prev_temp = temp_c` (so derivative is 0 on the first tick). Same reset on STOP and on FAULT-acknowledgment.

**Live gain changes**: the controller exposes `pid_set_gains(kp, ki, kd)`. `APPLY_PID` calls it; the new gains take effect on the next tick. The integrator is **not** reset on a gain change (so live tuning during a run doesn't bump the output).

This avoids the reference's "zero Ki when saturated" trick (which mutates user-configured gains and is asymmetric — see `Reflow_Oven_STM32_V1_1/Core/Src/main.c`, lines 600-609; the reference sources are not redistributed, [reference_design.md](../../../dev_ressources/reference_design/reference_design.md) says where to get them).

### 4.2 Slow PWM for the SSR

- Window = `SSR_PWM_WINDOW_MS` (2000 ms). Tick = `CONTROL_TICK_MS` (250 ms). Quanta per window = 8 → duty resolution = 12.5 %.
- The SSR-25DA is zero-cross switching, so faster PWM gains nothing; 2 s windows are the conventional reflow choice.
- Each tick, the SSR scheduler:

  ```
  slot          = (now_ms / CONTROL_TICK_MS) % 8     // 0..7
  on_threshold  = (duty_percent * 8 + 50) / 100      // rounds to nearest, range 0..8
  pin           = (slot < on_threshold) ? HIGH : LOW
  ```

  The `+ 50` rounds to nearest so duty=99 → 8 (full ON), not 7. With this, duty 0/100 are exact and intermediate duties quantise to {12.5, 25, 37.5, 50, 62.5, 75, 87.5} %.

- **Mid-window changes**: because the threshold is recomputed per tick, the SSR can flip on→off→on within a single 2 s window if the PID output drops sharply mid-window. Acceptable for the SSR-25DA (zero-cross switching means each transition still happens at a mains crossing) and harmless for an oven's thermal mass.

### 4.3 Setpoint source

A precomputed reflow curve at `PROFILE_CURVE_STEP_MS` (500 ms) resolution, regenerated whenever the profile changes. Sized to the actual computed length (~600–1200 entries), not a fixed `[4000]` like the reference. Lookup with clamping:

```c
size_t idx = MIN(elapsed_ms / PROFILE_CURVE_STEP_MS, curve_len - 1);
float setpoint = curve[idx];
```

Out-of-bounds reads can't happen even if `elapsed_ms` overshoots `phase_end_ms[REFLOW]` by a tick (e.g. a late stage advance).

## 5. Reflow state machine

```
                      START                                        STOP
            ┌──────────────────┐                       ┌────────────────────────┐
            │                  ▼                       ▼                        │
         IDLE ──> HEAT_UP_1 ──> SOAK ──> HEAT_UP_2 ──> REFLOW ──> COOL_DOWN ──> IDLE
            ▲                                                          │
            │                                              (temp < COOL_DOWN_THRESHOLD_C
            │                                               OR cool-down timeout)
            │   ACK_FAULT
            └────────────────────────── FAULT ◄── any state, on safety trip
```

Stage transitions are precomputed when the curve is generated: each non-cool-down stage has a known end time `phase_end_ms[stage]`, stored in a `phase_end_ms[STAGE_COUNT]` array maintained by `profile_curve` and read by `reflow_sm`. Triggers:

- `HEAT_UP_1` → `SOAK` at `phase_end_ms[HEAT_UP_1]`
- `SOAK` → `HEAT_UP_2` at `phase_end_ms[SOAK]`
- `HEAT_UP_2` → `REFLOW` at `phase_end_ms[HEAT_UP_2]`
- `REFLOW` → `COOL_DOWN` at `phase_end_ms[REFLOW]`
- `COOL_DOWN` → `IDLE` when `temp_c < COOL_DOWN_THRESHOLD_C` **or** `cool_down_elapsed_ms > COOL_DOWN_TIMEOUT_MS` (so a stuck-hot oven still returns to a usable IDLE eventually; a "still hot" warning persists in the UI in that case).

`COOL_DOWN` is a **passive** stage: no PID, no curve lookup, SSR off, oven cools naturally. The state exists so the UI can keep showing "COOL DOWN" until the oven is genuinely safe to declare idle, instead of jumping to "ready to start" while still hot.

`STOP` from any `RUNNING.*` stage → `IDLE`, SSR forced off (immediate, not a passive cool-down — the user wanted to stop *now*).
`FAULT` is sticky — only `ACK_FAULT` returns to `IDLE`. If the underlying fault condition still holds, the next tick will re-trip; the UI will see a brief flash of IDLE then back to FAULT. This is by design.

### 5.1 Control tick order

Every `control_task` iteration runs these steps in order, no exceptions:

1. Read MAX31855 → `{temp_c, fault_flags, age_ms}`.
   - **First tick after boot** is special: there is no prior reading, so the driver returns `age_ms = 0` for the very first call and a freshly-read sample. If the first read fails (SPI error), the driver returns `fault_flags = OPEN` so step 2 trips FAULT — safer than booting straight into a closed-loop run with no temperature.
2. If `fault_flags != 0` → trip `FAULT(reason)`, force SSR off, jump to step 7.
3. If `age_ms > SENSOR_MAX_AGE_MS` → trip `FAULT(stale)`, force SSR off, jump to step 7.
4. If `temp_c > OVER_TEMP_LIMIT_C` → trip `FAULT(over_temp)`, force SSR off, jump to step 7.
5. Drain `cmd_queue` of **every** queued message this tick (single-shot reads in a loop until empty). All `APPLY_*` messages are applied in arrival order — work per command is bounded (single profile-curve regeneration in the low-millisecond range) so the worst-case full-queue drain stays well inside the 250 ms tick. State-machine events (`START`, `STOP`, `ACK_FAULT`) are batched: only the **most recently received** SM event is forwarded to `sm_step` ("last button press wins"); earlier SM events from the same tick are discarded. Acceptance still depends on the current mode at the time the message is dequeued:
   - `MODE_FAULT` accepts only `ACK_FAULT`; everything else (including `APPLY_PID`) is dropped.
   - `MODE_IDLE` accepts `START`, `APPLY_PROFILE`, `APPLY_PID`.
   - `MODE_RUNNING` (any stage including `COOL_DOWN`) accepts `STOP` and `APPLY_PID` (live PID tuning is useful). `APPLY_PROFILE` is dropped during a run — the curve must not change mid-cycle.

   Note (post-review): `CMD_APPLY_PROFILE` and `CMD_APPLY_PID` now invoke
   `nvs_store_save_*` synchronously inside the drain, so this step can
   take up to ~500 ms in the worst case (flash erase + program). That's
   well inside the 2 s task-WDT bound (§6.1 item 5), but it does mean
   single-tick determinism cannot be assumed when an APPLY message is
   queued. Mitigations: `APPLY_PROFILE` is rejected outside `MODE_IDLE`
   (already required above), and `APPLY_PID` only persists when
   `mode == MODE_IDLE` — the gains are applied immediately in
   `MODE_RUNNING` but the NVS write defers until the operator next
   commits while idle.
6. Process the current stage:
   - `HEAT_UP_1` / `SOAK` / `HEAT_UP_2` / `REFLOW`: look up setpoint from curve at `elapsed_ms`; run PID; hand duty to SSR scheduler. Advance stage if `elapsed_ms >= phase_end_ms[stage]`.
   - `COOL_DOWN`: SSR off, PID skipped; transition to `IDLE` per §5 triggers.
   - `IDLE` or `FAULT`: SSR off (defensive — also covered by §6.1 item 6 and steps 2–4).
7. Acquire `oven_state_t` mutex briefly; publish snapshot.
8. Feed task watchdog.

The safety gates (2-4) run *before* command processing, so an in-flight `START` cannot override an active fault.

**STOP latency.** A STOP touch arrives via `cmd_queue` and is processed in step 5 of the *next* tick — worst case `CONTROL_TICK_MS` (250 ms) until the SSR turns off. Acceptable for oven control; documented here so it's not a surprise.

## 6. Safety

### 6.1 Firmware

1. `control_task` runs at higher FreeRTOS priority than `ui_task`. UI work cannot delay a control tick.
2. **Single writer for the SSR.** Only `control_task`'s step 6 (and the force-off in steps 2-4) calls the SSR driver. No timer or ISR ever writes the SSR pin. UI's STOP is a queued command — no UI bypass.
3. Safety gates (§5.1 steps 2-4) run first each tick, before PID or command processing.
4. **`FAULT` is latched.** Auto-recovery during a run is not safe. Home shows the fault reason; START is replaced by an ACKNOWLEDGE button which sends `CMD_ACK_FAULT`.
5. **Task watchdog (TWDT)** registered on `control_task`, configured with `CONFIG_ESP_TASK_WDT_TIMEOUT_S = 2`. If the task stalls — including during a panic, a hard fault, or an infinite loop — the WDT fires within ≤2 s and reboots the chip. After reboot, items 6 and §6.2's external pull-down keep the SSR off until firmware re-initialises GPIO. **No panic-time GPIO override is implemented**; the worst-case unintended-ON window is therefore the WDT timeout (see §6.2 for the resulting overshoot estimate).
6. **SSR pin boots LOW.** GPIO is configured as output-low *before* the slow-PWM scheduler is initialised. The `esp_register_shutdown_handler` callback also drives the pin low on graceful restart (`esp_restart()`).
7. **Brown-out detector enabled** at the highest sensible threshold (ESP-IDF `CONFIG_ESP32_BROWNOUT_DET_LVL=7` or equivalent for the board's supply). On brown-out the chip resets cleanly into the safe boot state of item 6 rather than running with marginal voltages.

### 6.2 Hardware

- **External 10 kΩ pull-down on the SSR control line.** Between power-on-reset and the firmware reaching §6.1 item 6 (≈300 ms worst case: bootloader + early app_main), the GPIO is high-Z. The pull-down keeps the SSR off across that window, across brown-outs, and on unflashed boards.
- **SSR pin is not a strapping pin** (IO32 — see §2.1). Strapping pins (0, 2, 5, 12, 15) influence boot mode and can glitch high during reset.
- **Optional external thermal cutoff in series with the heater.** A one-shot mechanical fuse rated above peak reflow temperature is the conventional last line of defence. **The MVP build does not require it**, but its absence has consequences worth being explicit about:

  Without the cutoff, the only failure mode that can drive the heater past `OVER_TEMP_LIMIT_C` is a firmware panic during a high-duty SSR slot. Worst case: the SSR is full-on when `control_task` panics; firmware safety stops running; the WDT takes ≤2 s (§6.1 item 5) to reboot the chip; for those ≤2 s the heater dumps full power. For the Koenic KMO 4341 (1500 W convection, 20 L cavity, thermal mass on the order of 3 kJ/°C of air + steel), 1500 W × 2 s = 3000 J → roughly **~5 °C overshoot** beyond the temperature at the panic moment. Combined with the 10 °C buffer between `REFLOW_TEMP_MAX = 250` and `OVER_TEMP_LIMIT_C = 260`, a panic during reflow peak still leaves headroom before reaching the firmware cutoff (which has no effect during the panic window itself, but bounds the *next* run after reboot).

  Acceptable for an attended hobby build. Not acceptable for unattended operation — if you ever leave the oven running unattended, fit the thermal cutoff.

## 7. UI

### 7.1 Navigation

Persistent **tab bar at the bottom of the screen**, three tabs: **Home / Profile / PID**. A single tap switches tabs; there is no per-screen back button.

### 7.2 Home screen

Top-to-bottom layout:

- Big temperature read-out (orange when running, dimmed when idle)
- Target / status line: "Target 183 °C · elapsed 3:18" when running; "Ambient · ready to start" when idle; the fault reason when in `FAULT`.
- Stage indicator chip: "Stage SOAK · 42 / 100 s" when running.
- Profile graph (taking the remaining vertical space):
  - Planned curve in grey
  - Live measured trace in orange (during a run)
  - Fault state shows the curve dimmed
- Bottom action button (full-width, large touch target):
  - `MODE_IDLE` → green **START**
  - `MODE_RUNNING` → red **STOP**
  - `MODE_FAULT` → amber **ACKNOWLEDGE**

### 7.3 Profile screen

Single column, five stage cards stacked: **Heat-up 1** (Rate), **Soak** (Temp + Time), **Heat-up 2** (Rate), **Reflow** (Temp + Time), **Cool-down** (Rate).
Each card has a title bar and one or two sub-rows. Every sub-row uses the same layout: `<label> [value chip] <unit>`, all on one line. The value chip (small panel, brighter border, Montserrat 20 in accent orange) is the visual focus; the whole sub-row is the tap target. Tap opens the keypad popup (§7.5).
Soak and Reflow are dual-value cards with two independently tappable sub-rows.
The cool-down rate is a tunable property of the profile (default 1.0 °C/s, bounds in `config.h` as `COOL_DOWN_RATE_MIN`/`MAX`); `profile_curve_generate` extends the planned curve past reflow down to ambient at this rate so the planned line includes the cool-down tail.

Action row at the bottom: **RESET** (factory defaults), **SAVE** (validates and writes to NVS). The earlier-draft PREVIEW button was removed — see §3.3.

### 7.4 PID screen

Three rows: **Kp** / **Ki** / **Kd**, each with a description and a tap-to-edit value.
Per-field commit on OK. In `MODE_IDLE`, `control_task` re-validates, applies the gains, calls `nvs_store_save_pid()`, then bumps `oven_state_t.pid_applied_seq` — the UI fires its "✓ saved" toast on that seq advance. In `MODE_RUNNING` ("live tune"), gains are applied immediately but the NVS write is deferred until the next idle commit to keep the control tick deterministic (see §5.1 step 5); the toast will not fire on live-tune commits.

### 7.5 Numeric keypad popup

Modal anchored to the bottom of the screen, above the tab bar. Header shows field name + unit + valid range, e.g. *"Soak Temperature - C - 50 to 200"* (ASCII-only — the configured Montserrat subset isn't guaranteed to carry `·` / `–` / `°`). 4×4 button grid: 0–9, decimal point, ⌫, CLR, ESC, OK. OK clamps to `_MIN`/`_MAX` from `config.h`.

### 7.6 UI → command map

| Screen / state | User action | Effect |
|---|---|---|
| Home (idle) | START | `cmd_queue.send(CMD_START)` |
| Home (running) | STOP | `cmd_queue.send(CMD_STOP)` |
| Home (fault) | ACKNOWLEDGE | `cmd_queue.send(CMD_ACK_FAULT)` |
| Profile | tap value → keypad → OK | update local `profile_buf` (no save yet) |
| Profile | RESET | overwrite `profile_buf` with built-in defaults |
| Profile | SAVE (idle only — disabled while RUNNING) | `cmd_queue.send(CMD_APPLY_PROFILE)`. `control_task` re-validates, applies, calls `nvs_store_save_profile()`, then bumps `profile_applied_seq` — that bump is the UI's ACK channel; the SAVE toast fires off it. The home screen's planned curve updates on the next refresh tick, so SAVE doubles as the "preview" path. |
| PID | tap value → keypad → OK | `cmd_queue.send(CMD_APPLY_PID)`. `control_task` re-validates, applies, and (only in `MODE_IDLE`) calls `nvs_store_save_pid()` + bumps `pid_applied_seq`. UI fires "✓ saved" off the seq advance. |

NVS persistence is performed by `control_task` (sole writer) after its own `*_validate` check succeeds. The UI never writes NVS; it enqueues `CMD_APPLY_*` and consumes the seq-counter ACK in the published snapshot. This collapses validate-apply-persist into one critical section and removes the need for a UI←controller ACK queue.

## 8. Persistence (NVS)

Namespace: `reflow`. Two records:

```c
typedef struct {
    uint8_t  version;          // PROFILE_SCHEMA_VERSION (=2)
    float    first_heatup_rate; // °C/s
    uint16_t soak_temp;        // °C
    uint16_t soak_time;        // s
    float    second_heatup_rate;
    uint16_t reflow_temp;
    uint16_t reflow_time;
    float    cool_down_rate;   // °C/s; v2 addition
} profile_v2_t;

typedef struct {
    uint8_t version;           // PID_SCHEMA_VERSION
    float   kp, ki, kd;
} pid_v1_t;
```

- **Read at boot.** ESP-IDF NVS stores entries with internal CRC32; a corrupted entry surfaces as `ESP_ERR_NVS_INVALID_LENGTH` or similar from `nvs_get_blob`. On any read error (key missing, CRC fail, version mismatch, length mismatch), load built-in defaults from `config.h`. No explicit application-level CRC needed.
- **Writes happen only on explicit user action**: Profile SAVE, or PID per-field commit. The writer is always `control_task` (sole-writer ownership; see §3.3 + §7.6); the UI signals intent via `CMD_APPLY_*` and consumes the post-save ACK from `oven_state_t.*_applied_seq`. PID commits performed during `MODE_RUNNING` apply the gains immediately but defer the NVS write to the next `MODE_IDLE` commit, so flash I/O never lengthens a control tick mid-run. At a few writes per session, NVS endurance is irrelevant.
- Versioned headers leave room to migrate (the v1 → v2 bump landed 2026-05-23 adding `cool_down_rate`; reads of v1 fall back to defaults).
- A power loss between `nvs_set_blob` and `nvs_commit` simply leaves the previous record intact (NVS is journalling). A power loss between the controller's APPLY_PID acknowledgment and the UI's NVS write is possible but bounded — at worst the next boot loads the previous gains, which is still a valid set the user once chose.

## 9. Configuration & defaults

All compile-time constants live in `include/config.h`. This is the only file with knobs; it covers pin assignments, timing, safety limits, default values, parameter bounds, and NVS keys. Every `_MIN` / `_MAX` is also imported by the keypad popup so the UI's clamp limits and the firmware's bounds can never drift apart.

Cross-field validity is checked at Profile → SAVE: `REFLOW_TEMP > SOAK_TEMP`, both heat-up rates > 0, and **at least 10 °C** of headroom between `REFLOW_TEMP` and `OVER_TEMP_LIMIT_C` (`REFLOW_TEMP <= OVER_TEMP_LIMIT_C - 10 °C`) so a profile can never run intentionally above the safety cutoff. An invalid combination shows an error toast and is not written to NVS. A `static_assert(REFLOW_TEMP_MAX <= OVER_TEMP_LIMIT_C - 10.0f)` in `config.h` enforces the same bound at compile time. With the current defaults (`REFLOW_TEMP_MAX=250`, `OVER_TEMP_LIMIT_C=260`) the headroom is exactly 10 °C; tighten either constant if a future profile family needs strictly-more.

Selected values (all live in `config.h`; rationale included in inline comments there):

| Key | Value | Tunable? | Rationale |
|---|---|---|---|
| `CONTROL_TICK_MS` | 250 | Yes (100–1000) | Finer than reference's 500 ms, more SSR resolution without straining MCU. |
| `SSR_PWM_WINDOW_MS` | 2000 | Yes (1000–5000) | 8 quanta at 250 ms tick = 12.5 % duty resolution. |
| `PROFILE_CURVE_STEP_MS` | 500 | Yes (250–1000) | Memory: ~1200 entries for a 10-min profile. |
| `OVER_TEMP_LIMIT_C` | 260.0 | Limited (230–280) | 10 °C above max allowed `REFLOW_TEMP_MAX` (250); below substrate damage (~280). |
| `SENSOR_MAX_AGE_MS` | 1500 | Yes (>tick, <5000) | Six missed 250 ms reads — generous, but avoids spurious trips on transient SPI hiccups. |
| `COOL_DOWN_THRESHOLD_C` | 50.0 | Yes (30–80) | Well above ambient (so the trigger is reliable) and below "still painful to touch". |
| `COOL_DOWN_TIMEOUT_MS` | 600000 | Yes | 10 min upper bound; oven that hasn't cooled in 10 min has bigger problems. |
| `KP` / `KI` / `KD` defaults | 10 / 10 / 0 | Yes (per §4.1 bounds) | Match the reference's defaults. |
| Profile defaults | 1.2 °C/s · 150 °C / 90 s · 1.0 °C/s · 220 °C / 45 s | Yes | Generic SAC305-ish; user re-tunes for their solder paste. |

Pin assignments and safety limits are **not** intended to be tuned during normal use — they're board-specific and physics-specific respectively. Other knobs (PID gains, profile defaults, timing) are expected to be tuned during bring-up and ongoing use.

## 10. Project structure

```
reflow_oven/
├── platformio.ini
├── include/
│   └── config.h                  // §9
├── src/
│   ├── main.c
│   ├── control/{pid,profile_curve,reflow_sm,control_task}.{c,h}
│   ├── drivers/{display,touch,thermocouple,ssr}.{c,h}
│   ├── storage/nvs_store.{c,h}
│   └── ui/{ui_task,screen_home,screen_profile,screen_pid,screen_keypad,theme}.{c,h}
├── test/
│   ├── test_pid/
│   ├── test_profile_curve/
│   ├── test_reflow_sm/
│   ├── test_ssr_scheduler/
│   └── test_nvs_schema/
├── tools/
│   └── bringup/                  // one self-contained mini-PIO project per §11.2 step 1–7
│       ├── 01_blink/             // main.c + platformio.ini, #includes ../../include/config.h
│       ├── 02_display/
│       ├── 03_touch/
│       ├── 04_lvgl_hello/
│       ├── 05_thermocouple/
│       ├── 06_ssr/
│       └── 07_safety/
├── docs/superpowers/specs/
│   └── 2026-05-15-reflow-oven-mvp-design.md
└── dev_ressources/               // existing — datasheets, reference design, oven specs
```

Sketch of `platformio.ini`:

```ini
[platformio]
default_envs = esp32dev

[env:esp32dev]
platform = espressif32
board = esp32dev
framework = espidf
monitor_speed = 115200
; IDF-managed components are declared in src/idf_component.yml and pulled
; by the IDF Component Manager at CMake-configure time. PlatformIO's
; lib_deps can't reach components.espressif.com, so it's left empty here.

[env:native]
platform = native
test_framework = unity
test_filter = test_*
test_build_src = yes
; Host-side TDD: exclude IDF-only modules (drivers/, control_task,
; nvs_store, main.c) from the native build — their headers don't exist
; on the host toolchain.
build_src_filter =
    +<*>
    -<main.c>
    -<drivers/thermocouple.c>
    -<drivers/ssr.c>
    -<drivers/display.c>
    -<drivers/touch.c>
    -<storage/nvs_store.c>
    -<control/control_task.c>
```

The IDF-managed components used are: `lvgl/lvgl ^9`, `espressif/esp_lvgl_port ^2`, `espressif/esp_lcd_st7796 ^1`, and `atanisoft/esp_lcd_touch_xpt2046 ^1`. **Note: the XPT2046 touch driver lives under the `atanisoft/` namespace on components.espressif.com — Espressif does not publish their own XPT2046 driver.** The generic `esp_lcd_panel_io` and `esp_lcd_panel_ops` they all depend on do ship with IDF.

## 11. Testing

### 11.1 Host-tested (PlatformIO `native` env)

The three pure-C modules under `control/` (`pid`, `profile_curve`, `reflow_sm`) plus the SSR scheduler logic and NVS schema serialisation are host-testable. Tests run on the developer machine via `pio test -e native` using Unity:

- `test_pid` — anti-windup behavior on saturation (clamp + integrator freeze on the saturated side); recovery when error reverses; derivative response is on measurement, not error (no kick on setpoint step); `pid_set_gains` takes effect on next call without resetting integrator; first-tick behavior with `prev_temp = temp` produces zero d_term.
- `test_profile_curve` — known profile inputs produce expected setpoints at sampled times; curve length matches sum of stage durations; `phase_end_ms[]` array is monotonic; cross-field validation rejects invalid profiles (REFLOW_TEMP ≤ SOAK_TEMP, rate ≤ 0, REFLOW_TEMP > OVER_TEMP_LIMIT_C - 10).
- `test_reflow_sm` — given an injected clock, stages advance on the right `phase_end_ms` boundaries; `STAGE_NONE` only paired with `MODE_IDLE` / `MODE_FAULT`; FAULT is sticky until `ACK_FAULT`; STOP from any RUNNING stage returns to IDLE; COOL_DOWN exits on either temp threshold or timeout.
- `test_ssr_scheduler` — duty 0 → always OFF; duty 100 → always ON; duty 50 → 50 % of slots ON; rounding behavior matches §4.2 formula; mid-window duty change reflects on the next tick.
- `test_nvs_schema` — round-trip serialise/deserialise; version mismatch falls through to defaults; truncated blob falls through to defaults.

### 11.2 Hardware bring-up checklist

Manual, not automated. Run in this order on the assembled board. Each step 1–7 has a self-contained mini-PIO project under `tools/bringup/0N_<name>/` (see §10) that flashes a stub `main.c` exercising only that step's hardware. Each stub `#include`s the main project's `include/config.h` so pin assignments stay consistent. Steps 8–9 run on the full firmware build.

1. **Blink** onboard RGB LED (`tools/bringup/01_blink/`) — confirms ESP-IDF + PlatformIO + flashing pipeline.
2. **`esp_lcd_st7796` "Hello world"** rectangle (`tools/bringup/02_display/`) — confirms display SPI + DC/CS/RST wiring + portrait orientation.
3. **Touch coordinates** logged to UART (`tools/bringup/03_touch/`) — confirms XPT2046 + IRQ handling.
4. **Empty LVGL screen** with theme applied (`tools/bringup/04_lvgl_hello/`) — confirms `esp_lvgl_port` integration.
5. **Thermocouple read loop** logging °C and fault bits to UART (`tools/bringup/05_thermocouple/`) — confirms MAX31855 + SPI peripheral header (P3) wiring + 3.3 V supply (from P4) + VSPI host configuration.
6. **SSR bring-up with the heater disconnected** (`tools/bringup/06_ssr/`) — toggle IO32, verify with multimeter that SSR's input LED responds; confirm pull-down keeps the input below ~0.4 V when GPIO is low or high-Z; confirm IO32 + pull-down + SSR DC wiring.
7. **Safety chain dry run** (`tools/bringup/07_safety/`) — pull the thermocouple wire mid-loop and observe FAULT latch + SSR stays off; restore and confirm ACK_FAULT returns to IDLE.
8. **First closed-loop hold** at low setpoint (~60 °C, no profile) — confirms PID + slow-PWM end-to-end.
9. **First full reflow cycle** at the reference's defaults, finally with the heater connected.

## 12. Out of scope (v2 backlog)

- Buzzer at stage transitions and on completion (board has FM8002E + IO26/IO4 wired).
- Persistent emergency-stop button on every screen.
- Multiple named profile presets (NVS-backed list + selector screen).
- WiFi / OTA / MQTT (board has WiFi onboard).
- SD-card profile import / export (would re-enable the SD slot and would require multiplexing HSPI between SD and the LCD, since they sit on the same bus).

# Audio Signals — Design

Date: 2026-05-23
Status: Approved — ready for implementation plan

## 1. Scope

Add audible feedback to the reflow oven controller for three control-side events: **stage transitions**, **run completion**, and **fault conditions**. A user-controllable mute lives on the Home screen and persists across reboot.

This change promotes the "buzzer feedback" item from §1 of the MVP design (deferred) into MVP scope, and expands it to cover the fault-alarm case that was not in the original deferred bullet. Other items still deferred (persistent e-stop, multi-profile, WiFi, SD) are unaffected.

**External hardware delta**: a new Q-8R 1W 23 mm 8 Ω speaker (see `dev_ressources/peripherals/peripherals.md`) plugged into the board's onboard 1.25 mm 2P speaker connector. No other wiring changes.

Out of scope (deferred to v2):

- Fine volume control (slider). Mute-only for MVP.

## 2. Hardware

The LCDwiki E32R35T board has an onboard **FM8002E** 2 W class-AB audio power amplifier (datasheet not redistributed; see `dev_ressources/board_ressources/board_reference.md`). Per the board specification PDF and the schematic, the amp's input network is wired to two ESP32 GPIOs:

| Signal | ESP32 pin | Role |
|---|---|---|
| `AUDIO_IN` | **IO26** | Amp +IN. IO26 is also the ESP32's **DAC channel 2** — we drive the analog input directly from the on-chip DAC. |
| `AUDIO_EN` | **IO4** | Amp `SHUTDOWN` pin. **Active low: drive 0 to enable the amp, 1 to put it in shutdown (~4 µA idle).** |

The speaker plugs into the board's 1.25 mm 2P "SPEAKER" connector — no soldering required. The FM8002E is rated 1.1 W @ 8 Ω (THD < 1 %), which matches the Q-8R's 1 W power handling and 8 Ω impedance.

There is no LCDwiki-supplied example for audio in their ESP-IDF demo; the wiring above is recovered from the schematic (`3.5inch_ESP32-32E_E32R35T_Schematic.pdf`) and the specification table (`E32R35T_E32N35T_Specification_V1.0.pdf`), both linked from `dev_ressources/board_ressources/board_reference.md`.

### 2.1 New entries in `include/config.h`

```c
#define PIN_AUDIO_DAC      26    /* DAC2 → FM8002E +IN. Board-fixed. */
#define PIN_AUDIO_EN        4    /* Active-low FM8002E SHUTDOWN. Board-fixed. */

#define AUDIO_FAULT_LOOP_COUNT    15   /* Fault alarm cycles before auto-stop. */
#define AUDIO_FAULT_GAP_MS      1500   /* Silence between fault-cycle bursts. */
/* 15 cycles * (~540 ms burst + 1500 ms gap) ≈ 30 s self-bounded alarm. */

#define NVS_KEY_MUTE          "mute"   /* NVS key for the mute flag. */
```

`PIN_AUDIO_EN = 4` is a strapping pin on the ESP32. It is sampled at reset (high = SDIO-slave timing variant, which we don't use) but free to repurpose as a regular GPIO afterwards. The reset value with the on-board pull-up keeps the amp shut down during boot, which is what we want.

## 3. Software architecture

### 3.1 Module layout

```
src/
├── audio/
│   ├── audio_task.{c,h}     FreeRTOS task; consumes events, plays melodies
│   ├── audio_events.{c,h}   pure transition-detection helper (host-testable)
│   └── audio_melodies.h     compile-time melody tables
├── drivers/
│   └── audio.{c,h}          DAC + AUDIO_EN driver, tone primitives
```

The split keeps the hardware-touching code (`drivers/audio.c`) separate from the policy logic (`audio_task.c`), and isolates the transition-detection logic so it can be unit-tested under the existing host-test framework.

### 3.2 Driver layer (`drivers/audio.{c,h}`)

```c
void audio_init(void);
void audio_set_amp_enabled(bool on);                   /* drives IO4 */
void audio_play_tone(uint32_t freq_hz, uint32_t ms);   /* blocking */
void audio_silence(uint32_t ms);                       /* blocking, amp stays on */
```

Tone generation uses the **ESP32 DAC cosine waveform (CW) generator**: a hardware sinewave source on DAC2 with programmable frequency (~130 Hz – several kHz), zero CPU cost. `audio_play_tone` configures the CW generator, blocks `vTaskDelay(ms)`, then disables it. Blocking is fine: only the audio task ever calls this driver.

`audio_set_amp_enabled(true)` drives IO4 low; `false` drives IO4 high. Melody playback enables the amp once at the start of a sequence and disables it at the end — this avoids the audible click of toggling the amp between consecutive notes.

At init, DAC2 outputs mid-scale (0x80) so that enabling the amp later doesn't snap from rail-to-rail and pop the speaker.

### 3.3 Audio task (`audio_task.{c,h}`)

```c
typedef enum {
    AUDIO_EVT_STAGE_TRANSITION,
    AUDIO_EVT_RUN_COMPLETE,
    AUDIO_EVT_FAULT,
    AUDIO_EVT_FAULT_ACK,    /* stops the fault loop early */
} audio_event_t;

extern QueueHandle_t g_audio_queue;          /* depth 4, audio_event_t */

void audio_task_start(void);                 /* creates queue + task */
void audio_set_muted(bool muted);            /* updates internal atomic + writes NVS */
bool audio_is_muted(void);
```

The mute state itself is `static atomic_bool s_muted` inside `audio_task.c` — accessed only through the `audio_set_muted` / `audio_is_muted` helpers. Keeping the variable file-private avoids a second writer outside the audio module ever desyncing it from NVS.

- **Priority**: 1 (below `control_task` and `ui_task`).
- **Stack**: 4 KB.
- **Loop**: `xQueueReceive(portMAX_DELAY)` → check `audio_is_muted()` → dispatch to melody routine → return to top.
- **Mute check is per-event, not per-tone**: an in-flight fault alarm continues to play notes even if the user mutes mid-sequence. The next event after mute will be silent. This is a deliberate choice: mid-melody truncation would sound broken, and the alarm is short anyway (~540 ms per cycle).
- **Fault loop early-exit**: the fault melody loop polls the queue between cycles with `xQueuePeek(timeout=0)` for an `AUDIO_EVT_FAULT_ACK`; if present, drains it and breaks the loop.

### 3.4 Event source (`audio_events.{c,h}` + `control_task` hook)

Pure function in `audio_events.c`, host-testable:

```c
typedef struct {
    oven_mode_t mode;
    stage_t     stage;
    uint8_t     fault_flags;
} audio_state_snapshot_t;

/* Emits 0..N events into out[] based on the diff. Returns event count. */
size_t audio_detect_events(audio_state_snapshot_t prev,
                           audio_state_snapshot_t curr,
                           audio_event_t out[4]);
```

Rules (all driven by transitions, never by level):

| Transition | Event |
|---|---|
| `curr.stage != prev.stage` and `curr.stage` is one of `HEAT_UP_1 / SOAK / HEAT_UP_2 / REFLOW / COOL_DOWN` | `STAGE_TRANSITION` |
| `prev.mode == MODE_RUNNING` and `curr.mode == MODE_IDLE` (cool-down completed normally) | `RUN_COMPLETE` |
| `prev.fault_flags == 0` and `curr.fault_flags != 0` | `FAULT` |
| `prev.mode == MODE_FAULT` and `curr.mode != MODE_FAULT` (user acked) | `FAULT_ACK` |

The first rule includes the run-start transition (`STAGE_NONE → HEAT_UP_1`), so tapping START emits a stage-transition pip as audible confirmation that the run has begun.

**Priority rule**: when a fault arises on the same tick that the stage advances (e.g. over-temp trip exactly at the SOAK→HEAT_UP_2 boundary), only the `FAULT` event is emitted — the stage-transition pip is suppressed for that tick. This prevents a "happy ping" from playing immediately before the alarm.

`control_task` keeps a `audio_state_snapshot_t s_prev_audio_state` (file-static), and after each `sm_step` calls `audio_detect_events(s_prev_audio_state, curr, out)` then `xQueueSend(g_audio_queue, ..., 0)` for each emitted event (timeout zero — drop on overflow; audio is non-critical and the 4-deep queue is plenty for a single-tick burst).

### 3.5 Melodies (`audio_melodies.h`)

Each event has a static table of `{freq_hz, on_ms, gap_after_ms}` triples played in order.

| Event | Sequence | Burst length |
|---|---|---|
| `STAGE_TRANSITION` | `{1200, 80, 0}` | 80 ms |
| `RUN_COMPLETE` | `{523, 120, 40} {659, 120, 40} {784, 120, 0}` (C5–E5–G5 rising) | 480 ms |
| `FAULT` (one burst) | `{880, 100, 80} {880, 100, 80} {880, 100, 0}` | 540 ms |

Fault loop: the audio task plays the `FAULT` burst, then `vTaskDelay(AUDIO_FAULT_GAP_MS)` between bursts (peeking the queue during the gap for an early `FAULT_ACK`). 15 bursts × (540 ms + 1500 ms) ≈ 30 s total — audibly "beep-beep-beep … (pause) … beep-beep-beep …" until acknowledged or the cap is hit.

There is no separate "mute-toggle confirmation" melody table: the UI re-uses the `STAGE_TRANSITION` event for unmute confirmation (80 ms pip).

All frequencies sit between 500 Hz and 1.2 kHz — well above the speaker datasheet's 100–150 Hz roll-off, comfortably inside the FM8002E's flat region.

## 4. UI changes

Single addition: a **speaker icon button** on the Home screen.

- **Location**: top-right of the Home tab, around `(x=280, y=0)`, 28×28 px tap target.
- **Glyphs**: LVGL's built-in `LV_SYMBOL_VOLUME_MAX` (unmuted) and `LV_SYMBOL_MUTE` (muted).
- **Behaviour**: tap toggles `g_audio_muted`, redraws the icon, calls `nvs_store_set_mute(new_state)`.
- **Mute-on action**: icon swaps to `LV_SYMBOL_MUTE`; no sound. (Muting silently is the obvious-correct UX.)
- **Mute-off action**: icon swaps to `LV_SYMBOL_VOLUME_MAX`; an audible confirmation pip plays. Implemented by queuing a `STAGE_TRANSITION` event (re-uses the 80 ms pip melody).
- **Initial state**: read from NVS in `nvs_store_init`; default unmuted if key missing.

No other screen gains an icon — the mute state is global, so one entry point is enough. Profile and PID screens are unchanged.

## 5. Persistence

New NVS key:

- `NVS_KEY_MUTE` (`"mute"`) — single `uint8_t`, 0 = unmuted, 1 = muted.

Stored under the existing `NVS_NAMESPACE` (`"reflow"`). Two new functions in `storage/nvs_store.{c,h}`:

```c
bool nvs_store_get_mute(void);            /* false on missing key */
void nvs_store_set_mute(bool muted);      /* fire-and-forget write */
```

Neither participates in the schema versioning of the existing profile/PID blobs — a single byte under its own key needs no migration story.

## 6. Boot sequence integration

1. `app_main` calls `audio_init()` before any task starts. This puts IO4 high (amp off) and DAC2 at mid-scale.
2. `nvs_store_init()` reads `mute` key into `g_audio_muted`.
3. `audio_task_start()` creates queue + task.
4. `control_task_start()` and `ui_task_start()` follow as today.

Boot is silent. The amp stays in shutdown until the first event fires.

## 7. Testing

### 7.1 Host-testable (under `test/`)

- `test_audio_events.c` — exhaustive snapshot-diff cases for `audio_detect_events`:
  - No-op (same snapshot in/out) → 0 events.
  - Mid-run stage advance → 1 `STAGE_TRANSITION`.
  - START transition (`STAGE_NONE → HEAT_UP_1`) → 1 `STAGE_TRANSITION`.
  - Cool-down completion (`RUNNING → IDLE` via `STAGE_COOL_DOWN`) → 1 `RUN_COMPLETE`.
  - Fault rising edge (`fault_flags 0 → 0x01`) → 1 `FAULT`.
  - Fault clear (`MODE_FAULT → MODE_IDLE`) → 1 `FAULT_ACK`.
  - Simultaneous fault + stage change in one tick → only `FAULT` (fault takes priority; stage emission suppressed when fault is fresh).
- `test_audio_melodies.c` — runtime sanity over the static melody tables: every `freq_hz ∈ [400, 4000]`, every `on_ms > 0`, every burst length below 1 s.

### 7.2 Hardware-only (manual bring-up: `tools/bringup/09_audio/`)

A standalone bring-up project that:

- Initialises only the audio driver and a UART CLI.
- Accepts single-char commands over USB-serial: `s` = stage pip, `c` = completion chime, `f` = fault alarm (one cycle), `F` = full alarm (`AUDIO_FAULT_LOOP_COUNT` cycles), `m` = toggle mute, `t [freq] [ms]` = arbitrary tone.
- Lets the user confirm by ear that:
  - Tones are audible without distortion.
  - The amp click-on / click-off is inaudible.
  - DAC mid-scale rest produces no idle hiss when AUDIO_EN is low.

In-system validation (against full firmware):

- Boot with mute off; trigger over-temp by spoofing the thermocouple driver to return 300 °C; confirm `FAULT` melody fires and self-stops after `AUDIO_FAULT_LOOP_COUNT` (15) bursts (~30 s total).
- Acknowledge the fault via the UI; confirm the loop terminates within one cycle.
- Toggle mute, reboot, verify mute state persists.

## 8. Risks and open questions

1. **IO4 state at reset.** ESP32 GPIO4 defaults to floating-input at reset (no internal pull). With the FM8002E `SHUTDOWN` pin floating, the amp's state during the ~100–300 ms between power-on and `audio_init()` is undefined. DAC2 is also floating until init, so even if the amp briefly enables, the input is at unknown DC and may pop. Mitigation: `audio_init()` runs at the top of `app_main` *before* any LVGL or driver init, minimising the window. To be confirmed by oscilloscope on the speaker terminals during bring-up; if pops are audible, add an external pull-up on IO4 to the +5 V rail (FM8002E SHUTDOWN is referenced to VDD).
2. **DAC CW generator coexistence with WiFi.** WiFi is not used in MVP, but if v2 enables it the DAC CW generator shares some clock infrastructure with the RF subsystem. Re-validate when WiFi lands.
3. **Speaker connector polarity.** The 1.25 mm 2P "SPEAKER" header has SP+ and SP- pins. Reversing them inverts the audio phase but does not damage anything — confirm orientation during first power-on.

## 9. v2 backlog updates

- Move the "Buzzer feedback" item from the originally-deferred MVP list to a "Done in MVP via the 2026-05-23 audio-signals change" footnote.
- Promote "Volume slider" as a new v2-backlog entry under UX / quality of life: deferred specifically here, paired with the existing audio module.
- The "RGB LED status" v2 item pairs naturally with this: both are out-of-screen state indicators driven by the same control-task transitions. When implementing v2 RGB-LED, consider sharing the `audio_detect_events` helper to drive both.

## 10. Files changed

New:

- `src/drivers/audio.{c,h}`
- `src/audio/audio_task.{c,h}`
- `src/audio/audio_events.{c,h}`
- `src/audio/audio_melodies.h`
- `test/test_audio_events.c`
- `test/test_audio_melodies.c`
- `tools/bringup/09_audio/` (project root + `platformio.ini` + `src/main.c`)

Modified:

- `include/config.h` — add `PIN_AUDIO_DAC`, `PIN_AUDIO_EN`, `AUDIO_FAULT_LOOP_COUNT`, `NVS_KEY_MUTE`.
- `src/main.c` — call `audio_init()` + `audio_task_start()`.
- `src/control/control_task.c` — keep `s_prev_audio_state`, call `audio_detect_events`, post to `g_audio_queue`.
- `src/storage/nvs_store.{c,h}` — add `_get_mute` / `_set_mute`.
- `src/ui/screen_home.{c,h}` — add speaker icon button, mute-toggle handler.
- `docs/superpowers/specs/2026-05-15-reflow-oven-mvp-design.md` — remove "Buzzer feedback" from §1 deferred list, add a back-reference to this spec.
- `CMakeLists.txt` — register `src/audio` and `src/drivers/audio.c` in the build.

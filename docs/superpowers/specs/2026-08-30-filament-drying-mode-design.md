# Filament drying mode — design spec

**Date:** 2026-08-30
**Status:** approved (brainstorming session) — implementation pending
**Motivation:** Reuse the reflow oven as a filament dryer. Drying is a long
low-temperature hold (40–100 °C for 6–12 h), which the reflow machinery cannot
express: `SOAK_TIME_MAX` is 300 s, a 6 h curve at `PROFILE_CURVE_STEP_MS` would
need ~86 KB against 37.4 KiB of free static DRAM, and `OVER_TEMP_LIMIT_C` =
260 °C is not a meaningful guard for a 45 °C PLA dry. Drying is therefore a
peer run mode alongside the step test, not a new profile.

## 0. Constraints that shaped this design

Measured on the current build (`esp32dev-ota`), and binding on every decision
below:

| Resource | State |
|---|---|
| Flash, per OTA slot | 1,452,144 / 2,031,616 B used — **566 KiB free**, chip fully allocated (`ota_1` ends at 0x400000) |
| Static DRAM (`dram0_0_seg`) | 142,432 / 180,736 B — **78.8 % used, 37.4 KiB free**; overflow fails the link |
| LVGL pool | 65,536 B static (`CONFIG_LV_MEM_SIZE_KILOBYTES=64`); screens are created at boot and never destroyed, so every new screen costs pool permanently |
| Heap | Already fragmented by LVGL frame buffers + WiFi — see `fce8ef5`, which added `control_task_prealloc()` after a ~12 KB allocation failed at boot |

**Consequence:** drying adds **no new buffers**. The setpoint is computed
analytically, the graph reuses `s_plan_pts` / `s_live_pts`, and the run display
reuses HOME rather than adding a second graph screen.

## 1. State machine (`reflow_sm`)

Mirrors the step-test pattern (`sm_start_step_test` / `sm_step_test_tick`).

`oven_mode_t` gains `MODE_DRYING`. New types:

```c
typedef enum { DRY_STAGE_NONE, DRY_STAGE_HEAT_UP, DRY_STAGE_HOLD } dry_stage_t;

typedef enum {
    DRY_END_NONE = 0,
    DRY_END_COMPLETE,      /* hold timer expired — normal */
    DRY_END_MANUAL_STOP,
    DRY_END_HEATUP_TIMEOUT, /* never reached the band — see 1.2 */
    DRY_END_FAULT,
} dry_end_reason_t;
```

`sm_state_t` gains:

| field | meaning |
|---|---|
| `dry_target_c` | requested hold temperature |
| `dry_hold_ms` | requested hold duration |
| `dry_hold_elapsed_ms` | time accrued **in `DRY_STAGE_HOLD` only** |
| `dry_start_temp_c` | oven temperature captured at START; ramp origin |
| `dry_stage` | `dry_stage_t` |
| `dry_end_reason` | `dry_end_reason_t` |

`elapsed_ms` continues to mean total run time (ramp + hold).

```c
bool sm_start_dry(sm_state_t *s, float target_c, uint32_t hold_ms, float temp_c);
bool sm_dry_tick(sm_state_t *s, float temp_c, float *setpoint_out);
```

`sm_start_dry` accepts only from `MODE_IDLE` with `!ota_reserved` and
`validation == SM_VALIDATION_NONE` (same gates as the step test), clamps
`target_c` to `[DRY_TEMP_MIN_C, DRY_TEMP_MAX_C]` and `hold_ms` to
`[DRY_HOLD_MIN_MS, DRY_DURATION_MAX_MS]`, and additionally requires
`temp_c <= target_c + DRY_START_MARGIN_C` (§4.3).

`sm_dry_tick` returns whether heating is permitted this tick and writes the
setpoint. The caller runs all safety gates **before** it, exactly as for the
step test; `sm_trip_fault` records `DRY_END_FAULT` when tripping out of
`MODE_DRYING`.

### 1.1 Analytic setpoint

```
setpoint(t) = min(dry_start_temp_c + DRY_RAMP_RATE_C_S · t_seconds,
                  dry_target_c)
```

Two floats and a multiply — no array at any duration. This is what dissolves
the memory wall, and it feeds the existing feedforward unmodified: `rate` is
`DRY_RAMP_RATE_C_S` in `DRY_STAGE_HEAT_UP` and `0` in `DRY_STAGE_HOLD`, so
`ff = k_hold·(setpoint − ambient_ref) + k_rate·rate` needs no change.

A ramped (not stepped) setpoint is required because overshoot is the primary
hazard: stepping 25 → 45 °C hands the PID a 20 °C error and invites exactly the
excursion that takes PLA through its glass transition.

**Ramp-rate rationale.** Overshoot after a ramp flattens is approximately
`rate × L`, with L = 32.5 s measured by the 2026-07-19 step test. At
0.05 °C/s that is ~1.6 °C, comfortably inside the 10 °C guard of §4.1. The
worst-case ramp (25 → 100 °C) is 25 minutes, negligible against a 6–12 h hold —
drying has no time pressure, which is what buys this margin.

A setpoint "leash" (clamping the setpoint to `temp_c + lead`) was designed and
**rejected**: at 0.05 °C/s the oven can follow the ramp under any realistic
load (1 kg PLA needs ~90 W against ~500 W available at the §2.2 duty cap), so
the leash never binds; and because it pins the error at a constant value it
would wind the integrator up (~25 % duty over a 12.5-min ramp) and discharge it
as the overshoot it was meant to prevent. The slower ramp removes the problem
rather than adding a mechanism to correct it.

### 1.2 Stage transitions

| from | to | condition |
|---|---|---|
| — | `DRY_STAGE_HEAT_UP` | `sm_start_dry` accepted |
| `HEAT_UP` | `HOLD` | `temp_c >= dry_target_c − DRY_HOLD_BAND_C` |
| `HEAT_UP` | end (`DRY_END_HEATUP_TIMEOUT`) | `elapsed_ms >= DRY_HEATUP_TIMEOUT_MS` without reaching the band |
| `HOLD` | end (`DRY_END_COMPLETE`) | `dry_hold_elapsed_ms >= dry_hold_ms` |
| any | end (`DRY_END_MANUAL_STOP`) | `SM_EVENT_CMD_STOP` |
| any | `MODE_FAULT` (`DRY_END_FAULT`) | any safety gate |

The hold timer runs **only** in `HOLD`. A 30-minute ramp must not consume 30
minutes of a 6-hour dry: "6 h at 80 °C" means six hours *at* 80 °C.

The transition is one-way. Once in `HOLD` the timer keeps running even if the
temperature briefly leaves the band, so a transient dip cannot extend the run
indefinitely.

`DRY_HEATUP_TIMEOUT_MS` is the backstop for a target the oven cannot reach — a
failing element, a door left open, an over-large load. Without it a run that
never enters the band would heat indefinitely, since the hold timer never
starts. The longest legitimate ramp (25 → 100 °C at 0.05 °C/s) is 25 minutes,
so 90 minutes leaves generous room for a heavy load while still bounding the
failure. Precedent: `COOL_DOWN_TIMEOUT_MS` is the same kind of stage backstop.

### 1.3 Completion

Timer expires → SSR off → completion tone (reuse the existing `RUN_COMPLETE`
audio event) → `MODE_IDLE`. **No cool-down stage:** at 40–100 °C the oven is at
or below `COOL_DOWN_THRESHOLD_C` (80 °C) already, so a reflow-style cool-down
would either terminate instantly or add a state that does nothing. Passive
cooling needs no code.

**No auto-resume after power loss.** A reboot mid-dry lands in `MODE_IDLE`,
matching reflow. A power blip silently ends an overnight dry; the alternative —
a device that spontaneously resumes heating unattended after a power event — is
worse.

### 1.4 `control_task` integration and IPC

Dispatch mirrors the step test: `control_task` runs every safety gate first,
then branches on `mode`. `MODE_DRYING` calls `sm_dry_tick`, feeds the returned
setpoint to `pid_step` with `out_max = DRY_DUTY_MAX_PERCENT`, and drives the
SSR through the drying window (§2.2). `pid_reset(temp_c)` is called on the
IDLE → DRYING transition, exactly as for a reflow run, so `ambient_ref` is
captured for the feedforward hold term.

`run_id` is bumped on drying start, like every other run kind, so archives stay
distinguishable.

`cmd_type_t` gains `CMD_START_DRY`, carrying a new payload member:

```c
typedef struct { float target_c; uint32_t hold_ms; } dry_params_t;
```

`oven_state_t` gains the fields the UI needs, published every tick under the
existing mutex — the same set §7 exposes over HTTP:

```c
bool     dry_active;
uint8_t  dry_stage;             /* dry_stage_t, copied as raw */
float    dry_target_c;
uint32_t dry_hold_ms;
uint32_t dry_hold_elapsed_ms;
uint8_t  dry_end_reason;        /* dry_end_reason_t, copied as raw */
```

These are scalars only; the drying path publishes no pointer, so the
`curve` locking caveat in `oven_state.h` does not extend to it.

**Audio.** `audio_detect_events` keys off `stage_t`, which drying does not use.
Drying emits the existing `RUN_COMPLETE` event on `DRY_END_COMPLETE` and the
existing fault alarm via the normal fault path; it emits **no** per-stage
transition tone. A single `HEAT_UP → HOLD` chirp is the only candidate and is
deliberately omitted — an overnight dry should stay quiet.


## 2. SSR duty resolution (`ssr_scheduler`)

### 2.1 The defect this fixes

`SSR_QUANTA_PER_WINDOW` is `SSR_PWM_WINDOW_MS / CONTROL_TICK_MS` = 8, and
`ssr_scheduler_should_be_on` rounds to nearest:

```c
threshold = (duty * 8 + 50) / 100;    /* integer */
```

so **any duty ≤ 6 % rounds to zero quanta and the SSR never fires.** A 50 °C
hold needs `k_hold·(50 − 25) = 0.19 · 25 ≈ 4.75 %`, which is below that floor.
Uncorrected, a low-temperature hold cannot be controlled at all — it would
limit-cycle in 12.5 % bursts.

### 2.2 Change

```c
bool ssr_scheduler_should_be_on(uint32_t now_ms, uint8_t duty_percent,
                                uint16_t quanta_per_window);
```

`ssr.c` gains `ssr_set_window_quanta(uint16_t q)`, called by `control_task` on
mode entry. Reflow passes `SSR_QUANTA_PER_WINDOW` (8) and is bit-identical to
today. Drying passes `DRY_SSR_QUANTA` = 120 (a 30 s window at the 250 ms tick)
for 0.83 % resolution. Slower switching also reduces zero-cross SSR wear.

The window is *defined* by the parameter as `quanta_per_window ×
CONTROL_TICK_MS`, so the only constraint is `quanta_per_window ≥ 1`, which the
function asserts. `SSR_QUANTA_PER_WINDOW` remains the reflow value.

## 3. PID output ceiling (`pid`)

Drying caps duty at `DRY_DUTY_MAX_PERCENT` (50). The cap **must be visible to
the anti-windup**, or the integrator winds up against a ceiling it cannot see —
`pid_step`'s conditional integration currently tests `raw > PID_OUTPUT_MAX`
(100), which a 20–30 % drying duty never reaches.

```c
float pid_step(pid_state_t *s, float setpoint, float rate,
               float temp, float dt, float out_max);
```

Inside, `PID_OUTPUT_MAX` becomes `out_max` in both the saturation test and the
final clamp. Reflow passes `PID_OUTPUT_MAX` and is bit-identical. Drying passes
`DRY_DUTY_MAX_PERCENT`.

Worst-case drying demand is a 100 °C hold at `0.19 · 75 ≈ 14 %` plus
`71 · 0.05 ≈ 3.6 %` of ramp term ≈ 18 %, so 50 % leaves ample PID authority
while bounding how fast a software fault can develop.

## 4. Safety envelope

### 4.1 Drying over-temperature

Evaluated against the **target**, not the ramping setpoint, so the ramp cannot
nuisance-trip it:

```
temp_c > dry_target_c + DRY_OVER_TEMP_MARGIN_C   →   FAULT_OVER_TEMP
```

PLA at 45 °C trips at 55 °C, below the ~60 °C glass transition, so a welded SSR
or a controller fault does not deform the spool. ASA at 60 °C trips at 70 °C.
The global `OVER_TEMP_LIMIT_C` (260 °C) remains as the absolute backstop, and
the thermocouple fault / staleness gates apply unchanged because they run
*before* the mode dispatch — drying inherits them at no cost.

### 4.2 Electronics over-temperature (new, global)

`cj_temp_c` (MAX31855 cold junction) has been display-only since the About
screen shipped, whose stated purpose was observing a *26-minute* run. The
archives show why that is insufficient for drying:

| run | duration | oven max | cj |
|---|---|---|---|
| 4 × reflow | ~10 min | ~235 °C | +22 °C rise → 39–48 °C |
| `2026_07_21_hold_staircase` | 40 min | 238 °C | 33.3 → 60.1 °C, **still rising linearly at the end** |

**No run in the archive ever reached thermal equilibrium.** The oven vents
through the electronics compartment, so a 12 h dry will settle somewhere we
have not measured.

```c
#define CJ_LIMIT_C  75.0f
FAULT_ELECTRONICS_HOT = 1 << 5,
```

Applied **globally, in every mode**, not only drying: if the board that
switches mains reaches 75 °C, aborting is correct in any run. Normal reflow
peaks at 48 °C so it will not fire in practice; the 40-minute diagnostic
staircase reached 60 °C, the closest anything has come. ESP32-WROOM-32E is
rated to +85 °C ambient, leaving 10 °C of margin.

`fault_text()` gains a case; the fault clears via the existing ACK path.

### 4.3 Run gating

`RUN_START_MAX_TEMP_C` (50 °C) exists so every reflow run starts from a
comparable near-ambient state. That rationale does not apply to drying — the
setpoint is constant, there is no ramp profile to reproduce — and 50 °C sits
*inside* the drying range, which would block restarting or extending a PLA dry.

Drying instead requires `temp_c <= dry_target_c + DRY_START_MARGIN_C` (5 °C).
Starting hotter than the target is refused rather than silently waiting to cool,
so behaviour is predictable.

`DRY_START_MARGIN_C` **must stay below** `DRY_OVER_TEMP_MARGIN_C`, or a start
accepted at the gate would immediately trip the §4.1 fault. Asserted in §6.

Reflow gating is unchanged: after a 90 °C nylon dry a reflow start is refused
until the oven cools below 50 °C, which is existing behaviour working
correctly.

### 4.4 Unchanged

Task watchdog (`CONTROL_TASK_WDT_S` = 2 s) is indifferent to run length; the
control tick is still 250 ms. `elapsed_ms` as `uint32_t` holds 49 days.

## 5. Persistence (NVS)

Names are **compile-time**; only values are editable and persisted. Editable
names would require a text keyboard — the most expensive widget LVGL offers,
landing permanently in the 64 KB pool — for no control benefit.

```c
typedef struct __attribute__((packed)) {
    uint16_t temp_c;      /* DRY_TEMP_MIN_C … DRY_TEMP_MAX_C */
    uint16_t hold_min;    /* minutes */
} dry_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t     version;                    /* = 1 */
    dry_entry_t e[DRY_PRESET_COUNT];        /* 6 */
} dry_presets_v1_t;                         /* 25 bytes */
```

`dry_presets_to_blob` / `dry_presets_from_blob` / `dry_presets_validate`
mirror the profile and PID helpers. `dry_presets_validate` range-checks every
entry; a corrupt, wrong-version or out-of-range record falls back to the
compiled defaults rather than being partially trusted — same policy as
`pid_validate`, where an out-of-range gain is a thermal-runaway hazard.

A new NVS key `NVS_KEY_DRY_PRESETS`. The selected preset index is **not**
persisted: the DRY tab selects index 0 (PLA) on every boot, so an unattended
restart can never inherit a stale high-temperature selection.

## 6. `config.h` additions

```c
/* ===== FILAMENT DRYING ===== */
#define DRY_TEMP_MIN_C            40.0f
#define DRY_TEMP_MAX_C           100.0f
#define DRY_OVER_TEMP_MARGIN_C    10.0f   /* fault above target + this */
#define DRY_START_MARGIN_C         5.0f   /* refuse START above target + this */
#define DRY_HOLD_BAND_C            2.0f   /* HEAT_UP -> HOLD entry band */
#define DRY_RAMP_RATE_C_S          0.05f  /* see §1.1 — overshoot ~ rate x L */
#define DRY_DUTY_MAX_PERCENT      50.0f
#define DRY_SSR_QUANTA             120    /* 30 s window at CONTROL_TICK_MS */
#define DRY_HEATUP_TIMEOUT_MS  5400000u          /* 90 min, see 1.2 */
#define DRY_HOLD_MIN_MS          60000u          /* 1 min  */
#define DRY_DURATION_MAX_MS   57600000u          /* 16 h   */
#define DRY_PRESET_COUNT             6

#define CJ_LIMIT_C                75.0f   /* electronics; global (§4.2) */

_Static_assert(DRY_TEMP_MAX_C + DRY_OVER_TEMP_MARGIN_C <= OVER_TEMP_LIMIT_C,
               "drying trip point must stay below the absolute over-temp limit");
_Static_assert(DRY_TEMP_MIN_C < DRY_TEMP_MAX_C, "drying range inverted");
_Static_assert(DRY_START_MARGIN_C < DRY_OVER_TEMP_MARGIN_C,
               "a start accepted at the gate must not instantly over-temp");
_Static_assert(DRY_SSR_QUANTA >= 1, "SSR window needs at least one quantum");
```

Preset defaults:

| # | material | temp | hold |
|---|---|---|---|
| 0 | PLA | 45 °C | 6 h |
| 1 | PETG | 65 °C | 6 h |
| 2 | TPU | 70 °C | 8 h |
| 3 | ASA | 60 °C | 6 h |
| 4 | Nylon | 90 °C | 8 h |
| 5 | PC | 90 °C | 6 h |

## 7. HTTP API — telemetry v3

`TELEMETRY_API_VERSION` 2 → 3. `telemetry_view_t` gains:

| field | meaning |
|---|---|
| `dry_active` | `mode == MODE_DRYING` |
| `dry_stage` | resolved string: `heat_up` / `hold` |
| `dry_target_c` | target temperature |
| `dry_hold_ms` | requested hold duration |
| `dry_hold_elapsed_ms` | accrued hold time |
| `dry_end` | resolved end-reason string |

`mode` gains the `"drying"` value. `telemetry_json.c` stays pure and
host-testable.

**`POST /stop` is unchanged** — it already stops whatever is running.

**No start endpoint.** Step tests are touchscreen-only today and drying stays
the same: remotely starting a mains heater is a different safety proposition.
Web start/stop remains `v2_backlog` #7.

**`GET /profile` is unchanged.** Drying has no curve to stream; the page draws
its reference line from `dry_target_c` and `dry_hold_ms` in telemetry.

## 8. Touchscreen UI

### 8.1 A fourth tab

`HOME / PROFILE / DRY / ⚙`. DRY is structurally parallel to PROFILE — PROFILE
edits the reflow recipe, DRY edits the drying recipe and starts it. Four tabs
on 320 px is ~80 px each and ⚙ is a narrow icon; `v2_backlog` #5's cramping
concern was about four *word* labels.

Contents: a six-row list (material name, temperature, hold time), each row's
two values editable through the **existing `screen_keypad`**, and a START
button. START switches the tabview to HOME.

*Amended 2026-09-01 after bench testing (commit `41bf67b`).* As specified, one
tap selected a row and a second tap on the selected row opened the temperature
keypad, which then chained into hold-time entry so a single gesture edited
both. In use that was wrong in two ways: editing one field forced a decision
about the other, and an ESC out of the temperature keypad skipped the hold time
entirely. As built, the temperature and the hold time are **independently
tappable zones** within the row, each opening the keypad for its own field
only, and each persisting on its own. The hold-time keypad takes whole minutes
— the unit `hold_min` is stored in — so the value round-trips unchanged.

HOME's existing START remains reflow-only.

A rejected START (`sm_start_dry` returned false — oven not idle, OTA reserved,
pending validation, or too hot per §4.3) surfaces through the existing toast
mechanism rather than failing silently.

### 8.2 Drying runs render on HOME

The expensive component is reused rather than cloned — this is the RAM decision
that matters, since `screen_home.c.o` is already 25.8 KiB of static buffers
(`s_plan_pts` + `s_live_pts`, 1600 × 8 B each).

Three scaling changes, all constants in the existing mapping functions:

| | reflow | drying |
|---|---|---|
| Y axis | `0 → OVER_TEMP_LIMIT_C` (260 °C) | `0 → dry_target_c + 20 °C` |
| X axis | absolute elapsed, full run | **rolling 30-minute window** |
| live trace cadence | 1 s | 2 s (900 samples = 30 min) |

The rolling window is the right display for a constant-temperature hold: a
6-hour dry plotted whole is a flat line carrying no information, whereas the
recent window shows ripple and stability. `s_live_pts` is already a ring
buffer, so 900 of its 1600 slots serve the window with headroom.

The planned-curve overlay collapses to a horizontal line at the target, plus
the ramp segment while still in view — 3 points in `s_plan_pts`.

Total run progress lives on the stage chip: `DRY · HEAT-UP`, then
`DRY · HOLD 2:14 / 6:00`.

### 8.3 Fault surface

`FAULT_ELECTRONICS_HOT` renders through the existing fault line and ACK flow.

## 9. Web page and run-file format

`app.js` gains a drying view. The browser has no RAM limit, so unlike the
device it keeps the **full** run history and plots the whole dry.

Run archives gain `kind: "dry"` alongside the existing `"reflow"` / `"step"`,
with `dry_target_c` and `dry_hold_ms` in the head. This matters more than the
display: it is how a long dry lands in `dev_ressources/oven/` and becomes
analysable, which is what will answer the open `cj_temp_c` question in §11.

`verifyRun` / `computeRunMetrics` are reflow-specific and are skipped for
`kind: "dry"`; drying metrics are hold-band deviation (mean, max) and time to
reach band.

## 10. Testing

Host-side, under `pio test -e native`:

| suite | coverage |
|---|---|
| `test_reflow_sm` | start gating (idle only, `ota_reserved`, validation, `temp <= target + margin`), clamping of target and duration, `HEAT_UP → HOLD` at band entry, timer accrues **only** in HOLD, completion, manual stop, fault transitions, one-way stage transition |
| `test_ssr_scheduler` | quanta = 8 reproduces current behaviour exactly; quanta = 120 gives non-zero on-time for 1–6 % duty (the §2.1 defect); rounding at both ends |
| `test_pid` | `out_max` = 100 is bit-identical to today; `out_max` = 50 freezes the integrator at 50 rather than 100 |
| `test_nvs_schema` | preset blob round-trip, out-of-range and wrong-version rejection, fallback to defaults |
| `test_telemetry_json` | v3 fields present, drying and non-drying shapes |
| **new** `test_dry_setpoint` | `min(start + 0.05·t, target)`, including start-above-target and the ramp/hold rate handoff |

No new bring-up firmware — there is no new hardware.

## 11. Required manual verification

**Done 2026-09-05.** `CJ_LIMIT_C = 75 °C` is now measured-and-justified rather
than reasoned. Source run: a 4 h ASA dry at 80 °C on firmware `d728738`,
12,675 samples, no `joinedLate` gap —
[`dev_ressources/oven/2026_09_05_dry_run_ASA.json`](../../../dev_ressources/oven/2026_09_05_dry_run_ASA.json).

| | |
|---|---|
| Ambient at start (`cj_c` on a cold board) | 22.4 °C |
| `cj_c` at the end of the 4 h hold | **38.8 °C** peak, 37.9 °C mean over the last 30 min |
| Ripple over the last 30 min | 1.6 °C peak-to-peak |
| Still rising at the end? | **yes** — +0.9 °C/h through hours 2–4 |
| Margin to `CJ_LIMIT_C` at cut-off | 36 °C |
| Hold quality (unrelated, recorded while here) | mean deviation 0.04 °C, max 3.25 °C, 1062 s to band |

**Steady state was not reached, so the asymptote is extrapolated, and the two
methods disagree** — the thermal time constant is longer than the 4 h window,
which makes the fit ill-conditioned:

- least-squares first-order fit: **46.2 °C**, τ ≈ 20 h (RMSE 0.58 °C)
- regressing d`cj`/d*t* against `cj`: **42.1 °C**, τ ≈ 5.3 h

Take the range 42–46 °C. Either way the limit has **≥ 29 °C of margin at
80 °C**, so it is not close, and neither the limit nor the venting needs
revisiting.

**The 90 °C case is extrapolated.** `cj` rise scales with the oven-to-ambient
difference; this run gives a coefficient of 0.34–0.41. At 90 °C that predicts
45–50 °C in a 22 °C room and 50–55 °C in a 30 °C one — 20 °C or more below the
limit.

**Decision (2026-09-05, maintainer): this obligation is closed in full, including
the 90 °C case.** `CJ_LIMIT_C` will not be reached, on the evidence above plus
engineering judgement: every extrapolation lands in the 40s against a 75 °C
limit and an 85 °C part rating, and the two disagreeing fits bracket the answer
rather than straddle the limit. **No 90 °C verification dry is owed.** The
limit remains a protective cut-out, not a working margin — if it ever trips, it
is telling you something has changed (venting blocked, fan failed, ambient far
outside anything seen here), and that is a fault to investigate, not a number
to re-derive.

**Decision (2026-08-30, maintainer), kept for the record:** 75 °C stands as
shipped. It is an explicitly accepted estimate, not a placeholder awaiting a
number — the verification above is confirmatory and does not block the feature.
The ESP32 is rated to +85 °C, so an estimate that errs low costs an aborted dry,
never hardware. **The 2026-09-05 measurement confirms that decision.**

## Out of scope

- **A web plotting library.** Deferred as its own topic. Flash supports it
  (~15 KiB gzipped for uPlot against 566 KiB free, and the page costs flash not
  RAM), but the existing chart carries a bespoke drag-to-refine interaction for
  the Ziegler–Nichols tangent that would have to be rebuilt on the library's
  event model — unrelated to drying. Natural trigger: when long-run archives
  make the hand-rolled canvas redraw sluggish.
- **Starting a dry from the browser** — `v2_backlog` #7.
- **Humidity sensing.** No sensor, and temperature-and-time is what commercial
  filament dryers do.
- **Per-preset PID gains.** The global gains plus feedforward serve the whole
  40–100 °C range; `k_hold` is a steady-state coefficient, so a loaded spool
  changes the transient but not the hold duty.
- **Editable preset names** — §5.
- **Auto-resume after power loss** — §1.3.
- **A drying-specific cool-down stage** — §1.3.

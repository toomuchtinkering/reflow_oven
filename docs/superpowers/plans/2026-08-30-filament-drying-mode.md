# Filament Drying Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `MODE_DRYING` — a long low-temperature hold (40–100 °C for 6–12 h) that turns the reflow oven into a filament dryer.

**Architecture:** Drying is a peer run mode alongside the step test, not a new profile. The setpoint is computed analytically (`min(start + 0.05·t, target)`) so no curve array exists at any duration — the device has 37.4 KiB of free static DRAM and a 6 h curve would need ~86 KB. Two latent defects are fixed along the way: `ssr_scheduler` rounds any duty ≤ 6 % to zero (a 50 °C hold needs ~4.75 %), and `pid_step`'s anti-windup can't see an externally-applied duty cap. Runs render on the existing HOME graph rather than a new screen, because every LVGL screen permanently consumes the 64 KB pool.

**Tech Stack:** C11, ESP-IDF via PlatformIO, FreeRTOS, LVGL 9, Unity (host tests via `pio test -e native`).

**Spec:** [../specs/2026-08-30-filament-drying-mode-design.md](../specs/2026-08-30-filament-drying-mode-design.md)

---

## File structure

| File | Responsibility | Change |
|---|---|---|
| `include/config.h` | drying constants + static asserts | modify |
| `src/drivers/ssr_scheduler.h/.c` | slow-PWM quanta, now parameterised | modify |
| `src/drivers/ssr.h/.c` | active window selection | modify |
| `src/control/pid.h/.c` | output ceiling as parameter | modify |
| `src/control/reflow_sm.h/.c` | `MODE_DRYING`, drying stages, tick | modify |
| `src/control/dry_setpoint.h/.c` | **new** — analytic ramp, pure logic | create |
| `src/storage/nvs_schema.h/.c` | preset blob + validation | modify |
| `src/storage/nvs_store.h/.c` | preset load/save | modify |
| `src/oven_state.h` | drying IPC fields + `CMD_START_DRY` | modify |
| `src/control/control_task.c` | dispatch, cj gate, run_id, window | modify |
| `src/ota/telemetry_json.h/.c` | telemetry v3 drying fields | modify |
| `src/ota/ota_server.c` | populate the new view fields | modify |
| `src/ui/screen_home.c` | drying axes + rolling window | modify |
| `src/ui/screen_dry.h/.c` | **new** — DRY tab (preset list + START) | create |
| `src/ui/ui_task.c` | fourth tab, drying status wiring | modify |
| `src/web/app.js` | drying view + `kind: "dry"` archives | modify |

`dry_setpoint` is split out rather than living in `reflow_sm.c` so the ramp maths is testable in isolation and `reflow_sm.c` stays focused on state transitions.

**Run all host tests:** `pio test -e native`
**Run one suite:** `pio test -e native -f test_<name>`

---

### Task 1: Parameterise the SSR window

The bug: `threshold = (duty * 8 + 50) / 100` in integer maths means duty 1–6 % yields 0 quanta and the SSR never fires at all. Drying needs ~4.75 % for a 50 °C hold.

**Files:**
- Modify: `src/drivers/ssr_scheduler.h`, `src/drivers/ssr_scheduler.c`
- Modify: `src/drivers/ssr.h`, `src/drivers/ssr.c`
- Test: `test/test_ssr_scheduler/test_ssr_scheduler.c`

- [x] **Step 1: Write the failing tests**

Append to `test/test_ssr_scheduler/test_ssr_scheduler.c`, and add every existing
call an explicit `SSR_QUANTA_PER_WINDOW` third argument (the existing five tests
must keep asserting today's behaviour):

```c
/* The defect: at the reflow window, a 5 % duty never fires. */
void test_low_duty_is_lost_at_reflow_window(void) {
    int on = 0;
    for (uint32_t t = 0; t < SSR_PWM_WINDOW_MS; t += CONTROL_TICK_MS) {
        if (ssr_scheduler_should_be_on(t, 5, SSR_QUANTA_PER_WINDOW)) on++;
    }
    TEST_ASSERT_EQUAL_INT(0, on);
}

/* The fix: at the drying window, it fires. */
void test_low_duty_survives_at_drying_window(void) {
    int on = 0;
    const uint32_t window = (uint32_t)DRY_SSR_QUANTA * CONTROL_TICK_MS;
    for (uint32_t t = 0; t < window; t += CONTROL_TICK_MS) {
        if (ssr_scheduler_should_be_on(t, 5, DRY_SSR_QUANTA)) on++;
    }
    TEST_ASSERT_EQUAL_INT(6, on);   /* (5*120+50)/100 = 6 */
}

void test_drying_window_resolves_one_percent(void) {
    int on = 0;
    const uint32_t window = (uint32_t)DRY_SSR_QUANTA * CONTROL_TICK_MS;
    for (uint32_t t = 0; t < window; t += CONTROL_TICK_MS) {
        if (ssr_scheduler_should_be_on(t, 1, DRY_SSR_QUANTA)) on++;
    }
    TEST_ASSERT_EQUAL_INT(1, on);   /* (1*120+50)/100 = 170/100 = 1 (integer division) */
}

void test_quanta_one_is_all_or_nothing(void) {
    TEST_ASSERT_FALSE(ssr_scheduler_should_be_on(0, 49, 1));
    TEST_ASSERT_TRUE(ssr_scheduler_should_be_on(0, 51, 1));
}
```

Register them in `main()`:

```c
    RUN_TEST(test_low_duty_is_lost_at_reflow_window);
    RUN_TEST(test_low_duty_survives_at_drying_window);
    RUN_TEST(test_drying_window_resolves_one_percent);
    RUN_TEST(test_quanta_one_is_all_or_nothing);
```

- [x] **Step 2: Run to verify it fails**

Run: `pio test -e native -f test_ssr_scheduler`
Expected: compile error — `too many arguments to function 'ssr_scheduler_should_be_on'`, and `DRY_SSR_QUANTA` undeclared.

- [x] **Step 3: Add the config constant**

In `include/config.h`, immediately after the `SSR_QUANTA_PER_WINDOW` definition:

```c
/* Drying uses a much longer PWM window. At the reflow window (8 quanta) the
 * scheduler's round-to-nearest drops any duty <= 6 % to zero quanta, but a
 * 50 C hold needs ~4.75 %. 120 quanta = a 30 s window = 0.83 % resolution.
 * Slow switching also reduces zero-cross SSR wear. See drying spec 2.1. */
#define DRY_SSR_QUANTA           120
_Static_assert(DRY_SSR_QUANTA >= 1, "SSR window needs at least one quantum");
```

- [x] **Step 4: Change the scheduler**

`src/drivers/ssr_scheduler.h` — replace the declaration:

```c
/* `quanta_per_window` DEFINES the window as quanta_per_window * CONTROL_TICK_MS.
 * Reflow passes SSR_QUANTA_PER_WINDOW (8); drying passes DRY_SSR_QUANTA (120).
 * Must be >= 1. */
bool ssr_scheduler_should_be_on(uint32_t now_ms, uint8_t duty_percent,
                                uint16_t quanta_per_window);
```

`src/drivers/ssr_scheduler.c`:

```c
#include "drivers/ssr_scheduler.h"
#include "config.h"
#include <assert.h>

bool ssr_scheduler_should_be_on(uint32_t now_ms, uint8_t duty_percent,
                                uint16_t quanta_per_window) {
    assert(quanta_per_window >= 1);
    if (duty_percent >= 100) return true;
    if (duty_percent == 0)   return false;

    uint32_t slot = (now_ms / CONTROL_TICK_MS) % quanta_per_window;
    /* Round to nearest. */
    uint32_t threshold = ((uint32_t)duty_percent * quanta_per_window + 50) / 100;
    return slot < threshold;
}
```

- [x] **Step 5: Give the driver a selectable window**

`src/drivers/ssr.h` — add above `ssr_tick`:

```c
/* Select the PWM window used by subsequent ssr_tick calls. Set on mode entry
 * by control_task: SSR_QUANTA_PER_WINDOW for reflow/step test, DRY_SSR_QUANTA
 * for drying. Defaults to SSR_QUANTA_PER_WINDOW. */
void ssr_set_window_quanta(uint16_t quanta);
```

`src/drivers/ssr.c` — add the file-static and setter, and use it in `ssr_tick`:

```c
static const char *TAG = "ssr";

static uint16_t s_quanta = SSR_QUANTA_PER_WINDOW;

void ssr_set_window_quanta(uint16_t quanta) {
    if (quanta < 1) {
        /* Reject rather than coerce to the reflow window: that would silently
         * reinstate the defect this parameter exists to fix. Not an assert --
         * a programming error here does not warrant rebooting a mid-run oven. */
        ESP_LOGE(TAG, "ssr_set_window_quanta(0) ignored; keeping %u quanta", s_quanta);
        return;
    }
    s_quanta = quanta;
}

void ssr_tick(uint32_t now_ms, uint8_t duty_percent) {
    bool on = ssr_scheduler_should_be_on(now_ms, duty_percent, s_quanta);
    gpio_set_level(PIN_SSR, on ? 1 : 0);
}
```

- [x] **Step 6: Run tests**

Run: `pio test -e native -f test_ssr_scheduler`
Expected: PASS, 9 tests. The five pre-existing tests must still pass unchanged — that is the proof the reflow path is bit-identical.

- [x] **Step 7: Commit**

```bash
git add include/config.h src/drivers/ssr_scheduler.h src/drivers/ssr_scheduler.c \
        src/drivers/ssr.h src/drivers/ssr.c test/test_ssr_scheduler/test_ssr_scheduler.c
git commit -m "fix(ssr): parameterise the PWM window; low duties no longer vanish

At 8 quanta the round-to-nearest threshold drops any duty <= 6 % to zero, so
the SSR never fires. A 50 C drying hold needs ~4.75 %. The window becomes a
parameter: reflow keeps 8 (bit-identical), drying uses 120 for 0.83 %."
```

---

### Task 2: Make the PID output ceiling a parameter

Drying caps duty at 50 %. If that cap is applied *outside* `pid_step`, the anti-windup — which tests `raw > PID_OUTPUT_MAX` (100) — never sees it, and the integrator winds up against an invisible ceiling.

**Files:**
- Modify: `src/control/pid.h`, `src/control/pid.c`
- Modify: `src/control/control_task.c` (call site only)
- Test: `test/test_pid/test_pid.c`

- [x] **Step 1: Write the failing tests**

Append to `test/test_pid/test_pid.c`, and add `PID_OUTPUT_MAX` as the sixth
argument to every existing `pid_step` call:

```c
/* The ceiling must participate in anti-windup: the integrator has to stop
 * climbing when the OUTPUT saturates, and WHERE it stops must depend on
 * out_max. Gains are chosen so the integrator climbs over several ticks
 * (6.25/step) rather than saturating on the first call — with a large kp the
 * p_term alone exceeds the ceiling immediately, the integrator never leaves
 * 0, and the test passes vacuously even if out_max were ignored. */
static float run_to_saturation(float out_max) {
    pid_gains_t g = { .kp = 0.1f, .ki = 1.0f, .kd = 0.0f,
                      .k_hold = 0.0f, .k_rate = 0.0f };
    pid_state_t s;
    pid_init(&s, &g);
    pid_reset(&s, 25.0f);
    for (int i = 0; i < 20; i++) {
        float out = pid_step(&s, 50.0f, 0.0f, 25.0f, 0.25f, out_max);
        TEST_ASSERT_TRUE(out <= out_max + 1e-4f);
    }
    return s.integrator;
}

void test_integrator_freezes_at_custom_ceiling(void) {
    float i50 = run_to_saturation(50.0f);
    TEST_ASSERT_TRUE(i50 > 6.0f);                        /* actually integrated */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 43.75f, i50);        /* froze, didn't run away */
}

void test_freeze_point_tracks_the_ceiling(void) {
    /* The load-bearing proof: the same input sequence must freeze the
     * integrator LATER at a higher ceiling. Fails if out_max never reaches
     * the saturation test — both would freeze at the PID_OUTPUT_MAX point. */
    float i50  = run_to_saturation(50.0f);
    float i100 = run_to_saturation(100.0f);
    TEST_ASSERT_TRUE(i100 > i50);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 93.75f, i100);
}

/* Passing PID_OUTPUT_MAX must reproduce the previous behaviour exactly. */
void test_default_ceiling_matches_legacy(void) {
    pid_gains_t g = { .kp = 2.0f, .ki = 0.1f, .kd = 0.0f,
                      .k_hold = 0.0f, .k_rate = 0.0f };
    pid_state_t s;
    pid_init(&s, &g);
    pid_reset(&s, 25.0f);
    float out = 0.0f;
    for (int i = 0; i < 50; i++) {
        out = pid_step(&s, 100.0f, 0.0f, 25.0f, 0.25f, PID_OUTPUT_MAX);
    }
    TEST_ASSERT_TRUE(out <= PID_OUTPUT_MAX + 1e-4f);
    TEST_ASSERT_TRUE(out > 0.0f);
}
```

Register in `main()`:

```c
    RUN_TEST(test_integrator_freezes_at_custom_ceiling);
    RUN_TEST(test_freeze_point_tracks_the_ceiling);
    RUN_TEST(test_default_ceiling_matches_legacy);
```

- [x] **Step 2: Run to verify it fails**

Run: `pio test -e native -f test_pid`
Expected: compile error — `too many arguments to function 'pid_step'`.

- [x] **Step 3: Change the signature**

`src/control/pid.h` — replace the `pid_step` declaration:

```c
/* rate: commanded setpoint slope (°C/s), from profile_curve_rate() or the
 * drying ramp rate; negative values (cool-down) are treated as 0. dt must be
 * > 0 (asserted). out_max is the duty ceiling in percent — reflow passes
 * PID_OUTPUT_MAX, drying passes DRY_DUTY_MAX_PERCENT. It participates in the
 * anti-windup saturation test, so a cap applied by the caller instead would
 * wind the integrator up invisibly. */
float pid_step(pid_state_t *s, float setpoint, float rate, float temp,
               float dt, float out_max);
```

- [x] **Step 4: Change the implementation**

`src/control/pid.c` — in `pid_step`, replace the saturation block. Only
`PID_OUTPUT_MAX` becomes `out_max`; nothing else changes:

```c
    float output;
    if (raw > out_max && error > 0.0f) {
        output = out_max;               /* freeze integrator */
    } else if (raw < PID_OUTPUT_MIN && error < 0.0f) {
        output = PID_OUTPUT_MIN;        /* freeze integrator */
    } else {
        s->integrator = i_tentative;
        output = clamp(raw, PID_OUTPUT_MIN, out_max);
    }
```

- [x] **Step 5: Update the existing call site**

`src/control/control_task.c` — the reflow `pid_step` call becomes:

```c
            float out = pid_step(&s_pid, setpoint,
                                 profile_curve_rate(s_curve, s_sm.elapsed_ms),
                                 temp_c, dt, PID_OUTPUT_MAX);
```

- [x] **Step 6: Run tests**

Run: `pio test -e native`
Expected: PASS across all suites. `test_pid`'s pre-existing tests passing unchanged is the proof reflow behaviour is untouched.

- [x] **Step 7: Commit**

```bash
git add src/control/pid.h src/control/pid.c src/control/control_task.c \
        test/test_pid/test_pid.c
git commit -m "fix(pid): output ceiling as a parameter so anti-windup can see it

A duty cap applied outside pid_step is invisible to the conditional-integration
branch, which tests against PID_OUTPUT_MAX. Drying caps at 50 %, so the ceiling
becomes a parameter. Reflow passes PID_OUTPUT_MAX and is bit-identical."
```

---

### Task 3: Drying constants in `config.h`

**Files:**
- Modify: `include/config.h`

- [x] **Step 1: Add the drying block**

Append after the SAFETY LIMITS section (`DRY_SSR_QUANTA` was already added in
Task 1 next to `SSR_QUANTA_PER_WINDOW`; leave it there):

```c
/* ============================================================
 * FILAMENT DRYING (spec 2026-08-30)
 * ============================================================ */
#define DRY_TEMP_MIN_C            40.0f
#define DRY_TEMP_MAX_C           100.0f
/* Fault above target + this. 10 C keeps PLA at 45 C tripping at 55 C, below
 * its ~60 C glass transition, so a welded SSR does not deform the spool. */
#define DRY_OVER_TEMP_MARGIN_C    10.0f
/* Refuse START above target + this. Must stay below the trip margin or a
 * start accepted at the gate would immediately fault. */
#define DRY_START_MARGIN_C         5.0f
/* HEAT_UP -> HOLD entry band. */
#define DRY_HOLD_BAND_C            2.0f
/* Overshoot after a ramp flattens is approximately rate * L, with L = 32.5 s
 * from the 2026-07-19 step test. 0.05 C/s gives ~1.6 C, well inside the 10 C
 * trip. Worst-case ramp 25->100 C is 25 min, negligible against a 6-12 h hold. */
#define DRY_RAMP_RATE_C_S          0.05f
#define DRY_DUTY_MAX_PERCENT      50.0f
/* Backstop for a target the oven cannot reach (failed element, open door,
 * over-large load): without it a run that never enters the band would heat
 * indefinitely, since the hold timer never starts. Longest legitimate ramp is
 * 25 min. Same role as COOL_DOWN_TIMEOUT_MS. */
#define DRY_HEATUP_TIMEOUT_MS  5400000u          /* 90 min */
#define DRY_HOLD_MIN_MS          60000u          /* 1 min  */
#define DRY_DURATION_MAX_MS   57600000u          /* 16 h   */
#define DRY_PRESET_COUNT             6

/* Electronics (MAX31855 cold junction) limit, GLOBAL to every mode. The oven
 * vents through the electronics bay; the 2026-07-21 hold staircase reached
 * 60 C after 40 min and was still climbing linearly, and no archived run ever
 * reached equilibrium. ESP32-WROOM-32E is rated to +85 C ambient. */
#define CJ_LIMIT_C                75.0f

_Static_assert(DRY_TEMP_MAX_C + DRY_OVER_TEMP_MARGIN_C <= OVER_TEMP_LIMIT_C,
               "drying trip point must stay below the absolute over-temp limit");
_Static_assert(DRY_TEMP_MIN_C < DRY_TEMP_MAX_C, "drying range inverted");
_Static_assert(DRY_START_MARGIN_C < DRY_OVER_TEMP_MARGIN_C,
               "a start accepted at the gate must not instantly over-temp");
_Static_assert(DRY_HOLD_MIN_MS < DRY_DURATION_MAX_MS, "drying duration range inverted");

/* Compiled preset defaults. Names are compile-time; only the values are
 * editable and persisted (an editable-name UI needs a text keyboard, the most
 * expensive widget LVGL offers, permanently in the 64 KB pool). */
#define DRY_PRESET_DEFAULTS { \
    { "PLA",   45,  360 }, \
    { "PETG",  65,  360 }, \
    { "TPU",   70,  480 }, \
    { "ASA",   60,  360 }, \
    { "Nylon", 90,  480 }, \
    { "PC",    90,  360 }, \
}
```

- [x] **Step 2: Verify the asserts compile**

Run: `pio test -e native`
Expected: PASS. A `_Static_assert` failure would surface as a compile error here.

- [x] **Step 3: Commit**

```bash
git add include/config.h
git commit -m "feat(config): drying constants, limits and preset defaults"
```

---

### Task 4: The analytic drying setpoint

Split into its own unit so the ramp maths is testable without touching state-machine transitions.

**Files:**
- Create: `src/control/dry_setpoint.h`, `src/control/dry_setpoint.c`
- Test: `test/test_dry_setpoint/test_dry_setpoint.c`

- [x] **Step 1: Write the failing test**

Create `test/test_dry_setpoint/test_dry_setpoint.c`:

```c
#include <unity.h>
#include "control/dry_setpoint.h"
#include "config.h"

void setUp(void) {}
void tearDown(void) {}

void test_starts_at_start_temp(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, dry_setpoint(25.0f, 80.0f, 0u));
}

void test_ramps_at_configured_rate(void) {
    /* 100 s at 0.05 C/s = +5 C */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, dry_setpoint(25.0f, 80.0f, 100000u));
}

void test_clamps_at_target(void) {
    /* 25 -> 80 needs 1100 s; ask for 5000 s. */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 80.0f, dry_setpoint(25.0f, 80.0f, 5000000u));
}

void test_start_above_target_returns_target(void) {
    /* A warm oven must never produce a descending or above-target setpoint. */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 45.0f, dry_setpoint(48.0f, 45.0f, 0u));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 45.0f, dry_setpoint(48.0f, 45.0f, 10000u));
}

void test_rate_is_ramp_rate_then_zero(void) {
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, DRY_RAMP_RATE_C_S,
                             dry_setpoint_rate(25.0f, 80.0f, 100000u));
    /* Past the ramp, the setpoint is flat, so the feedforward rate term is 0. */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f,
                             dry_setpoint_rate(25.0f, 80.0f, 5000000u));
}

void test_rate_zero_when_starting_at_or_above_target(void) {
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, dry_setpoint_rate(80.0f, 80.0f, 0u));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_at_start_temp);
    RUN_TEST(test_ramps_at_configured_rate);
    RUN_TEST(test_clamps_at_target);
    RUN_TEST(test_start_above_target_returns_target);
    RUN_TEST(test_rate_is_ramp_rate_then_zero);
    RUN_TEST(test_rate_zero_when_starting_at_or_above_target);
    return UNITY_END();
}
```

- [x] **Step 2: Run to verify it fails**

Run: `pio test -e native -f test_dry_setpoint`
Expected: FAIL — `control/dry_setpoint.h: No such file or directory`.

- [x] **Step 3: Write the header**

Create `src/control/dry_setpoint.h`:

```c
/* Analytic drying setpoint — the whole reason drying needs no curve array.
 *
 *   setpoint(t) = min(start_temp_c + DRY_RAMP_RATE_C_S * t, target_c)
 *
 * A 12 h dry sampled at PROFILE_CURVE_STEP_MS would need ~170 KB against
 * 37.4 KiB of free static DRAM; this is two floats and a multiply. Pure
 * logic, no state. See drying spec 1.1.
 */
#pragma once

#include <stdint.h>

/* Setpoint in °C at `elapsed_ms` into the run. If start_temp_c >= target_c
 * the result is target_c for all t — the setpoint never descends. */
float dry_setpoint(float start_temp_c, float target_c, uint32_t elapsed_ms);

/* Commanded setpoint slope (°C/s) at `elapsed_ms`, for the feedforward
 * k_rate term: DRY_RAMP_RATE_C_S while ramping, 0 once at target. */
float dry_setpoint_rate(float start_temp_c, float target_c, uint32_t elapsed_ms);
```

- [x] **Step 4: Write the implementation**

Create `src/control/dry_setpoint.c`:

```c
#include "control/dry_setpoint.h"
#include "config.h"

float dry_setpoint(float start_temp_c, float target_c, uint32_t elapsed_ms) {
    if (start_temp_c >= target_c) return target_c;
    float sp = start_temp_c + DRY_RAMP_RATE_C_S * ((float)elapsed_ms / 1000.0f);
    return (sp > target_c) ? target_c : sp;
}

float dry_setpoint_rate(float start_temp_c, float target_c, uint32_t elapsed_ms) {
    /* Single source of truth for the ramp formula: the rate is just the
     * derivative of dry_setpoint() — nonzero only while it hasn't reached
     * target_c yet (this also covers start_temp_c >= target_c, where
     * dry_setpoint() returns target_c immediately). */
    float sp = dry_setpoint(start_temp_c, target_c, elapsed_ms);
    return (sp < target_c) ? DRY_RAMP_RATE_C_S : 0.0f;
}
```

- [x] **Step 5: Run tests**

Run: `pio test -e native -f test_dry_setpoint`
Expected: PASS, 6 tests.

- [x] **Step 6: Commit**

```bash
git add src/control/dry_setpoint.h src/control/dry_setpoint.c \
        test/test_dry_setpoint/test_dry_setpoint.c
git commit -m "feat(control): analytic drying setpoint

setpoint(t) = min(start + 0.05*t, target). No array at any duration, which is
what makes a 12 h hold fit alongside 37.4 KiB of free static DRAM."
```

---

### Task 5: Drying state machine

**Files:**
- Modify: `src/control/reflow_sm.h`, `src/control/reflow_sm.c`
- Test: `test/test_reflow_sm/test_reflow_sm.c`

- [x] **Step 1: Write the failing tests**

Append to `test/test_reflow_sm/test_reflow_sm.c`:

```c
/* ---- drying ---- */

static void dry_ticks(sm_state_t *s, float temp_c, int n) {
    float sp;
    for (int i = 0; i < n; i++) sm_dry_tick(s, temp_c, &sp);
}

void test_dry_start_requires_idle(void) {
    sm_state_t s; sm_init(&s);
    s.mode = MODE_RUNNING;
    TEST_ASSERT_FALSE(sm_start_dry(&s, 45.0f, 3600000u, 25.0f));
}

void test_dry_start_rejected_when_ota_reserved(void) {
    sm_state_t s; sm_init(&s);
    s.ota_reserved = true;
    TEST_ASSERT_FALSE(sm_start_dry(&s, 45.0f, 3600000u, 25.0f));
}

void test_dry_start_rejected_when_validation_pending(void) {
    sm_state_t s; sm_init(&s);
    s.validation = SM_VALIDATION_PENDING;
    TEST_ASSERT_FALSE(sm_start_dry(&s, 45.0f, 3600000u, 25.0f));
}

void test_dry_start_rejected_when_too_hot(void) {
    sm_state_t s; sm_init(&s);
    /* target 45 + DRY_START_MARGIN_C 5 = 50 is the ceiling */
    TEST_ASSERT_FALSE(sm_start_dry(&s, 45.0f, 3600000u, 50.1f));
    TEST_ASSERT_TRUE(sm_start_dry(&s, 45.0f, 3600000u, 49.9f));
}

void test_dry_start_clamps_target_and_duration(void) {
    sm_state_t s; sm_init(&s);
    TEST_ASSERT_TRUE(sm_start_dry(&s, 500.0f, 0xFFFFFFFFu, 25.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, DRY_TEMP_MAX_C, s.dry_target_c);
    TEST_ASSERT_EQUAL_UINT32(DRY_DURATION_MAX_MS, s.dry_hold_ms);

    sm_init(&s);
    TEST_ASSERT_TRUE(sm_start_dry(&s, 1.0f, 1u, 25.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, DRY_TEMP_MIN_C, s.dry_target_c);
    TEST_ASSERT_EQUAL_UINT32(DRY_HOLD_MIN_MS, s.dry_hold_ms);
}

void test_dry_start_sets_initial_state(void) {
    sm_state_t s; sm_init(&s);
    TEST_ASSERT_TRUE(sm_start_dry(&s, 45.0f, 3600000u, 22.0f));
    TEST_ASSERT_EQUAL_INT(MODE_DRYING, s.mode);
    TEST_ASSERT_EQUAL_INT(DRY_STAGE_HEAT_UP, s.dry_stage);
    TEST_ASSERT_EQUAL_INT(DRY_END_NONE, s.dry_end_reason);
    TEST_ASSERT_EQUAL_UINT32(0u, s.elapsed_ms);
    TEST_ASSERT_EQUAL_UINT32(0u, s.dry_hold_elapsed_ms);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 22.0f, s.dry_start_temp_c);
}

void test_dry_hold_timer_does_not_run_during_heat_up(void) {
    sm_state_t s; sm_init(&s);
    sm_start_dry(&s, 80.0f, 3600000u, 25.0f);
    dry_ticks(&s, 30.0f, 40);          /* well below the band */
    TEST_ASSERT_EQUAL_INT(DRY_STAGE_HEAT_UP, s.dry_stage);
    TEST_ASSERT_EQUAL_UINT32(0u, s.dry_hold_elapsed_ms);
    TEST_ASSERT_EQUAL_UINT32(40u * CONTROL_TICK_MS, s.elapsed_ms);
}

void test_dry_enters_hold_at_band(void) {
    sm_state_t s; sm_init(&s);
    sm_start_dry(&s, 80.0f, 3600000u, 25.0f);
    dry_ticks(&s, 77.9f, 1);           /* band is target - 2.0 = 78.0 */
    TEST_ASSERT_EQUAL_INT(DRY_STAGE_HEAT_UP, s.dry_stage);
    dry_ticks(&s, 78.1f, 1);
    TEST_ASSERT_EQUAL_INT(DRY_STAGE_HOLD, s.dry_stage);
}

void test_dry_hold_is_one_way(void) {
    sm_state_t s; sm_init(&s);
    sm_start_dry(&s, 80.0f, 3600000u, 25.0f);
    dry_ticks(&s, 79.0f, 1);
    TEST_ASSERT_EQUAL_INT(DRY_STAGE_HOLD, s.dry_stage);
    dry_ticks(&s, 40.0f, 4);           /* transient dip must not revert */
    TEST_ASSERT_EQUAL_INT(DRY_STAGE_HOLD, s.dry_stage);
    TEST_ASSERT_EQUAL_UINT32(5u * CONTROL_TICK_MS, s.dry_hold_elapsed_ms);
}

void test_dry_completes_after_hold_duration(void) {
    sm_state_t s; sm_init(&s);
    /* Shortest hold sm_start_dry will accept; anything less is clamped up to
     * DRY_HOLD_MIN_MS (see test_dry_start_clamps_target_and_duration). */
    const uint32_t hold  = DRY_HOLD_MIN_MS;
    const int      ticks = (int)(hold / CONTROL_TICK_MS);
    sm_start_dry(&s, 80.0f, hold, 25.0f);
    TEST_ASSERT_EQUAL_UINT32(hold, s.dry_hold_ms);
    dry_ticks(&s, 79.0f, ticks - 1);
    TEST_ASSERT_EQUAL_INT(MODE_DRYING, s.mode);
    float sp;
    TEST_ASSERT_FALSE(sm_dry_tick(&s, 79.0f, &sp));   /* last tick ends it */
    TEST_ASSERT_EQUAL_INT(MODE_IDLE, s.mode);
    TEST_ASSERT_EQUAL_INT(DRY_END_COMPLETE, s.dry_end_reason);
}

void test_dry_heatup_timeout(void) {
    sm_state_t s; sm_init(&s);
    sm_start_dry(&s, 100.0f, 3600000u, 25.0f);
    /* Never reaches the band. */
    const int ticks = (int)(DRY_HEATUP_TIMEOUT_MS / CONTROL_TICK_MS);
    dry_ticks(&s, 30.0f, ticks - 1);
    TEST_ASSERT_EQUAL_INT(MODE_DRYING, s.mode);
    float sp;
    TEST_ASSERT_FALSE(sm_dry_tick(&s, 30.0f, &sp));
    TEST_ASSERT_EQUAL_INT(MODE_IDLE, s.mode);
    TEST_ASSERT_EQUAL_INT(DRY_END_HEATUP_TIMEOUT, s.dry_end_reason);
}

void test_dry_stop_event(void) {
    sm_state_t s; sm_init(&s);
    sm_start_dry(&s, 80.0f, 3600000u, 25.0f);
    dry_ticks(&s, 79.0f, 3);
    TEST_ASSERT_TRUE(sm_dry_stop(&s));
    TEST_ASSERT_EQUAL_INT(MODE_IDLE, s.mode);
    TEST_ASSERT_EQUAL_INT(DRY_END_MANUAL_STOP, s.dry_end_reason);
}

void test_dry_fault_records_reason(void) {
    sm_state_t s; sm_init(&s);
    sm_start_dry(&s, 80.0f, 3600000u, 25.0f);
    sm_trip_fault(&s, FAULT_OVER_TEMP);
    TEST_ASSERT_EQUAL_INT(MODE_FAULT, s.mode);
    TEST_ASSERT_EQUAL_INT(DRY_END_FAULT, s.dry_end_reason);
}

void test_dry_tick_reports_setpoint(void) {
    sm_state_t s; sm_init(&s);
    sm_start_dry(&s, 80.0f, 3600000u, 25.0f);
    float sp = 0.0f;
    sm_dry_tick(&s, 30.0f, &sp);
    /* One tick elapsed: 25 + 0.05 * 0.25 s */
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 25.0f, sp);
}

void test_dry_tick_ignores_wrong_mode(void) {
    sm_state_t s; sm_init(&s);
    float sp = 0.0f;
    TEST_ASSERT_FALSE(sm_dry_tick(&s, 30.0f, &sp));
}

void test_dry_sm_step_stop_records_reason(void) {
    sm_state_t s; sm_init(&s);
    sm_start_dry(&s, 80.0f, 3600000u, 25.0f);
    sm_step(&s, SM_EVENT_CMD_STOP, 79.0f, NULL);
    TEST_ASSERT_EQUAL_INT(MODE_IDLE, s.mode);
    /* Not DRY_END_NONE: a STOP arriving via sm_step must be indistinguishable
     * from one via sm_dry_stop. */
    TEST_ASSERT_EQUAL_INT(DRY_END_MANUAL_STOP, s.dry_end_reason);
}

void test_dry_sm_step_does_not_advance_elapsed(void) {
    sm_state_t s; sm_init(&s);
    sm_start_dry(&s, 80.0f, 3600000u, 25.0f);
    dry_ticks(&s, 30.0f, 4);
    const uint32_t before = s.elapsed_ms;
    sm_step(&s, SM_EVENT_NONE, 30.0f, NULL);
    /* sm_dry_tick owns the drying clock; sm_step must not add to it. */
    TEST_ASSERT_EQUAL_UINT32(before, s.elapsed_ms);
    TEST_ASSERT_EQUAL_INT(MODE_DRYING, s.mode);
}
```

Register all seventeen in `main()`:

```c
    RUN_TEST(test_dry_start_requires_idle);
    RUN_TEST(test_dry_start_rejected_when_ota_reserved);
    RUN_TEST(test_dry_start_rejected_when_validation_pending);
    RUN_TEST(test_dry_start_rejected_when_too_hot);
    RUN_TEST(test_dry_start_clamps_target_and_duration);
    RUN_TEST(test_dry_start_sets_initial_state);
    RUN_TEST(test_dry_hold_timer_does_not_run_during_heat_up);
    RUN_TEST(test_dry_enters_hold_at_band);
    RUN_TEST(test_dry_hold_is_one_way);
    RUN_TEST(test_dry_completes_after_hold_duration);
    RUN_TEST(test_dry_heatup_timeout);
    RUN_TEST(test_dry_stop_event);
    RUN_TEST(test_dry_fault_records_reason);
    RUN_TEST(test_dry_tick_reports_setpoint);
    RUN_TEST(test_dry_tick_ignores_wrong_mode);
    RUN_TEST(test_dry_sm_step_stop_records_reason);
    RUN_TEST(test_dry_sm_step_does_not_advance_elapsed);
```

- [x] **Step 2: Run to verify it fails**

Run: `pio test -e native -f test_reflow_sm`
Expected: compile errors — `MODE_DRYING` undeclared, `sm_start_dry` not defined.

- [x] **Step 3: Extend the header**

`src/control/reflow_sm.h` — add `MODE_DRYING` to `oven_mode_t`:

```c
typedef enum {
    MODE_IDLE,
    MODE_RUNNING,
    MODE_FAULT,
    MODE_PENDING_VALIDATION,
    MODE_STEP_TEST,
    MODE_DRYING,
} oven_mode_t;
```

Add the drying types after `steptest_end_reason_t`:

```c
/* Filament drying sub-stage. The hold timer runs ONLY in DRY_STAGE_HOLD, so a
 * long ramp cannot consume the requested hold time. */
typedef enum {
    DRY_STAGE_NONE,
    DRY_STAGE_HEAT_UP,
    DRY_STAGE_HOLD,
} dry_stage_t;

/* Why a dry ended. DRY_END_NONE = still running or never run. */
typedef enum {
    DRY_END_NONE = 0,
    DRY_END_COMPLETE,        /* hold timer expired — normal */
    DRY_END_MANUAL_STOP,
    DRY_END_HEATUP_TIMEOUT,  /* never reached the band; see DRY_HEATUP_TIMEOUT_MS */
    DRY_END_FAULT,
} dry_end_reason_t;
```

Add to `sm_state_t`, after the step-test block:

```c
    /* Filament drying. Valid only in MODE_DRYING (or just ended). */
    float            dry_target_c;
    float            dry_start_temp_c;    /* oven temp at START; ramp origin */
    uint32_t         dry_hold_ms;         /* requested hold duration */
    uint32_t         dry_hold_elapsed_ms; /* accrues in DRY_STAGE_HOLD only */
    dry_stage_t      dry_stage;
    dry_end_reason_t dry_end_reason;
```

Add the API after `sm_step_test_tick`:

```c
/* Begin a dry. Returns true if accepted (idle, not OTA-reserved, not gated,
 * and temp_c <= target_c + DRY_START_MARGIN_C). Clamps target to
 * [DRY_TEMP_MIN_C, DRY_TEMP_MAX_C] and hold to
 * [DRY_HOLD_MIN_MS, DRY_DURATION_MAX_MS]. */
bool sm_start_dry(sm_state_t *s, float target_c, uint32_t hold_ms, float temp_c);

/* Advance a dry by one CONTROL_TICK_MS. Returns true if the SSR should be
 * driven this tick and writes the setpoint to *setpoint_out; false once the
 * run has ended (mode back to MODE_IDLE, dry_end_reason populated). The
 * CALLER must run all safety gates BEFORE this — a fault path uses
 * sm_trip_fault, which records DRY_END_FAULT itself. Pure logic; the caller
 * owns the SSR and the PID. */
bool sm_dry_tick(sm_state_t *s, float temp_c, float *setpoint_out);

/* Stop a running dry (touchscreen or browser). Returns true if a dry was
 * actually stopped. */
bool sm_dry_stop(sm_state_t *s);

/* Human-readable drying stage / end reason for the UI and telemetry.
 * Static strings, never NULL. */
const char *dry_stage_name(dry_stage_t s);
const char *dry_end_name(dry_end_reason_t r);
```

- [x] **Step 4: Implement**

`src/control/reflow_sm.c` — add the include at the top:

```c
#include "control/dry_setpoint.h"
```

Give `sm_step` a `MODE_DRYING` branch mirroring the `MODE_STEP_TEST` one,
placed immediately after it. Without this, `MODE_DRYING` falls through every
early branch to the tail, where `SM_EVENT_CMD_STOP` idles the oven without
setting `dry_end_reason`, and any other event adds a second `CONTROL_TICK_MS`
to `elapsed_ms` on top of `sm_dry_tick`'s, skewing the ramp clock:

```c
    if (s->mode == MODE_DRYING) {
        /* Route STOP through sm_dry_stop so the end reason is recorded; the
         * control task calls sm_dry_tick directly for the per-tick advance.
         * Falling through to the tail would double-count elapsed_ms. */
        if (event == SM_EVENT_CMD_STOP) sm_dry_stop(s);
        return false;
    }
```

Extend `sm_trip_fault` so a fault out of drying is recorded:

```c
void sm_trip_fault(sm_state_t *s, uint8_t flag) {
    if (s->mode == MODE_STEP_TEST) s->step_end_reason = STEP_END_FAULT;
    if (s->mode == MODE_DRYING)    s->dry_end_reason  = DRY_END_FAULT;
    s->mode = MODE_FAULT;
    s->stage = STAGE_NONE;
    s->fault_flags |= flag;
}
```

Add after `sm_step_test_tick`:

```c
static void dry_end(sm_state_t *s, dry_end_reason_t why) {
    s->mode           = MODE_IDLE;
    s->stage          = STAGE_NONE;
    s->dry_stage      = DRY_STAGE_NONE;
    s->dry_end_reason = why;
}

bool sm_start_dry(sm_state_t *s, float target_c, uint32_t hold_ms, float temp_c) {
    /* Same entry guard as every other heat-producing mode. */
    if (s->mode != MODE_IDLE) return false;
    if (s->ota_reserved) return false;
    if (s->validation != SM_VALIDATION_NONE) return false;

    if (target_c < DRY_TEMP_MIN_C) target_c = DRY_TEMP_MIN_C;
    if (target_c > DRY_TEMP_MAX_C) target_c = DRY_TEMP_MAX_C;
    if (hold_ms  < DRY_HOLD_MIN_MS)      hold_ms = DRY_HOLD_MIN_MS;
    if (hold_ms  > DRY_DURATION_MAX_MS)  hold_ms = DRY_DURATION_MAX_MS;

    /* Refuse a start hotter than the target rather than silently waiting to
     * cool. Clamped target first, so the gate tracks what will actually run. */
    if (temp_c > target_c + DRY_START_MARGIN_C) return false;

    s->mode                = MODE_DRYING;
    s->stage               = STAGE_NONE;
    s->elapsed_ms          = 0;
    s->dry_target_c        = target_c;
    s->dry_start_temp_c    = temp_c;
    s->dry_hold_ms         = hold_ms;
    s->dry_hold_elapsed_ms = 0;
    s->dry_stage           = DRY_STAGE_HEAT_UP;
    s->dry_end_reason      = DRY_END_NONE;
    return true;
}

bool sm_dry_tick(sm_state_t *s, float temp_c, float *setpoint_out) {
    if (s->mode != MODE_DRYING) return false;

    s->elapsed_ms += CONTROL_TICK_MS;

    if (s->dry_stage == DRY_STAGE_HEAT_UP) {
        /* One-way: once in the band we stay in HOLD, so a transient dip
         * cannot extend the run indefinitely. */
        if (temp_c >= s->dry_target_c - DRY_HOLD_BAND_C) {
            s->dry_stage = DRY_STAGE_HOLD;
        } else if (s->elapsed_ms >= DRY_HEATUP_TIMEOUT_MS) {
            /* Target unreachable (failed element, open door, huge load).
             * Without this the hold timer never starts and heating never
             * ends. */
            dry_end(s, DRY_END_HEATUP_TIMEOUT);
            return false;
        }
    }

    if (s->dry_stage == DRY_STAGE_HOLD) {
        s->dry_hold_elapsed_ms += CONTROL_TICK_MS;
        if (s->dry_hold_elapsed_ms >= s->dry_hold_ms) {
            dry_end(s, DRY_END_COMPLETE);
            return false;
        }
    }

    *setpoint_out = dry_setpoint(s->dry_start_temp_c, s->dry_target_c,
                                 s->elapsed_ms);
    return true;
}

bool sm_dry_stop(sm_state_t *s) {
    if (s->mode != MODE_DRYING) return false;
    dry_end(s, DRY_END_MANUAL_STOP);
    return true;
}

const char *dry_stage_name(dry_stage_t s) {
    switch (s) {
        case DRY_STAGE_HEAT_UP: return "heat_up";
        case DRY_STAGE_HOLD:    return "hold";
        default:                return "none";
    }
}

const char *dry_end_name(dry_end_reason_t r) {
    switch (r) {
        case DRY_END_COMPLETE:       return "complete";
        case DRY_END_MANUAL_STOP:    return "stopped";
        case DRY_END_HEATUP_TIMEOUT: return "heatup_timeout";
        case DRY_END_FAULT:          return "fault";
        default:                     return "none";
    }
}
```

- [x] **Step 5: Run tests**

Run: `pio test -e native -f test_reflow_sm`
Expected: PASS. All pre-existing reflow and step-test tests must still pass.

- [x] **Step 6: Commit**

```bash
git add src/control/reflow_sm.h src/control/reflow_sm.c \
        test/test_reflow_sm/test_reflow_sm.c
git commit -m "feat(control): MODE_DRYING state machine

Peer run mode alongside the step test. Hold timer accrues only in
DRY_STAGE_HOLD so a long ramp cannot eat the requested hold; the HEAT_UP
timeout bounds a target the oven cannot reach."
```

---

### Task 6: Preset persistence

**Files:**
- Modify: `src/storage/nvs_schema.h`, `src/storage/nvs_schema.c`
- Modify: `src/storage/nvs_store.h`, `src/storage/nvs_store.c`
- Modify: `include/config.h` (NVS key)
- Test: `test/test_nvs_schema/test_nvs_schema.c`

- [x] **Step 1: Write the failing tests**

Append to `test/test_nvs_schema/test_nvs_schema.c`:

```c
/* ---- drying presets ---- */

void test_dry_presets_defaults_are_valid(void) {
    dry_presets_t p;
    dry_presets_defaults(&p);
    TEST_ASSERT_TRUE(dry_presets_validate(&p));
    TEST_ASSERT_EQUAL_UINT16(45, p.e[0].temp_c);
    TEST_ASSERT_EQUAL_UINT16(360, p.e[0].hold_min);
    TEST_ASSERT_EQUAL_UINT16(90, p.e[4].temp_c);
}

void test_dry_presets_blob_round_trip(void) {
    dry_presets_t in, out;
    dry_presets_defaults(&in);
    in.e[2].temp_c = 55;
    in.e[2].hold_min = 90;

    dry_presets_v1_t blob;
    dry_presets_to_blob(&in, &blob);
    TEST_ASSERT_TRUE(dry_presets_from_blob(&blob, sizeof blob, &out));
    TEST_ASSERT_EQUAL_UINT16(55, out.e[2].temp_c);
    TEST_ASSERT_EQUAL_UINT16(90, out.e[2].hold_min);
}

void test_dry_presets_rejects_wrong_length(void) {
    dry_presets_t in, out;
    dry_presets_defaults(&in);
    dry_presets_v1_t blob;
    dry_presets_to_blob(&in, &blob);
    TEST_ASSERT_FALSE(dry_presets_from_blob(&blob, sizeof(blob) - 1, &out));
}

void test_dry_presets_rejects_wrong_version(void) {
    dry_presets_t in, out;
    dry_presets_defaults(&in);
    dry_presets_v1_t blob;
    dry_presets_to_blob(&in, &blob);
    blob.version = 99;
    TEST_ASSERT_FALSE(dry_presets_from_blob(&blob, sizeof blob, &out));
}

void test_dry_presets_validate_rejects_out_of_range(void) {
    dry_presets_t p;

    dry_presets_defaults(&p);
    p.e[1].temp_c = (uint16_t)(DRY_TEMP_MAX_C + 1);
    TEST_ASSERT_FALSE(dry_presets_validate(&p));

    dry_presets_defaults(&p);
    p.e[1].temp_c = (uint16_t)(DRY_TEMP_MIN_C - 1);
    TEST_ASSERT_FALSE(dry_presets_validate(&p));

    dry_presets_defaults(&p);
    p.e[3].hold_min = 0;
    TEST_ASSERT_FALSE(dry_presets_validate(&p));

    dry_presets_defaults(&p);
    p.e[3].hold_min = (uint16_t)(DRY_DURATION_MAX_MS / 60000u) + 1;
    TEST_ASSERT_FALSE(dry_presets_validate(&p));
}

void test_dry_preset_names_are_stable(void) {
    TEST_ASSERT_EQUAL_STRING("PLA", dry_preset_name(0));
    TEST_ASSERT_EQUAL_STRING("PC",  dry_preset_name(DRY_PRESET_COUNT - 1));
    TEST_ASSERT_EQUAL_STRING("?",   dry_preset_name(DRY_PRESET_COUNT));
}
```

Register in `main()`:

```c
    RUN_TEST(test_dry_presets_defaults_are_valid);
    RUN_TEST(test_dry_presets_blob_round_trip);
    RUN_TEST(test_dry_presets_rejects_wrong_length);
    RUN_TEST(test_dry_presets_rejects_wrong_version);
    RUN_TEST(test_dry_presets_validate_rejects_out_of_range);
    RUN_TEST(test_dry_preset_names_are_stable);
```

- [x] **Step 2: Run to verify it fails**

Run: `pio test -e native -f test_nvs_schema`
Expected: compile error — `unknown type name 'dry_presets_t'`.

- [x] **Step 3: Extend the schema header**

`src/storage/nvs_schema.h` — add after the PID blob types:

```c
#define DRY_PRESETS_SCHEMA_VERSION 1

/* Runtime form. Names are NOT stored — they are compile-time (see config.h
 * DRY_PRESET_DEFAULTS); only the editable values persist. */
typedef struct {
    uint16_t temp_c;
    uint16_t hold_min;
} dry_entry_t;

typedef struct {
    dry_entry_t e[DRY_PRESET_COUNT];
} dry_presets_t;

/* Stored form: 1 + 6*4 = 25 bytes. */
typedef struct __attribute__((packed)) {
    uint8_t     version;
    dry_entry_t e[DRY_PRESET_COUNT];
} dry_presets_v1_t;

void dry_presets_to_blob(const dry_presets_t *p, dry_presets_v1_t *out);
bool dry_presets_from_blob(const void *blob, size_t blob_len, dry_presets_t *out);
void dry_presets_defaults(dry_presets_t *out);

/* Range-checks every entry against the drying bounds in config.h. A corrupt
 * or out-of-range record must never be partially trusted — an out-of-range
 * target is a thermal hazard, same reasoning as pid_validate. */
bool dry_presets_validate(const dry_presets_t *p);

/* Compile-time preset name for index i. Returns "?" when out of range.
 * Static string, never NULL, never freed. */
const char *dry_preset_name(unsigned i);
```

- [x] **Step 4: Implement the schema**

`src/storage/nvs_schema.c` — append:

```c
/* Compile-time name + default value table (config.h DRY_PRESET_DEFAULTS). */
static const struct { const char *name; uint16_t temp_c; uint16_t hold_min; }
    s_dry_defaults[DRY_PRESET_COUNT] = DRY_PRESET_DEFAULTS;

const char *dry_preset_name(unsigned i) {
    if (i >= DRY_PRESET_COUNT) return "?";
    return s_dry_defaults[i].name;
}

void dry_presets_defaults(dry_presets_t *out) {
    for (unsigned i = 0; i < DRY_PRESET_COUNT; i++) {
        out->e[i].temp_c   = s_dry_defaults[i].temp_c;
        out->e[i].hold_min = s_dry_defaults[i].hold_min;
    }
}

void dry_presets_to_blob(const dry_presets_t *p, dry_presets_v1_t *out) {
    out->version = DRY_PRESETS_SCHEMA_VERSION;
    for (unsigned i = 0; i < DRY_PRESET_COUNT; i++) out->e[i] = p->e[i];
}

bool dry_presets_from_blob(const void *blob, size_t blob_len, dry_presets_t *out) {
    if (blob_len != sizeof(dry_presets_v1_t)) return false;
    const dry_presets_v1_t *b = (const dry_presets_v1_t *)blob;
    if (b->version != DRY_PRESETS_SCHEMA_VERSION) return false;
    for (unsigned i = 0; i < DRY_PRESET_COUNT; i++) out->e[i] = b->e[i];
    return true;
}

bool dry_presets_validate(const dry_presets_t *p) {
    const uint16_t hold_max_min = (uint16_t)(DRY_DURATION_MAX_MS / 60000u);
    const uint16_t hold_min_min = (uint16_t)(DRY_HOLD_MIN_MS / 60000u);
    for (unsigned i = 0; i < DRY_PRESET_COUNT; i++) {
        if (p->e[i].temp_c < (uint16_t)DRY_TEMP_MIN_C) return false;
        if (p->e[i].temp_c > (uint16_t)DRY_TEMP_MAX_C) return false;
        if (p->e[i].hold_min < (hold_min_min ? hold_min_min : 1)) return false;
        if (p->e[i].hold_min > hold_max_min) return false;
    }
    return true;
}
```

Add `#include "config.h"` at the top of `nvs_schema.c` if it is not already there.

- [x] **Step 5: Add the NVS key and store functions**

`include/config.h` — beside the other `NVS_KEY_*` definitions:

```c
#define NVS_KEY_DRY_PRESETS  "dry_presets"
```

`src/storage/nvs_store.h` — after the PID entries:

```c
/* Drying presets. Same contract as the profile/PID loaders: *out is always
 * populated, falling back to dry_presets_defaults() on miss, schema mismatch
 * or validation failure. */
void nvs_store_load_dry_presets(dry_presets_t *out);
bool nvs_store_save_dry_presets(const dry_presets_t *p);
```

`src/storage/nvs_store.c` — mirror `nvs_store_load_profile` / `save_profile`
exactly, with defaults as the first statement:

```c
void nvs_store_load_dry_presets(dry_presets_t *out) {
    dry_presets_defaults(out);          /* first statement: every early return
                                         * below leaves safe values in place */
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    dry_presets_v1_t blob;
    size_t len = sizeof blob;
    esp_err_t err = nvs_get_blob(h, NVS_KEY_DRY_PRESETS, &blob, &len);
    nvs_close(h);
    if (err != ESP_OK) return;

    dry_presets_t tmp;
    if (!dry_presets_from_blob(&blob, len, &tmp)) {
        ESP_LOGW(TAG, "dry presets: bad blob, using defaults");
        return;
    }
    if (!dry_presets_validate(&tmp)) {
        ESP_LOGW(TAG, "dry presets: out of range, using defaults");
        return;
    }
    *out = tmp;
}

bool nvs_store_save_dry_presets(const dry_presets_t *p) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "dry presets: nvs_open failed");
        return false;
    }
    dry_presets_v1_t blob;
    dry_presets_to_blob(p, &blob);
    esp_err_t err = nvs_set_blob(h, NVS_KEY_DRY_PRESETS, &blob, sizeof blob);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dry presets: save failed (%s)", esp_err_to_name(err));
        return false;
    }
    return true;
}
```

Match the existing file's `NVS_NAMESPACE` and `TAG` names — read the top of
`nvs_store.c` and reuse whatever it already defines rather than inventing new
identifiers.

- [x] **Step 6: Run tests**

Run: `pio test -e native -f test_nvs_schema`
Expected: PASS, 6 new tests plus all pre-existing.

- [x] **Step 7: Commit**

```bash
git add include/config.h src/storage/nvs_schema.h src/storage/nvs_schema.c \
        src/storage/nvs_store.h src/storage/nvs_store.c \
        test/test_nvs_schema/test_nvs_schema.c
git commit -m "feat(storage): drying preset persistence (25-byte blob v1)

Names stay compile-time; only temperature and hold time persist. Out-of-range
or wrong-version records fall back to defaults rather than being partially
trusted, as for PID gains."
```

---

### Task 7: IPC — command and published state

**Files:**
- Modify: `src/oven_state.h`

- [x] **Step 1: Add the command**

In `cmd_type_t`, after `CMD_START_STEP_TEST`:

```c
    CMD_START_DRY,         /* touchscreen-only: begin a filament dry */
```

Add the payload struct next to `steptest_params_t`:

```c
typedef struct {
    float    target_c;
    uint32_t hold_ms;
} dry_params_t;
```

and add it to the `cmd_msg_t` union:

```c
        dry_params_t      dry;       /* START_DRY */
```

- [x] **Step 2: Add the published fields**

In `oven_state_t`, after the step-test sub-status block:

```c
    /* Drying sub-status (valid when mode == MODE_DRYING or just ended).
     * Scalars only — no pointer is published here, so the `curve` locking
     * caveat above does not extend to these. */
    bool        dry_active;
    uint8_t     dry_stage;            /* dry_stage_t, copied as raw */
    float       dry_target_c;
    uint32_t    dry_hold_ms;
    uint32_t    dry_hold_elapsed_ms;
    uint8_t     dry_end_reason;       /* dry_end_reason_t, copied as raw */
```

- [x] **Step 3: Verify it still builds**

Run: `pio test -e native`
Expected: PASS — this task adds no behaviour, only IPC surface.

- [x] **Step 4: Commit**

```bash
git add src/oven_state.h
git commit -m "feat(ipc): CMD_START_DRY and drying fields in oven_state_t"
```

---

### Task 8: Global electronics over-temperature guard

`cj_temp_c` has been display-only. It applies to **every** mode, not just drying: if the board that switches mains reaches 75 °C, aborting is correct in any run.

**Files:**
- Modify: `src/control/reflow_sm.h`, `src/control/reflow_sm.c`
- Modify: `src/control/control_task.c`
- Test: `test/test_reflow_sm/test_reflow_sm.c`

- [x] **Step 1: Write the failing test**

Append to `test/test_reflow_sm/test_reflow_sm.c`:

```c
void test_electronics_fault_text(void) {
    TEST_ASSERT_EQUAL_STRING("electronics too hot",
                             fault_text(FAULT_ELECTRONICS_HOT));
}

void test_electronics_fault_trips_out_of_drying(void) {
    sm_state_t s; sm_init(&s);
    sm_start_dry(&s, 80.0f, 3600000u, 25.0f);
    sm_trip_fault(&s, FAULT_ELECTRONICS_HOT);
    TEST_ASSERT_EQUAL_INT(MODE_FAULT, s.mode);
    TEST_ASSERT_EQUAL_INT(DRY_END_FAULT, s.dry_end_reason);
    TEST_ASSERT_TRUE(s.fault_flags & FAULT_ELECTRONICS_HOT);
}
```

Register both in `main()`:

```c
    RUN_TEST(test_electronics_fault_text);
    RUN_TEST(test_electronics_fault_trips_out_of_drying);
```

- [x] **Step 2: Run to verify it fails**

Run: `pio test -e native -f test_reflow_sm`
Expected: compile error — `FAULT_ELECTRONICS_HOT` undeclared.

- [x] **Step 3: Add the flag and its text**

`src/control/reflow_sm.h` — extend `fault_flag_t`:

```c
typedef enum {
    FAULT_NONE            = 0,
    FAULT_TC_OPEN         = 1 << 0,
    FAULT_TC_SCG          = 1 << 1,
    FAULT_TC_SCV          = 1 << 2,
    FAULT_TC_STALE        = 1 << 3,
    FAULT_OVER_TEMP       = 1 << 4,
    FAULT_ELECTRONICS_HOT = 1 << 5,
} fault_flag_t;
```

`src/control/reflow_sm.c` — add to `fault_text`, before the `return "unknown"`:

```c
    if (fault_flags & FAULT_ELECTRONICS_HOT) return "electronics too hot";
```

- [x] **Step 4: Add the gate**

`src/control/control_task.c` — immediately after the existing
`temp_c > OVER_TEMP_LIMIT_C` gate, add a fourth gate with the same shape:

```c
        /* Electronics over-temperature. The oven vents through the
         * electronics bay and no archived run ever reached thermal
         * equilibrium — the 40 min hold staircase hit 60 C still climbing.
         * A 12 h dry is an uncharacterised sustained load, so this guards
         * every mode, not just drying. isnan() check: cj_temp_c is NAN
         * until the first successful read (see oven_state.h). */
        if (!isnan(s_cj_c) && s_cj_c > CJ_LIMIT_C) {
            sm_trip_fault(&s_sm, FAULT_ELECTRONICS_HOT);
            ssr_force_off();
            (void)drain_commands();
            publish_and_detect(0, 0.0f, temp_ptr);
            goto feed_wdt;
        }
```

Add `#include <math.h>` at the top of `control_task.c` if not already present.

- [x] **Step 5: Run tests**

Run: `pio test -e native`
Expected: PASS.

- [x] **Step 6: Commit**

```bash
git add src/control/reflow_sm.h src/control/reflow_sm.c \
        src/control/control_task.c test/test_reflow_sm/test_reflow_sm.c
git commit -m "feat(safety): global electronics over-temperature fault at 75 C

cj_temp_c was display-only, sized for a 26-minute reflow. The 2026-07-21 hold
staircase reached 60 C after 40 min and was still climbing; no archived run
reached equilibrium. Guards every mode - aborting is correct in any run if the
board switching mains reaches 75 C."
```

---

### Task 9: Wire drying into `control_task`

**Files:**
- Modify: `src/control/control_task.c`

- [x] **Step 1: Add includes and drying command handling**

Add near the other control includes:

```c
#include "control/dry_setpoint.h"
```

In `drain_commands()`, handle the new command alongside `CMD_START_STEP_TEST`
— store the parameters in a file-static and return the event. Add the static
beside `s_step_params`:

```c
static dry_params_t s_dry_params;
```

and in the command switch:

```c
            case CMD_START_DRY:
                s_dry_params = m.payload.dry;
                ev = SM_EVENT_CMD_START_DRY;
                break;
```

Add `SM_EVENT_CMD_START_DRY` to `sm_event_t` in `src/control/reflow_sm.h`:

```c
    SM_EVENT_CMD_START_DRY,
```

- [x] **Step 2: Handle the start event**

In the run loop, directly after the existing `SM_EVENT_CMD_START_STEP_TEST`
block:

```c
        /* Drying start is a distinct entry (touchscreen-only), handled before
         * sm_step so it doesn't fall through reflow logic. */
        if (ev == SM_EVENT_CMD_START_DRY) {
            if (sm_start_dry(&s_sm, s_dry_params.target_c,
                             s_dry_params.hold_ms, temp_c)) {
                pid_reset(&s_pid, temp_c);     /* captures ambient_ref for ff */
                publish_pid_info();
                ssr_set_window_quanta(DRY_SSR_QUANTA);
                s_run_id++;
            }
            ev = SM_EVENT_NONE;   /* consumed */
        }
```

- [x] **Step 3: Route STOP to the drying stopper**

Still before `sm_step`, so a STOP during drying does not fall through reflow
logic:

```c
        if (ev == SM_EVENT_CMD_STOP && s_sm.mode == MODE_DRYING) {
            sm_dry_stop(&s_sm);
            ssr_force_off();
            ssr_set_window_quanta(SSR_QUANTA_PER_WINDOW);
            ev = SM_EVENT_NONE;
        }
```

- [x] **Step 4: Add the drive branch**

Extend the dispatch chain. The drying branch is closed-loop (unlike the
open-loop step test), so it runs the PID with the drying ceiling:

```c
        } else if (s_sm.mode == MODE_DRYING) {
            float dry_sp = 0.0f;
            if (sm_dry_tick(&s_sm, temp_c, &dry_sp)) {
                setpoint = dry_sp;
                float rate = dry_setpoint_rate(s_sm.dry_start_temp_c,
                                               s_sm.dry_target_c,
                                               s_sm.elapsed_ms);
                float out = pid_step(&s_pid, setpoint, rate, temp_c, dt,
                                     DRY_DUTY_MAX_PERCENT);
                duty = (uint8_t)out;
                ssr_tick((uint32_t)(esp_timer_get_time() / 1000), duty);
            } else {
                /* Run ended this tick (complete, stopped, or heat-up timeout). */
                ssr_force_off();
                ssr_set_window_quanta(SSR_QUANTA_PER_WINDOW);
            }
        } else {
```

- [x] **Step 5: Add the drying over-temperature guard**

This is mode-scoped, so it belongs with the drive branch rather than the
global gates. Insert as the first statement inside the `MODE_DRYING` branch,
before `sm_dry_tick`:

```c
            /* Mode-scoped over-temperature. The global OVER_TEMP_LIMIT_C
             * (260 C) is no guard for a 45 C dry: a welded SSR would fuse the
             * spool long before it trips. Evaluated against the TARGET, not
             * the ramping setpoint, so the ramp cannot nuisance-trip it. */
            if (temp_c > s_sm.dry_target_c + DRY_OVER_TEMP_MARGIN_C) {
                sm_trip_fault(&s_sm, FAULT_OVER_TEMP);
                ssr_force_off();
                ssr_set_window_quanta(SSR_QUANTA_PER_WINDOW);
                publish_and_detect(0, 0.0f, &temp_c);
                goto feed_wdt;
            }
```

- [x] **Step 6: Publish the drying fields**

In `publish()`, after the step-test fields:

```c
    s_pub->dry_active          = (s_sm.mode == MODE_DRYING);
    s_pub->dry_stage           = (uint8_t)s_sm.dry_stage;
    s_pub->dry_target_c        = s_sm.dry_target_c;
    s_pub->dry_hold_ms         = s_sm.dry_hold_ms;
    s_pub->dry_hold_elapsed_ms = s_sm.dry_hold_elapsed_ms;
    s_pub->dry_end_reason      = (uint8_t)s_sm.dry_end_reason;
```

- [x] **Step 7: Restore the window on any fault exit**

`sm_trip_fault` is called from the global gates, which do not know about the
SSR window. Add one line to each of the four global gate blocks, right after
their `ssr_force_off()`:

```c
            ssr_set_window_quanta(SSR_QUANTA_PER_WINDOW);
```

- [x] **Step 8: Build and test**

Run: `pio test -e native && pio run -e esp32dev`
Expected: tests PASS; firmware builds clean.

- [x] **Step 9: Commit**

```bash
git add src/control/control_task.c src/control/reflow_sm.h
git commit -m "feat(control): drive drying from control_task

Closed-loop PID with the drying duty ceiling and SSR window, a mode-scoped
over-temperature guard at target + 10 C, and window restoration on every exit
path including faults."
```

---

### Task 10: Telemetry v3

**Files:**
- Modify: `src/ota/telemetry_json.h`, `src/ota/telemetry_json.c`
- Modify: `include/config.h` (`TELEMETRY_API_VERSION`)
- Modify: `src/ota/ota_server.c` (populate the view)
- Test: `test/test_telemetry_json/test_telemetry_json.c`

- [x] **Step 1: Write the failing test**

Append to `test/test_telemetry_json/test_telemetry_json.c`:

```c
void test_telemetry_includes_drying_fields(void) {
    char buf[768];
    telemetry_view_t v = {0};
    v.api = 3;
    v.mode = "drying";
    v.temp_c = 78.5f; v.setpoint_c = 80.0f; v.cj_c = 41.0f;
    v.duty = 12; v.elapsed_ms = 900000; v.run_id = 7;
    v.dry_active = true;
    v.dry_stage = "hold";
    v.dry_target_c = 80.0f;
    v.dry_hold_ms = 21600000u;
    v.dry_hold_elapsed_ms = 600000u;
    v.dry_end = "none";

    int n = telemetry_json(&v, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mode\":\"drying\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"dry_active\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"dry_stage\":\"hold\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"dry_hold_ms\":21600000"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"dry_hold_elapsed_ms\":600000"));
}

void test_telemetry_drying_inactive_shape(void) {
    char buf[768];
    telemetry_view_t v = {0};
    v.api = 3; v.mode = "idle";
    v.dry_active = false; v.dry_stage = "none"; v.dry_end = "none";
    int n = telemetry_json(&v, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"dry_active\":false"));
}
```

Register both in `main()`. Add `#include <string.h>` if not present.

- [x] **Step 2: Run to verify it fails**

Run: `pio test -e native -f test_telemetry_json`
Expected: compile error — `telemetry_view_t has no member named 'dry_active'`.

- [x] **Step 3: Extend the view**

`src/ota/telemetry_json.h` — add to `telemetry_view_t`, after the step fields:

```c
    bool     dry_active;
    const char *dry_stage;     /* already-resolved: "none"/"heat_up"/"hold" */
    float    dry_target_c;
    unsigned dry_hold_ms, dry_hold_elapsed_ms;
    const char *dry_end;       /* already-resolved end-reason string */
```

- [x] **Step 4: Emit them**

`src/ota/telemetry_json.c` — extend the format string and argument list with
the drying block, following exactly the style already used for the step-test
fields:

```c
        ",\"dry_active\":%s"
        ",\"dry_stage\":\"%s\""
        ",\"dry_target_c\":%.1f"
        ",\"dry_hold_ms\":%u"
        ",\"dry_hold_elapsed_ms\":%u"
        ",\"dry_end\":\"%s\""
```

with arguments:

```c
        v->dry_active ? "true" : "false",
        v->dry_stage ? v->dry_stage : "none",
        (double)v->dry_target_c,
        v->dry_hold_ms,
        v->dry_hold_elapsed_ms,
        v->dry_end ? v->dry_end : "none",
```

- [x] **Step 5: Bump the API version**

`include/config.h`:

```c
#define TELEMETRY_API_VERSION        3   /* v3: drying fields */
```

- [x] **Step 6: Populate from the snapshot**

`src/ota/ota_server.c`, in the telemetry handler where the view is built from
the `oven_state_t` snapshot, add after the step-test assignments:

```c
    v.dry_active          = snap.dry_active;
    v.dry_stage           = dry_stage_name((dry_stage_t)snap.dry_stage);
    v.dry_target_c        = snap.dry_target_c;
    v.dry_hold_ms         = snap.dry_hold_ms;
    v.dry_hold_elapsed_ms = snap.dry_hold_elapsed_ms;
    v.dry_end             = dry_end_name((dry_end_reason_t)snap.dry_end_reason);
```

In the same file, the mode-to-string helper gains a case:

```c
        case MODE_DRYING: return "drying";
```

Also confirm the JSON buffer passed to `telemetry_json` is large enough for
the six extra fields (~120 bytes); grow it if the handler uses a tight fixed
size. `telemetry_json` returns -1 on truncation, so a too-small buffer fails
loudly rather than emitting malformed JSON.

- [x] **Step 7: Run tests and build**

Run: `pio test -e native && pio run -e esp32dev`
Expected: PASS, clean build.

- [x] **Step 8: Commit**

```bash
git add include/config.h src/ota/telemetry_json.h src/ota/telemetry_json.c \
        src/ota/ota_server.c test/test_telemetry_json/test_telemetry_json.c
git commit -m "feat(api): telemetry v3 with drying fields"
```

---

### Task 11: HOME graph renders drying runs

Reusing HOME rather than adding a second graph screen is the RAM decision — `screen_home.c.o` is already 25.8 KiB of static buffers and every LVGL screen permanently consumes the 64 KB pool.

**Files:**
- Modify: `src/ui/screen_home.h`, `src/ui/screen_home.c`

- [x] **Step 1: Add the drying render entry point**

`src/ui/screen_home.h` — add:

```c
/* Switch the graph into drying mode: Y axis rescaled to target + 20 C and the
 * X axis becomes a rolling DRY_GRAPH_WINDOW_MS window ending at "now", rather
 * than the whole run. A 6 h hold plotted whole is a flat line carrying no
 * information; the rolling window shows ripple and stability. Pass
 * target_c <= 0 to return the graph to reflow scaling. */
void screen_home_set_dry_view(float target_c);
```

- [x] **Step 2: Add the constants**

`include/config.h`, in the drying block:

```c
/* HOME graph while drying: a rolling window, sampled coarsely enough that it
 * fits inside the existing s_live_pts ring (1600 slots) with headroom. */
#define DRY_GRAPH_WINDOW_MS     1800000u   /* 30 min */
#define DRY_GRAPH_SAMPLE_MS        2000u   /* 900 samples per window */
_Static_assert(DRY_GRAPH_WINDOW_MS / DRY_GRAPH_SAMPLE_MS <= PROFILE_CURVE_MAX_SAMPLES,
               "drying graph window must fit the existing live-trace ring");
```

- [x] **Step 3: Implement the rescaling**

`src/ui/screen_home.c` — add file-statics and use them in the two mapping
helpers. Replace the fixed `OVER_TEMP_LIMIT_C` in the Y mapping and the
cycle-window X mapping:

```c
/* > 0 while a dry is being rendered; drives both axis mappings. */
static float s_dry_target_c = 0.0f;

void screen_home_set_dry_view(float target_c) {
    s_dry_target_c = (target_c > 0.0f) ? target_c : 0.0f;
}

static float graph_temp_max(void) {
    return (s_dry_target_c > 0.0f) ? (s_dry_target_c + 20.0f)
                                   : OVER_TEMP_LIMIT_C;
}
```

In the Y mapping helper, replace `OVER_TEMP_LIMIT_C` with `graph_temp_max()`.

In the X mapping, when `s_dry_target_c > 0` the window runs from
`elapsed_ms - DRY_GRAPH_WINDOW_MS` (floored at 0) to `elapsed_ms`, so a point
at `t_ms` maps to:

```c
    /* Rolling window: newest sample at the right edge. */
    uint32_t t_end   = elapsed_ms;
    uint32_t t_start = (t_end > DRY_GRAPH_WINDOW_MS)
                     ? (t_end - DRY_GRAPH_WINDOW_MS) : 0u;
    uint32_t span    = (t_end > t_start) ? (t_end - t_start) : 1u;
    if (t_ms < t_start) return -1;      /* off the left edge: skip the point */
    return GRAPH_INSET_LEFT +
           (int32_t)((uint64_t)(t_ms - t_start) * GRAPH_PLOT_W / span);
```

Callers must skip points whose mapped X is negative.

- [x] **Step 4: Decimate the live trace while drying**

Where the live ring is appended, gate the append on the drying cadence so
30 minutes fits in 900 of the 1600 slots:

```c
    const uint32_t cadence = (s_dry_target_c > 0.0f) ? DRY_GRAPH_SAMPLE_MS
                                                     : PROFILE_CURVE_STEP_MS;
    if (snap->elapsed_ms - s_last_live_ms < cadence) return;
    s_last_live_ms = snap->elapsed_ms;
```

- [x] **Step 5: Draw the drying reference line**

While drying, the planned curve is a horizontal line at the target plus the
ramp segment if still in view — three points written into the existing
`s_plan_pts`, replacing the reflow curve render:

```c
    if (s_dry_target_c > 0.0f) {
        /* Reference: flat line at the target across the visible window. */
        s_plan_pts[0].x = GRAPH_INSET_LEFT;
        s_plan_pts[0].y = graph_y(s_dry_target_c);
        s_plan_pts[1].x = GRAPH_INSET_LEFT + GRAPH_PLOT_W;
        s_plan_pts[1].y = graph_y(s_dry_target_c);
        lv_line_set_points(s_plan_line, s_plan_pts, 2);
        return;
    }
```

placed at the top of `screen_home_set_curve`, before the reflow path. Use
whatever the file's existing Y-mapping helper is actually named in place of
`graph_y`.

- [x] **Step 6: Show the drying stage chip**

In `screen_home_update`, when `snap->dry_active`, render the chip from the
drying fields instead of the reflow stage:

```c
    if (snap->dry_active) {
        const char *st = (snap->dry_stage == DRY_STAGE_HOLD) ? "HOLD" : "HEAT-UP";
        if (snap->dry_stage == DRY_STAGE_HOLD) {
            lv_label_set_text_fmt(s_stage_label, "DRY · %s %lu:%02lu / %lu:%02lu",
                st,
                (unsigned long)(snap->dry_hold_elapsed_ms / 3600000u),
                (unsigned long)((snap->dry_hold_elapsed_ms / 60000u) % 60u),
                (unsigned long)(snap->dry_hold_ms / 3600000u),
                (unsigned long)((snap->dry_hold_ms / 60000u) % 60u));
        } else {
            lv_label_set_text_fmt(s_stage_label, "DRY · %s", st);
        }
    }
```

Use the file's actual stage-label handle name in place of `s_stage_label`.

- [x] **Step 7: Build**

Run: `pio run -e esp32dev`
Expected: clean build. (No host test — `screen_home.c` is excluded from the
native env by `build_src_filter`.)

- [x] **Step 8: Commit**

```bash
git add include/config.h src/ui/screen_home.h src/ui/screen_home.c
git commit -m "feat(ui): render drying runs on the HOME graph

Rolling 30-minute window and a target-relative Y axis, reusing the existing
25.8 KiB of trace buffers rather than adding a second graph screen."
```

---

### Task 12: The DRY tab

> **Superseded in part, 2026-09-01 (commit `41bf67b`).** The row-editing
> interaction below — one tap selects, a second tap on the selected row opens
> the temperature keypad, which then *chains* into hold-time entry — did not
> survive bench testing and is **not** what shipped. Editing one field forced a
> decision about the other, and an ESC out of the temperature keypad skipped
> the hold time entirely. The shipped `screen_dry.c` gives each row two
> independently tappable value zones, each opening the keypad for its own field
> and persisting on its own, and the hold-time keypad works in whole minutes
> rather than fractional hours. Drying spec §8.1 carries the amended
> description. The rest of this task is accurate; read the chaining code below
> as history, not as the current design.

**Files:**
- Create: `src/ui/screen_dry.h`, `src/ui/screen_dry.c`
- Modify: `src/ui/ui_task.c`

- [x] **Step 1: Write the header**

Create `src/ui/screen_dry.h`:

```c
/* DRY tab — filament drying preset list, editing, and START.
 * Structurally parallel to the PROFILE tab: PROFILE edits the reflow recipe,
 * DRY edits the drying recipe and starts it. Runs render on HOME.
 */
#pragma once
#include "lvgl.h"
#include "oven_state.h"

void screen_dry_create(lv_obj_t *parent);

/* Refresh enable/disable state from the latest snapshot (START is disabled
 * while anything is running). */
void screen_dry_update(const oven_state_t *snap);
```

- [x] **Step 2: Write the implementation**

Create `src/ui/screen_dry.c`:

```c
#include "ui/screen_dry.h"
#include "ui/screen_keypad.h"
#include "ui/theme.h"
#include "storage/nvs_schema.h"
#include "storage/nvs_store.h"
#include "config.h"
#include <stdio.h>

extern QueueHandle_t ui_cmd_queue(void);   /* provided by ui_task.c */

static dry_presets_t s_presets;
static unsigned      s_selected = 0;       /* index 0 (PLA) on every boot */
static lv_obj_t     *s_rows[DRY_PRESET_COUNT];
static lv_obj_t     *s_vals[DRY_PRESET_COUNT];
static lv_obj_t     *s_start_btn;

static void row_text(unsigned i, char *buf, size_t n) {
    unsigned h = s_presets.e[i].hold_min / 60u;
    unsigned m = s_presets.e[i].hold_min % 60u;
    if (m) snprintf(buf, n, "%u °C   %uh%02u", s_presets.e[i].temp_c, h, m);
    else   snprintf(buf, n, "%u °C   %u h",   s_presets.e[i].temp_c, h);
}

static void refresh_rows(void) {
    char buf[32];
    for (unsigned i = 0; i < DRY_PRESET_COUNT; i++) {
        row_text(i, buf, sizeof buf);
        lv_label_set_text(s_vals[i], buf);
        lv_obj_set_style_border_color(s_rows[i],
            (i == s_selected) ? THEME_ACCENT : THEME_BORDER, 0);
        lv_obj_set_style_border_width(s_rows[i], (i == s_selected) ? 2 : 1, 0);
    }
}

static void persist(void) {
    if (!dry_presets_validate(&s_presets)) {
        dry_presets_defaults(&s_presets);   /* never persist an invalid set */
    }
    nvs_store_save_dry_presets(&s_presets);
    refresh_rows();
}

static void hold_done_cb(float value, void *ud) {
    unsigned i = (unsigned)(intptr_t)ud;
    s_presets.e[i].hold_min = (uint16_t)(value * 60.0f + 0.5f);
    persist();
}

static void temp_done_cb(float value, void *ud) {
    unsigned i = (unsigned)(intptr_t)ud;
    s_presets.e[i].temp_c = (uint16_t)(value + 0.5f);
    /* SUPERSEDED — see the note at the head of Task 12. Shipped code does not
     * chain; temperature and hold time are edited independently. */
    /* Chain straight into the hold-time entry so one tap edits both. */
    keypad_config_t k = {
        .field_label = "Drying time", .unit = "h",
        .min = (float)DRY_HOLD_MIN_MS / 3600000.0f,
        .max = (float)DRY_DURATION_MAX_MS / 3600000.0f,
        .initial = (float)s_presets.e[i].hold_min / 60.0f,
        .allow_decimal = true,
        .on_ok = hold_done_cb, .user_data = (void*)(intptr_t)i,
    };
    screen_keypad_open(&k);
}

static void row_clicked_cb(lv_event_t *e) {
    unsigned i = (unsigned)(intptr_t)lv_event_get_user_data(e);
    if (i == s_selected) {
        /* Second tap on the selected row edits it. */
        keypad_config_t k = {
            .field_label = "Drying temperature", .unit = "°C",
            .min = DRY_TEMP_MIN_C, .max = DRY_TEMP_MAX_C,
            .initial = (float)s_presets.e[i].temp_c,
            .allow_decimal = false,
            .on_ok = temp_done_cb, .user_data = (void*)(intptr_t)i,
        };
        screen_keypad_open(&k);
    } else {
        s_selected = i;
        refresh_rows();
    }
}

static void start_clicked_cb(lv_event_t *e) {
    (void)e;
    cmd_msg_t m = { .type = CMD_START_DRY };
    m.payload.dry.target_c = (float)s_presets.e[s_selected].temp_c;
    m.payload.dry.hold_ms  = (uint32_t)s_presets.e[s_selected].hold_min * 60000u;
    xQueueSend(ui_cmd_queue(), &m, 0);
}

void screen_dry_create(lv_obj_t *parent) {
    lv_obj_set_style_bg_color(parent, THEME_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);

    nvs_store_load_dry_presets(&s_presets);

    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_size(list, LV_HOR_RES, LV_VER_RES - 24 - 36 - 48);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(list, THEME_BG, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    for (unsigned i = 0; i < DRY_PRESET_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(list);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 40);
        lv_obj_set_style_bg_color(btn, THEME_PANEL_BG, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, THEME_BORDER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_left(btn, 10, 0);
        lv_obj_set_style_pad_right(btn, 10, 0);
        lv_obj_add_event_cb(btn, row_clicked_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)i);
        s_rows[i] = btn;

        lv_obj_t *name = lv_label_create(btn);
        lv_label_set_text(name, dry_preset_name(i));
        lv_obj_set_style_text_color(name, THEME_FG, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *val = lv_label_create(btn);
        lv_obj_set_style_text_color(val, THEME_FG_FAINT, 0);
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);
        s_vals[i] = val;
    }

    s_start_btn = lv_button_create(parent);
    lv_obj_set_size(s_start_btn, LV_HOR_RES - 24, 40);
    lv_obj_align(s_start_btn, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_event_cb(s_start_btn, start_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(s_start_btn);
    lv_label_set_text(lbl, "START DRY");
    lv_obj_center(lbl);

    refresh_rows();
}

void screen_dry_update(const oven_state_t *snap) {
    bool idle = (snap->mode == MODE_IDLE);
    if (idle) lv_obj_clear_state(s_start_btn, LV_STATE_DISABLED);
    else      lv_obj_add_state(s_start_btn, LV_STATE_DISABLED);
}
```

Check `src/ui/theme.h` for the actual colour macro names and substitute them if
they differ from `THEME_BG` / `THEME_PANEL_BG` / `THEME_BORDER` / `THEME_FG` /
`THEME_FG_FAINT` / `THEME_ACCENT`. Add the FreeRTOS queue includes the file
needs (`freertos/FreeRTOS.h`, `freertos/queue.h`).

- [x] **Step 3: Add the tab and expose the queue**

`src/ui/ui_task.c` — add the include, create the tab between PROFILE and
settings, and route updates:

```c
#include "ui/screen_dry.h"
...
    lv_obj_t *tab_home    = lv_tabview_add_tab(tv, "HOME");
    lv_obj_t *tab_profile = lv_tabview_add_tab(tv, "PROFILE");
    lv_obj_t *tab_dry     = lv_tabview_add_tab(tv, "DRY");
    lv_obj_t *tab_settings = lv_tabview_add_tab(tv, LV_SYMBOL_SETTINGS);
...
    screen_home_create(tab_home);
    screen_profile_create(tab_profile);
    screen_dry_create(tab_dry);
    screen_settings_create(tab_settings);
```

If `ui_task.c` does not already expose its command queue, add:

```c
QueueHandle_t ui_cmd_queue(void) { return s_cmd_queue; }
```

using whatever the file's queue handle is actually called.

In the refresh timer, alongside the other per-screen updates:

```c
    screen_dry_update(&snap);
    screen_home_set_dry_view(snap.dry_active ? snap.dry_target_c : 0.0f);
```

- [x] **Step 4: Switch to HOME on a successful start**

In the same refresh path, when `dry_active` goes false→true, switch the
tabview to HOME so the run is visible:

```c
    static bool s_prev_dry = false;
    if (snap.dry_active && !s_prev_dry) lv_tabview_set_active(tv, 0, LV_ANIM_ON);
    s_prev_dry = snap.dry_active;
```

`tv` must be reachable here — store it in a file-static if it is currently a
local in the create function.

- [x] **Step 5: Build**

Run: `pio run -e esp32dev`
Expected: clean build.

- [x] **Step 6: Commit**

```bash
git add src/ui/screen_dry.h src/ui/screen_dry.c src/ui/ui_task.c
git commit -m "feat(ui): DRY tab with editable material presets

Six presets, names compile-time and values persisted. Tap to select, tap again
to edit temperature then hold time via the existing keypad. START switches to
HOME where the run renders."
```

---

### Task 13: Web page drying support

**Files:**
- Modify: `src/web/app.js`
- Modify: `tools/web_sim.py`

- [x] **Step 0: Accept the v3 API**

Task 10 bumped `TELEMETRY_API_VERSION` to 3, but the SPA pins the version it
expects and shows a mismatch banner otherwise. Until this step lands, every
browser hitting a v3 device sees that banner. Bump both pins together:

```js
const API_EXPECTED = 3;          /* src/web/app.js */
```
```python
API_VERSION = 3                  # tools/web_sim.py
```

`src/web/tuning.html.built` carries a built copy of the same constant, and
`src/web/tuning_html_gz.c` is generated from it — check how the build
regenerates them and follow that path rather than hand-editing the generated
files. Also check whether `API_EXPECTED` gates the run-archive import/export
format, and if so whether a v2 archive must still import.

- [x] **Step 1: Recognise the drying mode**

In `isLiveTelemetry` and wherever the mode drives UI state, treat `"drying"`
as a live run. In `onSample`, capture the drying head on the first sample of a
run:

```js
    if (s.dry_active) {
        curKind   = 'dry';
        curTarget = s.dry_target_c;
        curHoldMs = s.dry_hold_ms;
    }
```

- [x] **Step 2: Draw the drying reference line**

In `drawChart`, when `curKind === 'dry'`, replace the planned-curve block with
a flat target line and rescale the temperature axis — the browser keeps the
full history, so unlike the device it plots the whole run:

```js
    const Tmax = (curKind === 'dry') ? (curTarget + 20) : 250;
    ...
    if (curKind === 'dry' && curTarget > 0) {
        x.strokeStyle = '#3fb950'; x.setLineDash([4, 3]); x.lineWidth = 1.5;
        x.beginPath(); x.moveTo(pL, syT(curTarget));
        x.lineTo(W - pR, syT(curTarget)); x.stroke(); x.setLineDash([]);
    }
```

`Tmax` is currently a `const` used by both `drawChart` and `chartGeom`; make
both read the same helper so the drag hit-testing stays aligned with what is
drawn:

```js
function chartTmax() { return (curKind === 'dry') ? (curTarget + 20) : 250; }
```

and use `chartTmax()` in both functions.

- [x] **Step 3: Archive drying runs**

In `finalizeRun` / `saveRun`, record the kind and head so the file is
self-describing and analysable alongside the existing archives:

```js
    const extra = (curKind === 'dry')
        ? { kind: 'dry', dry_target_c: curTarget, dry_hold_ms: curHoldMs }
        : { kind: curKind };
```

- [x] **Step 4: Skip reflow-only analysis for dries**

`verifyRun` and `computeRunMetrics` assume a reflow profile. Guard them:

```js
    if (run.kind === 'dry') return dryMetrics(run);
```

and add:

```js
/* Drying metrics: how tightly the hold tracked, and how long it took to get
 * there. Deliberately different from the reflow metrics, which are all
 * profile-segment based. */
function dryMetrics(run) {
    const target = run.dry_target_c;
    const inBand = run.samples.filter(s => s.temp_c >= target - 2);
    const devs = inBand.map(s => Math.abs(s.temp_c - target));
    return {
        kind: 'dry',
        timeToBandS: inBand.length ? inBand[0].t_s : null,
        holdMeanDev: devs.length ? devs.reduce((a, b) => a + b, 0) / devs.length : null,
        holdMaxDev:  devs.length ? Math.max(...devs) : null,
    };
}
```

- [x] **Step 5: Show the API version**

The page displays an `api vN` chip. Update the expected version to 3 so a
stale firmware is visible rather than silently mis-parsed.

- [x] **Step 6: Verify the page rebuilds**

Run: `pio run -e esp32dev`
Expected: clean build. `tools/pio_gen_web_asset.py` runs pre-build and
regenerates `src/web/tuning_html_gz.c`, so a JS syntax error surfaces here.

If the project has web tests, also run: `npm run test:web`

- [x] **Step 7: Commit**

```bash
git add src/web/app.js
git commit -m "feat(web): drying view, kind:\"dry\" archives, drying metrics

The browser keeps full history and plots the whole dry, unlike the device's
rolling window. Archives carry target and hold duration so a long dry is
analysable alongside the existing reflow runs."
```

---

### Task 14: Hardware verification (manual — requires the maintainer and the oven)

Do NOT automate. Flash via the established OTA workflow and verify the booted
ELF SHA256 matches the build before debugging anything.

- [ ] **Step 1: Cold checks (no filament)**

1. All host tests pass: `pio test -e native`
2. Firmware builds clean: `pio run -e esp32dev`
3. Open every screen including the new DRY tab — no freeze. A silent `while(1)`
   here means the 64 KB LVGL pool overflowed.
4. DRY tab: edit a preset temperature and hold time, power-cycle, confirm the
   edit persisted; confirm the selection returns to PLA on boot.
5. START while a reflow run is active — confirm it is refused.

- [ ] **Step 2: Short functional dry**

Select PLA, edit the hold time down to ~10 minutes, START.

1. Stage chip shows `DRY · HEAT-UP`, then `DRY · HOLD m:ss / 0:10` once in band
2. **Confirm the hold timer only starts at band entry**, not at START
3. Duty settles at a low non-zero value — this is the §2.1 fix working; before
   it, the SSR would never have fired at all
4. HOME graph: Y axis scaled near the target, rolling window scrolls
5. `http://reflow.local/telemetry` shows `"mode":"drying"`, `api 3`, and the
   drying fields
6. Completion tone fires and the oven returns to IDLE
7. Export the run — confirm `kind: "dry"` in the JSON

- [ ] **Step 3: Stop and fault paths**

1. STOP mid-dry → SSR off, IDLE, `dry_end` reads `stopped`
2. With the oven still warm above target + 5 °C, confirm START is refused

- [ ] **Step 4: The long run that answers the open question**

**This is the verification that cannot be faked.** Run a full-length dry at a
moderate setpoint (PETG 65 °C is a good first choice) and watch `cj_temp_c`
to steady state.

Every run in the archive was still climbing when it ended, so `CJ_LIMIT_C`
= 75 °C is reasoned, not measured. Export the run into `dev_ressources/oven/`
and analyse it like the existing archives.

**If steady state lands near 75 °C, stop and revisit** the limit or the
venting before attempting nylon at 90 °C for 8 hours.

- [ ] **Step 5: Record the result**

Edit `docs/superpowers/specs/2026-08-30-filament-drying-mode-design.md` §11:
replace "reasoned, not measured" with the measured steady-state `cj_temp_c`,
the setpoint and duration it was measured at, and the archive filename. The
next person must read a measurement, not an assumption.

```bash
git add docs/superpowers/specs/2026-08-30-filament-drying-mode-design.md \
        dev_ressources/oven/
git commit -m "docs(spec): record measured steady-state electronics temperature

First long dry: cj_temp_c settled at <N> C at <setpoint> C over <duration>.
CJ_LIMIT_C = 75 C was reasoned from runs that never reached equilibrium; this
replaces it with a measurement."
```

---

## Self-review notes

**Spec coverage:**

| Spec section | Covered by |
|---|---|
| §0 constraints | Design constraint throughout; no new buffers in any task |
| §1 state machine | Task 5 |
| §1.1 analytic setpoint | Task 4 |
| §1.2 stage transitions + heat-up timeout | Task 5 |
| §1.3 completion / no auto-resume | Task 5 (`dry_end`) |
| §1.4 control_task + IPC | Tasks 7, 9 |
| §2 SSR resolution | Task 1 |
| §3 PID ceiling | Task 2 |
| §4.1 drying over-temp | Task 9 step 5 |
| §4.2 electronics guard | Task 8 |
| §4.3 run gating | Task 5 (`sm_start_dry`) |
| §5 persistence | Task 6 |
| §6 config.h | Tasks 1, 3, 6, 10, 11 |
| §7 telemetry v3 | Task 10 |
| §8.1 DRY tab | Task 12 |
| §8.2 HOME rendering | Task 11 |
| §8.3 fault surface | Task 8 (`fault_text`) |
| §9 web + archive format | Task 13 |
| §10 testing | Tasks 1–10 (tests precede implementation in each) |
| §11 manual verification | Task 14 |

No gaps.

**Type consistency checked:** `dry_stage_t` / `dry_end_reason_t` defined in
Task 5 and consumed in Tasks 7, 9, 10, 11. `dry_presets_t` / `dry_entry_t`
defined in Task 6, used in Task 12. `dry_setpoint()` / `dry_setpoint_rate()`
defined in Task 4, called in Tasks 5 and 9. `ssr_scheduler_should_be_on`'s
third parameter (Task 1) matches every call site. `pid_step`'s sixth parameter
(Task 2) matches both call sites (Tasks 2 and 9). `CMD_START_DRY` /
`dry_params_t` defined in Task 7, sent in Task 12, consumed in Task 9.

**Ordering note:** Tasks 1–10 are strictly test-first and independently
committable. Tasks 11–13 are UI and web, which have no host tests (excluded
from the native env by `build_src_filter`), so their verification is a clean
`pio run -e esp32dev` plus Task 14's manual checks.

**Known judgement calls flagged for the implementer:**
- Task 11 and 12 reference existing identifiers in `screen_home.c`,
  `ui_task.c` and `theme.h` generically (stage-label handle, Y-mapping helper,
  colour macros, queue handle). Read the surrounding file and use its real
  names rather than introducing new ones.
- Task 9 restores the SSR window on every fault exit path. If a future gate is
  added, it must do the same, or a subsequent reflow run inherits the 30 s
  drying window.

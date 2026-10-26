# PID Tuning Web UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a network-driven, OTA-deployable PID tuning workflow: an open-loop `MODE_STEP_TEST` heat (started/supervised at the touchscreen), a small device HTTP API, and a lean embedded vanilla-JS web app that fits the Ziegler–Nichols reaction curve and suggests gains the operator confirms on-device.

**Architecture:** The device is a thin, safe data source with a stable versioned HTTP API; all analysis/visualization/history live in the browser. `MODE_STEP_TEST` runs *inside* the existing control-task per-tick safety block so it inherits every safety gate (TC fault, stale sensor, 260 °C cutoff) and the OTA idle-reservation mutual-exclusion. Everything that ignites mains heat is started physically at the touchscreen; the browser can only observe and STOP.

**Tech Stack:** ESP-IDF (C, FreeRTOS), `esp_http_server`, LVGL (touchscreen UI), CMake `EMBED_FILES` (gzipped SPA), vanilla JS + `<canvas>` (no libraries), Unity host tests (`env:native`), Node for JS unit tests.

**Spec:** [docs/superpowers/specs/2026-05-31-pid-tuning-web-ui-design.md](../specs/2026-05-31-pid-tuning-web-ui-design.md)

**Resolved open questions (planner, 2026-05-31):**
- Step-test parameters travel in the `cmd_msg_t` payload union (new `steptest_params_t`), consistent with `CMD_APPLY_PROFILE`/`CMD_APPLY_PID`.
- Suggested gains use a **distinct** `CMD_SUGGEST_PID` command that stores a pending suggestion; the touchscreen confirm enqueues the **existing, unchanged** `CMD_APPLY_PID` (preserving its proven idle-only apply + NVS save + `pid_applied_seq` bump). Dismiss enqueues `CMD_DISMISS_PID`.
- No live-trace decimation: the browser captures at its ~1 Hz poll cadence, already at `PROFILE_CURVE_STEP_MS` (1 s) granularity.

---

## File Structure

**Firmware — control core (Phase 1):**
- `src/control/reflow_sm.h` — add `MODE_STEP_TEST`, `SM_EVENT_CMD_START_STEP_TEST`, `steptest_end_reason_t`, step-test fields on `sm_state_t`
- `src/control/reflow_sm.c` — entry guard, per-tick step-test handling helper, abort/exit transitions
- `src/oven_state.h` — `run_id`, step-test sub-status + pending-PID fields on `oven_state_t`; new command types + `steptest_params_t` payload
- `src/control/control_task.c` — step-test branch in the per-tick body, `run_id` bump, suggested-gain hold/confirm wiring
- `include/config.h` — step-test bounds/defaults + telemetry constants
- `test/test_reflow_sm/test_reflow_sm.c` — new state-machine cases

**Firmware — HTTP API (Phase 2):**
- `src/ota/ota_server.c` — `/telemetry`, `/stop`, `/suggest-gains` handlers; embedded-SPA `root_handler`; `max_uri_handlers` bump
- `src/web/tuning.html` — placeholder page embedded this phase (real SPA lands Phase 4)
- `src/web/CMakeLists.txt` or root `src/CMakeLists.txt` — embed the gzipped asset
- `src/control/control_task.{c,h}` — `control_post_cmd` helper if needed for the server task

**Firmware — touchscreen UI (Phase 3):**
- `src/ui/screen_pid_tuning.{c,h}` — new Settings sub-screen (params + START + live view + STOP + confirm prompt)
- `src/ui/screen_settings.c` — add the "PID Tuning" row

**Web SPA + JS tests (Phase 4):**
- `src/web/tuning.html` — the real single-file SPA (HTML + inline CSS + JS)
- `src/web/fit.js` — extracted pure fit math (so it is node-testable and inlined at build)
- `test/web/fit.test.mjs` — node unit tests for L/N detection + tuning-rule formulas
- `tools/build_web_asset.py` (or CMake step) — inline `fit.js` into `tuning.html`, gzip, stage for `EMBED_FILES`

---

## Phase 0 — Prerequisites & flash budget measurement

### Task 0.1: Confirm branch and build the current firmware to measure slot headroom

**Files:** none (measurement only)

- [ ] **Step 1: Confirm the working branch**

Run: `git -C ~/reflow_oven branch --show-current`
Expected: `pid-tuning-web-ui`

- [ ] **Step 2: Build the current firmware**

Run: `~/.platformio/penv/bin/pio run -e esp32dev`
Expected: build SUCCEEDS and produces `.pio/build/esp32dev/firmware.bin`.

- [ ] **Step 3: Record the binary size against the 0x170000 (1,507,328 B) slot**

Run: `ls -l .pio/build/esp32dev/firmware.bin`
Record the byte size. Compute free headroom = `1507328 − <size>`. Note it in the task notes.
Expected: a positive headroom figure (spec §6 expects ~85 KB). If headroom is < ~30 KB, STOP and raise with the user before continuing — the gzipped page (Phase 2/4, ~12–15 KB) must fit with margin.

- [ ] **Step 4: No commit** (measurement only — nothing changed).

---

## Phase 1 — Firmware control core (`MODE_STEP_TEST`, run_id, suggested-gain plumbing)

> Pure-logic and control-task changes, fully host-testable via `env:native` (`test_reflow_sm`). No network or UI yet. The control task's step-test SSR drive reuses `ssr_tick` and sits *after* the existing safety gates.

### Task 1.1: Add step-test config constants

**Files:**
- Modify: `include/config.h`

- [ ] **Step 1: Add the step-test + telemetry constants**

In `include/config.h`, after the `SAFETY LIMITS` block (after the `CONTROL_TASK_WDT_S` line), add:

```c
/* ============================================================
 * STEP TEST (open-loop PID identification) — spec 2026-05-31
 * ============================================================ */
#define STEP_TEST_DUTY_MIN          10
#define STEP_TEST_DUTY_MAX          80
#define STEP_TEST_DUTY_DEFAULT      50       /* TUNE */
#define STEP_TEST_CEILING_MIN_C     80
#define STEP_TEST_CEILING_MAX_C    200       /* stays >=60 C below OVER_TEMP_LIMIT_C */
#define STEP_TEST_CEILING_DEFAULT_C 180      /* TUNE */
#define STEP_TEST_MAX_DURATION_MS  600000    /* 10 min backstop; TUNE */

/* Telemetry HTTP API */
#define TELEMETRY_API_VERSION        1
#define TELEMETRY_POLL_HINT_MS     1000      /* advisory; browser owns cadence. TUNE */
```

- [ ] **Step 2: Add a compile-time guard tying the ceiling to the over-temp limit**

`OVER_TEMP_LIMIT_C` is a *float* literal (`260.0f`), and casting a floating constant to int inside a `_Static_assert` is not guaranteed to be a valid integer-constant-expression under strict `-std`/`-Wpedantic` (review finding F4). Add an **integer** companion constant and assert on that instead — both are plain ints, so the assert is a clean ICE:

```c
/* Integer mirror of OVER_TEMP_LIMIT_C (260.0f) for integer-constant-expression
 * guards. Keep in sync with OVER_TEMP_LIMIT_C above. */
#define OVER_TEMP_LIMIT_C_INT 260
_Static_assert((int)(OVER_TEMP_LIMIT_C) == OVER_TEMP_LIMIT_C_INT,
               "OVER_TEMP_LIMIT_C_INT out of sync with OVER_TEMP_LIMIT_C");
_Static_assert(STEP_TEST_CEILING_MAX_C <= OVER_TEMP_LIMIT_C_INT - 60,
               "STEP_TEST_CEILING_MAX_C must stay >=60 C below OVER_TEMP_LIMIT_C");
```

> The first assert (the `(int)` cast comparison) only has to hold at the planner's known value and is belt-and-braces; if a toolchain rejects even that cast, drop the first assert and keep only the integer-vs-integer ceiling guard plus a code comment. The load-bearing guard is the second one, which is pure integer arithmetic.

- [ ] **Step 3: Build host tests to confirm config.h still compiles**

Run: `~/.platformio/penv/bin/pio test -e native -f test_reflow_sm`
Expected: PASS (no behavior change yet; this just proves the header compiles under the native toolchain).

- [ ] **Step 4: Commit**

```bash
git add include/config.h
git commit -m "feat(config): add step-test bounds + telemetry API constants"
```

### Task 1.2: Extend reflow_sm types for step test

**Files:**
- Modify: `src/control/reflow_sm.h`

- [ ] **Step 1: Add the mode, event, end-reason enum, and state fields**

In `src/control/reflow_sm.h`:

Add `MODE_STEP_TEST` to `oven_mode_t` (after `MODE_PENDING_VALIDATION`):

```c
typedef enum {
    MODE_IDLE,
    MODE_RUNNING,
    MODE_FAULT,
    MODE_PENDING_VALIDATION,
    MODE_STEP_TEST,
} oven_mode_t;
```

Add the end-reason enum after `sm_validation_t`:

```c
/* Why a step test ended. STEP_END_NONE = still running or never run. */
typedef enum {
    STEP_END_NONE = 0,
    STEP_END_CEILING,       /* temp reached the configured ceiling (normal) */
    STEP_END_MAX_DURATION,  /* hit the max-duration backstop */
    STEP_END_MANUAL_STOP,   /* CMD_STOP from touchscreen or browser */
    STEP_END_FAULT,         /* a safety gate tripped */
} steptest_end_reason_t;
```

Add `SM_EVENT_CMD_START_STEP_TEST` to `sm_event_t`:

```c
typedef enum {
    SM_EVENT_NONE,
    SM_EVENT_CMD_START,
    SM_EVENT_CMD_STOP,
    SM_EVENT_CMD_ACK_FAULT,
    SM_EVENT_CMD_CLEAR_PENDING,
    SM_EVENT_CMD_START_STEP_TEST,
} sm_event_t;
```

Add step-test fields to `sm_state_t` (after `ota_reserved`):

```c
    /* Step test (open-loop PID identification). Valid only in MODE_STEP_TEST. */
    uint8_t               step_duty;        /* fixed duty %, clamped to STEP_TEST_DUTY_* */
    float                 step_ceiling_c;   /* abort temperature */
    uint32_t              step_max_ms;      /* abort elapsed time */
    steptest_end_reason_t step_end_reason;  /* set when the test ends */
```

- [ ] **Step 2: Declare the new entry helper in the header**

Add after the `sm_step` declaration:

```c
/* Begin a step test. Returns true if accepted (was idle, not reserved, not
 * gated). Clamps duty to [STEP_TEST_DUTY_MIN, STEP_TEST_DUTY_MAX]. On accept,
 * sets mode = MODE_STEP_TEST, stage = STAGE_NONE, elapsed = 0,
 * step_end_reason = STEP_END_NONE, and stores the parameters. */
bool sm_start_step_test(sm_state_t *s, uint8_t duty, float ceiling_c, uint32_t max_ms);
```

- [ ] **Step 3: Build to confirm the header compiles**

Run: `~/.platformio/penv/bin/pio test -e native -f test_reflow_sm`
Expected: PASS (the .c does not yet use the new symbols, but the header must compile).

- [ ] **Step 4: Commit**

```bash
git add src/control/reflow_sm.h
git commit -m "feat(sm): add MODE_STEP_TEST types, events, and state fields"
```

### Task 1.3: Step-test entry guard — failing test first

**Files:**
- Test: `test/test_reflow_sm/test_reflow_sm.c`

- [ ] **Step 1: Write the failing tests**

Append to `test/test_reflow_sm/test_reflow_sm.c` (before any `RUN_TEST` registration block / `main`):

```c
void test_step_test_starts_from_idle(void) {
    bool ok = sm_start_step_test(&sm, 50, 180.0f, 600000u);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(MODE_STEP_TEST, sm.mode);
    TEST_ASSERT_EQUAL(STAGE_NONE, sm.stage);
    TEST_ASSERT_EQUAL_UINT32(0, sm.elapsed_ms);
    TEST_ASSERT_EQUAL(STEP_END_NONE, sm.step_end_reason);
    TEST_ASSERT_EQUAL_UINT8(50, sm.step_duty);
}

void test_step_test_rejected_when_running(void) {
    sm_step(&sm, SM_EVENT_CMD_START, 25.0f, &curve);   /* -> MODE_RUNNING */
    bool ok = sm_start_step_test(&sm, 50, 180.0f, 600000u);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(MODE_RUNNING, sm.mode);
}

void test_step_test_rejected_when_ota_reserved(void) {
    sm.ota_reserved = true;
    bool ok = sm_start_step_test(&sm, 50, 180.0f, 600000u);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(MODE_IDLE, sm.mode);
}

void test_step_test_clamps_duty(void) {
    sm_start_step_test(&sm, 200, 180.0f, 600000u);   /* above max */
    TEST_ASSERT_EQUAL_UINT8(STEP_TEST_DUTY_MAX, sm.step_duty);
}

void test_step_test_rejected_when_fault(void) {
    sm_trip_fault(&sm, FAULT_TC_OPEN);               /* -> MODE_FAULT */
    bool ok = sm_start_step_test(&sm, 50, 180.0f, 600000u);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(MODE_FAULT, sm.mode);
}

void test_step_test_rejected_when_pending_validation(void) {
    sm.mode = MODE_PENDING_VALIDATION;
    sm.validation = SM_VALIDATION_PENDING;           /* guard: validation != NONE */
    bool ok = sm_start_step_test(&sm, 50, 180.0f, 600000u);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(MODE_PENDING_VALIDATION, sm.mode);
}
```

Register them in the test runner's `main()` (where the other `RUN_TEST(...)` lines live):

```c
    RUN_TEST(test_step_test_starts_from_idle);
    RUN_TEST(test_step_test_rejected_when_running);
    RUN_TEST(test_step_test_rejected_when_ota_reserved);
    RUN_TEST(test_step_test_clamps_duty);
    RUN_TEST(test_step_test_rejected_when_fault);
    RUN_TEST(test_step_test_rejected_when_pending_validation);
```

- [ ] **Step 2: Run to verify failure**

Run: `~/.platformio/penv/bin/pio test -e native -f test_reflow_sm`
Expected: FAIL — link error "undefined reference to `sm_start_step_test`".

### Task 1.4: Implement the step-test entry guard

**Files:**
- Modify: `src/control/reflow_sm.c`

- [ ] **Step 1: Implement `sm_start_step_test`**

Add to `src/control/reflow_sm.c` (after `sm_trip_fault`, before `active_stage`):

```c
bool sm_start_step_test(sm_state_t *s, uint8_t duty, float ceiling_c, uint32_t max_ms) {
    /* Mirror the START/OTA entry guard: idle only, not reserved for OTA, no
     * validation gate active. Starting any heat requires these same
     * preconditions as a normal reflow run. */
    if (s->mode != MODE_IDLE) return false;
    if (s->ota_reserved) return false;
    if (s->validation != SM_VALIDATION_NONE) return false;

    if (duty < STEP_TEST_DUTY_MIN) duty = STEP_TEST_DUTY_MIN;
    if (duty > STEP_TEST_DUTY_MAX) duty = STEP_TEST_DUTY_MAX;

    s->mode            = MODE_STEP_TEST;
    s->stage           = STAGE_NONE;
    s->elapsed_ms      = 0;
    s->step_duty       = duty;
    s->step_ceiling_c  = ceiling_c;
    s->step_max_ms     = max_ms;
    s->step_end_reason = STEP_END_NONE;
    return true;
}
```

- [ ] **Step 2: Run the tests to verify pass**

Run: `~/.platformio/penv/bin/pio test -e native -f test_reflow_sm`
Expected: PASS (all four new tests + existing tests).

- [ ] **Step 3: Commit**

```bash
git add src/control/reflow_sm.c test/test_reflow_sm/test_reflow_sm.c
git commit -m "feat(sm): step-test entry guard with idle/OTA/validation gating"
```

### Task 1.5: Step-test per-tick advance + aborts — failing test first

**Files:**
- Test: `test/test_reflow_sm/test_reflow_sm.c`

The control task owns the SSR and safety gates, but the *time/ceiling abort decisions* and elapsed-time bookkeeping for a step test belong in pure logic so they are testable. Add a helper `sm_step_test_tick` that advances elapsed time and reports whether the SSR should drive (true) or the test has ended (false, with `step_end_reason` set).

- [ ] **Step 1: Write the failing tests**

Append to `test/test_reflow_sm/test_reflow_sm.c`:

```c
void test_step_tick_drives_below_ceiling(void) {
    sm_start_step_test(&sm, 50, 180.0f, 600000u);
    bool drive = sm_step_test_tick(&sm, 100.0f);   /* below ceiling, within time */
    TEST_ASSERT_TRUE(drive);
    TEST_ASSERT_EQUAL(MODE_STEP_TEST, sm.mode);
    TEST_ASSERT_EQUAL_UINT32(CONTROL_TICK_MS, sm.elapsed_ms);
}

void test_step_tick_aborts_on_ceiling(void) {
    sm_start_step_test(&sm, 50, 180.0f, 600000u);
    bool drive = sm_step_test_tick(&sm, 180.0f);   /* at ceiling */
    TEST_ASSERT_FALSE(drive);
    TEST_ASSERT_EQUAL(MODE_IDLE, sm.mode);
    TEST_ASSERT_EQUAL(STEP_END_CEILING, sm.step_end_reason);
}

void test_step_tick_aborts_on_max_duration(void) {
    /* max = 2 ticks: first tick drives (elapsed 0->CONTROL_TICK_MS, still < max),
     * second tick advances to 2*CONTROL_TICK_MS >= max and aborts. This exercises
     * the genuine two-tick boundary rather than aborting on the first call. */
    sm_start_step_test(&sm, 50, 180.0f, 2u * CONTROL_TICK_MS);
    bool d1 = sm_step_test_tick(&sm, 100.0f);       /* elapsed -> CONTROL_TICK_MS */
    TEST_ASSERT_TRUE(d1);
    TEST_ASSERT_EQUAL(MODE_STEP_TEST, sm.mode);
    bool d2 = sm_step_test_tick(&sm, 100.0f);       /* elapsed -> 2*tick >= max */
    TEST_ASSERT_FALSE(d2);
    TEST_ASSERT_EQUAL(MODE_IDLE, sm.mode);
    TEST_ASSERT_EQUAL(STEP_END_MAX_DURATION, sm.step_end_reason);
}

void test_step_stop_event_ends_with_manual_reason(void) {
    sm_start_step_test(&sm, 50, 180.0f, 600000u);
    /* CMD_STOP routed through sm_step while in MODE_STEP_TEST */
    sm_step(&sm, SM_EVENT_CMD_STOP, 100.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_IDLE, sm.mode);
    TEST_ASSERT_EQUAL(STEP_END_MANUAL_STOP, sm.step_end_reason);
}

void test_step_fault_ends_with_fault_reason(void) {
    /* A safety gate tripping during a step test calls sm_trip_fault, which must
     * record STEP_END_FAULT before moving to MODE_FAULT (spec §9 gate pre-empt). */
    sm_start_step_test(&sm, 50, 180.0f, 600000u);
    sm_trip_fault(&sm, FAULT_OVER_TEMP);
    TEST_ASSERT_EQUAL(MODE_FAULT, sm.mode);
    TEST_ASSERT_EQUAL(STEP_END_FAULT, sm.step_end_reason);
}
```

Register all five with `RUN_TEST(...)` in `main()`.

- [ ] **Step 2: Run to verify failure**

Run: `~/.platformio/penv/bin/pio test -e native -f test_reflow_sm`
Expected: FAIL — "undefined reference to `sm_step_test_tick`" (and the STOP-reason test fails too).

### Task 1.6: Implement step-test tick + STOP handling

**Files:**
- Modify: `src/control/reflow_sm.h` (declare helper)
- Modify: `src/control/reflow_sm.c`

- [ ] **Step 1: Declare the tick helper in the header**

In `src/control/reflow_sm.h`, after `sm_start_step_test`:

```c
/* Advance a step test by one CONTROL_TICK_MS. Returns true if the SSR should
 * be driven at step_duty this tick; false if the test has ended (mode set back
 * to MODE_IDLE and step_end_reason populated). The CALLER must run all safety
 * gates BEFORE this — a fault path uses sm_trip_fault, which records
 * STEP_END_FAULT itself when tripping out of a step test. Pure logic; the
 * caller owns the SSR. */
bool sm_step_test_tick(sm_state_t *s, float temp_c);
```

- [ ] **Step 2: Capture STEP_END_FAULT inside `sm_trip_fault`**

So a step test that trips a safety gate records the reason in pure, host-testable logic (rather than three inline assignments in control_task.c — review spec-gap #3), amend `sm_trip_fault` to capture the end-reason BEFORE it overwrites `mode`:

```c
void sm_trip_fault(sm_state_t *s, uint8_t flag) {
    if (s->mode == MODE_STEP_TEST) s->step_end_reason = STEP_END_FAULT;
    s->mode = MODE_FAULT;
    s->stage = STAGE_NONE;
    s->fault_flags |= flag;
}
```

- [ ] **Step 3: Implement the tick and add STOP handling for MODE_STEP_TEST**

In `src/control/reflow_sm.c`, add the tick function after `sm_start_step_test`:

```c
bool sm_step_test_tick(sm_state_t *s, float temp_c) {
    if (s->mode != MODE_STEP_TEST) return false;

    /* Ceiling is the normal end-of-test. Checked before advancing time so a
     * test that starts already at/above the ceiling ends immediately. */
    if (temp_c >= s->step_ceiling_c) {
        s->mode = MODE_IDLE;
        s->stage = STAGE_NONE;
        s->step_end_reason = STEP_END_CEILING;
        return false;
    }
    s->elapsed_ms += CONTROL_TICK_MS;
    if (s->elapsed_ms >= s->step_max_ms) {
        s->mode = MODE_IDLE;
        s->stage = STAGE_NONE;
        s->step_end_reason = STEP_END_MAX_DURATION;
        return false;
    }
    return true;
}
```

In `sm_step`, add STOP handling for the step-test mode. Place this block at the *top* of `sm_step` (before the `MODE_PENDING_VALIDATION` check) so a STOP in step-test is handled deterministically and never falls through to reflow logic:

```c
    if (s->mode == MODE_STEP_TEST) {
        if (event == SM_EVENT_CMD_STOP) {
            s->mode = MODE_IDLE;
            s->stage = STAGE_NONE;
            s->step_end_reason = STEP_END_MANUAL_STOP;
        }
        /* All other events ignored here; the control task calls
         * sm_step_test_tick directly for the per-tick advance. */
        return false;
    }
```

- [ ] **Step 4: Run the tests to verify pass**

Run: `~/.platformio/penv/bin/pio test -e native -f test_reflow_sm`
Expected: PASS (all step-test tests, incl. the fault end-reason test added in Task 1.5, plus existing tests).

- [ ] **Step 5: Commit**

```bash
git add src/control/reflow_sm.c src/control/reflow_sm.h test/test_reflow_sm/test_reflow_sm.c
git commit -m "feat(sm): step-test per-tick aborts, manual stop, fault end-reason"
```

### Task 1.7: Extend oven_state.h with run_id, step sub-status, pending PID, and new commands

**Files:**
- Modify: `src/oven_state.h`

- [ ] **Step 1: Add fields to `oven_state_t`**

In `src/oven_state.h`, inside `oven_state_t` (after `pid_applied_seq`), add:

```c
    uint32_t    run_id;             /* bumped on each run start (step test + reflow); §3.1 */
    /* Step-test sub-status (valid when mode == MODE_STEP_TEST or just ended). */
    bool        step_active;
    uint8_t     step_duty;
    float       step_ceiling_c;
    uint32_t    step_max_ms;
    uint8_t     step_end_reason;    /* steptest_end_reason_t, copied as raw */
    /* Pending PID suggestion from POST /suggest-gains (touchscreen confirms). */
    bool        pending_pid_valid;
    pid_gains_t pending_pid;
```

- [ ] **Step 2: Add the new command types and the step-test payload**

In `src/oven_state.h`, extend `cmd_type_t`:

```c
typedef enum {
    CMD_START,
    CMD_STOP,
    CMD_ACK_FAULT,
    CMD_APPLY_PROFILE,
    CMD_APPLY_PID,
    CMD_CLEAR_PENDING,
    CMD_BEGIN_OTA,
    CMD_END_OTA,
    CMD_START_STEP_TEST,   /* touchscreen-only: begin an open-loop step test */
    CMD_SUGGEST_PID,       /* browser: hold a pending PID suggestion */
    CMD_DISMISS_PID,       /* touchscreen: discard the pending suggestion */
} cmd_type_t;
```

Add the parameter struct above `cmd_msg_t` and a union member:

```c
typedef struct {
    uint8_t  duty;          /* % */
    float    ceiling_c;
    uint32_t max_ms;
} steptest_params_t;

typedef struct {
    cmd_type_t type;
    union {
        profile_t         profile;   /* APPLY_PROFILE */
        pid_gains_t       pid;       /* APPLY_PID, SUGGEST_PID */
        steptest_params_t steptest;  /* START_STEP_TEST */
    } payload;
} cmd_msg_t;
```

- [ ] **Step 3: Build host tests to confirm the header compiles**

Run: `~/.platformio/penv/bin/pio test -e native -f test_reflow_sm`
Expected: PASS (header compiles; `pid_gains_t` is already included via `nvs_schema.h`).

- [ ] **Step 4: Commit**

```bash
git add src/oven_state.h
git commit -m "feat(state): run_id, step-test sub-status, pending-PID, new commands"
```

### Task 1.8: Wire the step test + suggested gains into control_task

**Files:**
- Modify: `src/control/control_task.c`

- [ ] **Step 1: Handle the new commands in `apply_non_sm_command`**

In `src/control/control_task.c`, add cases to the `switch (msg->type)` in `apply_non_sm_command` (after the `CMD_END_OTA` case, before `default`):

```c
        case CMD_SUGGEST_PID:
            /* Hold a browser-suggested gain set as pending; the touchscreen
             * confirms it. Validate now so a bad suggestion never reaches the
             * confirm prompt. Does NOT apply or save here. The published
             * oven_state_t.pending_pid IS the single source of truth — the UI
             * reads it from the snapshot and carries its own copy through the
             * confirm dialog, so no separate control-task-side copy is needed
             * (review finding F19). */
            if (pid_validate(&msg->payload.pid)) {
                xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
                s_pub->pending_pid = msg->payload.pid;
                s_pub->pending_pid_valid = true;
                xSemaphoreGive(s_pub->mutex);
            }
            break;
        case CMD_DISMISS_PID:
            xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
            s_pub->pending_pid_valid = false;
            xSemaphoreGive(s_pub->mutex);
            break;
```

> No new file-static is needed for the pending PID — the published `s_pub->pending_pid` / `pending_pid_valid` (added to `oven_state_t` in Task 1.7) is the only copy. (`CMD_APPLY_PID` on confirm carries the gains from the UI's own `s_confirm_gains`, not from the controller.)

- [ ] **Step 2: Handle START_STEP_TEST in the SM-event drain**

In `drain_commands`, add a case so the start request becomes an SM event:

```c
            case CMD_START_STEP_TEST:
                /* Stash params for the tick body; signal via a dedicated SM event. */
                s_step_params = msg.payload.steptest;
                latest = SM_EVENT_CMD_START_STEP_TEST;
                break;
```

Add the file-static near the others:

```c
static steptest_params_t s_step_params;
```

- [ ] **Step 3: Start the step test + bump run_id on the START event**

In `control_task`, the SM is stepped at `bool pid_active = sm_step(...)`. Replace the section from the `sm_step` call through the `pid_active` SSR branch with logic that handles the step-test event and mode. Specifically, immediately BEFORE the existing `sm_step` call, add step-test start handling:

```c
        /* Step-test start is a distinct entry (touchscreen-only). Handle it
         * before sm_step so the dedicated event doesn't fall through reflow
         * logic. run_id bumps on every run start (step test AND reflow). */
        if (ev == SM_EVENT_CMD_START_STEP_TEST) {
            if (sm_start_step_test(&s_sm, s_step_params.duty,
                                   s_step_params.ceiling_c, s_step_params.max_ms)) {
                s_run_id++;
            }
            ev = SM_EVENT_NONE;   /* consumed */
        }
```

Add `static uint32_t s_run_id;` with the other statics.

> **Symmetric OTA ↔ step-test exclusion (spec §1.5 / §3.2) is already enforced — no new code needed.** `sm_start_step_test` rejects when `s_sm.ota_reserved` (Task 1.4), so OTA blocks a step test. The reverse — a step test blocks an OTA reservation — is already true because `CMD_BEGIN_OTA` grants only when `s_sm.mode == MODE_IDLE` ([control_task.c:146](../../../src/control/control_task.c#L146)), and a running step test is `MODE_STEP_TEST ≠ MODE_IDLE`. Both directions hold; the on-hardware check in Task 5.2 should include attempting an OTA upload while a step test runs and confirming it is refused with 409.

There is already exactly one edge block in `control_task.c` (currently at ~line 274):

```c
        if (pid_active && !pid_active_prev) {
            pid_reset(&s_pid, temp_c);
        }
```

Add the reflow `run_id` bump INSIDE that existing block — do **not** create a second `if`:

```c
        if (pid_active && !pid_active_prev) {
            pid_reset(&s_pid, temp_c);
            s_run_id++;   /* a fresh reflow run started (§3.1) */
        }
```

After this edit, grep to confirm there is exactly one `if (pid_active && !pid_active_prev)` in the file:

Run: `grep -c "pid_active && !pid_active_prev" src/control/control_task.c`
Expected: `1`.

- [ ] **Step 4b: Drive the step test each tick (replace ONLY the final SSR branch)**

Replace the existing terminal PID/SSR branch:

```c
        float setpoint = 0.0f;
        uint8_t duty = 0;
        if (pid_active) {
            setpoint = profile_curve_setpoint(s_curve, s_sm.elapsed_ms);
            float out = pid_step(&s_pid, setpoint, temp_c, dt);
            duty = (uint8_t)out;
            ssr_tick((uint32_t)(esp_timer_get_time() / 1000), duty);
        } else {
            ssr_force_off();
        }
```

with (note: the `run_id`/`pid_reset` edge block above is left untouched — this replaces only the `setpoint`/`duty` branch):

```c
        float setpoint = 0.0f;
        uint8_t duty = 0;
        if (pid_active) {
            setpoint = profile_curve_setpoint(s_curve, s_sm.elapsed_ms);
            float out = pid_step(&s_pid, setpoint, temp_c, dt);
            duty = (uint8_t)out;
            ssr_tick((uint32_t)(esp_timer_get_time() / 1000), duty);
        } else if (s_sm.mode == MODE_STEP_TEST) {
            /* Open-loop fixed-duty drive. Safety gates above already ran and
             * would have forced off + exited on any fault. sm_step_test_tick
             * advances time and applies the ceiling/duration aborts. */
            if (sm_step_test_tick(&s_sm, temp_c)) {
                duty = s_sm.step_duty;
                ssr_tick((uint32_t)(esp_timer_get_time() / 1000), duty);
            } else {
                ssr_force_off();   /* test just ended this tick */
            }
        } else {
            ssr_force_off();
        }
```

> Step-test start (`SM_EVENT_CMD_START_STEP_TEST`, Step 3) and reflow start (the `pid_active` edge, Step 4a) are mutually exclusive paths — a step test never sets `pid_active` — so `run_id` is bumped exactly once per run from exactly one of the two sites.

- [ ] **Step 5: Publish the new fields**

In the `publish` function, add inside the mutex section (after `s_pub->mode = s_sm.mode;`):

```c
    s_pub->run_id          = s_run_id;
    s_pub->step_active     = (s_sm.mode == MODE_STEP_TEST);
    s_pub->step_duty       = s_sm.step_duty;
    s_pub->step_ceiling_c  = s_sm.step_ceiling_c;
    s_pub->step_max_ms     = s_sm.step_max_ms;
    s_pub->step_end_reason = (uint8_t)s_sm.step_end_reason;
```

- [ ] **Step 6: (no control_task change needed for the fault end-reason)**

The fault-path end-reason is now captured inside `sm_trip_fault` itself (Task 1.6 Step 2), which the three safety-gate branches already call. So there is **nothing to add** in control_task.c for this — the host test `test_step_fault_ends_with_fault_reason` (Task 1.5) covers it. This step is intentionally a no-op marker so the step count stays stable; skip to Step 7.

- [ ] **Step 7: Build the firmware**

Run: `~/.platformio/penv/bin/pio run -e esp32dev`
Expected: build SUCCEEDS. (No host test exercises control_task directly — it is ESP-IDF-only per platformio.ini `build_src_filter`. The logic it calls is covered by `test_reflow_sm`.)

- [ ] **Step 8: Commit**

```bash
git add src/control/control_task.c
git commit -m "feat(control): drive step test, bump run_id, hold/dismiss suggested PID"
```

---

## Phase 2 — HTTP API (`/telemetry`, `/stop`, `/suggest-gains`, embedded SPA)

> Adds endpoints to the **existing** `esp_http_server` in `ota_server.c`. The §1.5 OTA invariant governs this phase: `/ota`, `/version`, and `/` keep working; `max_uri_handlers` must be raised. A placeholder page is embedded now; the real SPA lands in Phase 4. The server task posts commands to the control task via a new `control_post_cmd` helper (it must not reach into the UI's `g_cmd_queue`).

### Task 2.1: Add a `control_post_cmd` helper for the server task

**Files:**
- Modify: `src/control/control_task.h`
- Modify: `src/control/control_task.c`

The OTA server already calls `control_begin_ota()`; the new `/stop` and `/suggest-gains` handlers need to enqueue `CMD_STOP` / `CMD_SUGGEST_PID`. Expose a thin, queue-handle-agnostic post helper so the server does not depend on the UI's `g_cmd_queue` global.

- [ ] **Step 1: Declare the helper**

In `src/control/control_task.h`, after `control_end_ota()`:

```c
/* Post a command to the control task's queue from any task (e.g. the HTTP
 * server). Non-blocking (timeout 0); returns true if enqueued. Safe because
 * the control task drains the whole queue each tick. */
bool control_post_cmd(const cmd_msg_t *msg);
```

- [ ] **Step 2: Implement it**

In `src/control/control_task.c`, after `control_end_ota()`:

```c
bool control_post_cmd(const cmd_msg_t *msg) {
    if (!s_cmd_queue || !msg) return false;
    return xQueueSend(s_cmd_queue, msg, 0) == pdTRUE;
}
```

- [ ] **Step 3: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32dev`
Expected: build SUCCEEDS.

- [ ] **Step 4: Commit**

```bash
git add src/control/control_task.h src/control/control_task.c
git commit -m "feat(control): control_post_cmd helper for cross-task command posting"
```

### Task 2.2: Embed a placeholder web asset via CMake (gzipped)

**Files:**
- Create: `src/web/tuning.html`
- Modify: `src/CMakeLists.txt`
- Create: `tools/build_web_asset.py`

The SPA is served gzipped (spec §6). To keep the embedded bytes reproducible and the source readable, a build step gzips `src/web/tuning.html` → `src/web/tuning.html.gz`, and CMake embeds the `.gz`. This phase uses a tiny placeholder; Phase 4 replaces the HTML content (the embed plumbing stays).

- [ ] **Step 1: Create the placeholder page**

Create `src/web/tuning.html`:

```html
<!doctype html><meta charset=utf-8><title>Reflow oven — tuning</title>
<h1>Reflow oven tuning</h1>
<p>Web UI placeholder — replaced in Phase 4.</p>
```

- [ ] **Step 2: Create the gzip helper**

Create `tools/build_web_asset.py`:

```python
#!/usr/bin/env python3
"""Gzip src/web/tuning.html -> src/web/tuning.html.gz for CMake EMBED_FILES.
Deterministic (mtime=0) so the embedded bytes only change when content does."""
import gzip, pathlib, sys

src = pathlib.Path(__file__).resolve().parent.parent / "src" / "web" / "tuning.html"
dst = src.with_suffix(".html.gz")
data = src.read_bytes()
with gzip.GzipFile(filename="", mode="wb", fileobj=open(dst, "wb"), mtime=0) as f:
    f.write(data)
print(f"{src.name} ({len(data)} B) -> {dst.name} ({dst.stat().st_size} B)")
```

- [ ] **Step 3: Generate the .gz once**

Run: `python3 tools/build_web_asset.py`
Expected: prints the uncompressed and compressed sizes; creates `src/web/tuning.html.gz`.

- [ ] **Step 4: Embed the .gz in CMake AND auto-generate it as a build dependency**

The current `src/CMakeLists.txt` uses `FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)` which would sweep `tuning.html`/`.gz`/`.yml`/this `CMakeLists.txt` into SRCS. Narrow the glob to `*.c`, embed the gz, and ensure the gz **exists at configure time** (via `execute_process`) so a clean checkout — where `tuning.html.gz` is gitignored and absent — doesn't fail when `idf_component_register` evaluates `EMBED_FILES`. An `add_custom_command` then keeps it regenerated on later edits (fixes both the missing-on-clean-checkout and the silently-stale-page risks, review findings #12/F12/F25). Replace `src/CMakeLists.txt` with:

```cmake
# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)

set(WEB_DIR ${CMAKE_SOURCE_DIR}/src/web)
set(WEB_GZ  ${WEB_DIR}/tuning.html.gz)

# (1) Configure-time generation: guarantees ${WEB_GZ} exists before
#     idf_component_register evaluates EMBED_FILES, even on a fresh checkout
#     where the gitignored .gz is absent.
execute_process(
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/build_web_asset.py
    RESULT_VARIABLE _web_rc)
if(NOT _web_rc EQUAL 0)
    message(FATAL_ERROR "build_web_asset.py failed (rc=${_web_rc})")
endif()

# (2) Build-time regeneration: rebuilds the asset whenever a source changes so
#     the embedded page never goes stale. DEPENDS lists only files that exist
#     in THIS phase; Task 4.5 adds fit.js/app.js to this list when they land.
add_custom_command(
    OUTPUT  ${WEB_GZ}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/build_web_asset.py
    DEPENDS ${WEB_DIR}/tuning.html ${CMAKE_SOURCE_DIR}/tools/build_web_asset.py
    COMMENT "Inlining + gzipping tuning web UI"
    VERBATIM)
add_custom_target(web_asset DEPENDS ${WEB_GZ})

idf_component_register(
    SRCS ${app_sources}
    EMBED_FILES ${WEB_GZ})
target_include_directories(${COMPONENT_LIB} PRIVATE ${REFLOW_GEN_INCLUDE_DIR})
add_dependencies(${COMPONENT_LIB} web_asset)
```

> Notes: (1) the glob is narrowed from `*.*` to `*.c` — there are no `.cpp`/`.S`/non-`.c` sources in `src/` (verified), preventing the `.gz`/`.html`/`.yml`/`CMakeLists.txt` from being mis-compiled as a source. (2) The Phase-2 `DEPENDS` lists ONLY `tuning.html` + the script, because `fit.js`/`app.js` don't exist yet; **Task 4.5 Step 1b extends this `DEPENDS`** once they land. The configure-time `execute_process` is what makes a clean build work regardless. (3) `build_web_asset.py` in Phase 2 only gzips `tuning.html` (no inlining yet); Task 4.5 upgrades the script to inline `fit.js`/`app.js` first.

- [ ] **Step 5: Add a `.gitignore` entry for the generated artifact**

Append to the repo root `.gitignore`:

```
src/web/*.html.gz
```

- [ ] **Step 6: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32dev`
Expected: build SUCCEEDS; linker generates `_binary_tuning_html_gz_start`/`_end` symbols.

- [ ] **Step 7: Commit**

```bash
git add src/web/tuning.html tools/build_web_asset.py src/CMakeLists.txt .gitignore
git commit -m "build: embed gzipped web asset via CMake EMBED_FILES (placeholder page)"
```

### Task 2.3: Serve the embedded gzipped SPA from `root_handler`

**Files:**
- Modify: `src/ota/ota_server.c`

- [ ] **Step 1: Reference the embedded symbols**

In `src/ota/ota_server.c`, in the `#ifdef ESP_PLATFORM` include region (after the existing externs/statics near the top), add:

```c
extern const uint8_t tuning_html_gz_start[] asm("_binary_tuning_html_gz_start");
extern const uint8_t tuning_html_gz_end[]   asm("_binary_tuning_html_gz_end");
```

- [ ] **Step 2: Replace `root_handler` to serve the gzipped asset**

Replace the existing `root_handler` body with:

```c
static esp_err_t root_handler(httpd_req_t *req) {
    const size_t len = tuning_html_gz_end - tuning_html_gz_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)tuning_html_gz_start, len);
}
```

- [ ] **Step 3: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32dev`
Expected: build SUCCEEDS.

- [ ] **Step 4: Commit**

```bash
git add src/ota/ota_server.c
git commit -m "feat(server): serve embedded gzipped SPA from GET /"
```

### Task 2.4: Add `GET /telemetry`

**Files:**
- Modify: `src/ota/ota_server.c`

- [ ] **Step 1: Add the telemetry handler**

The handler needs a read-only snapshot of `oven_state_t`. Add a small accessor in `control_task` rather than exposing the global. In `src/control/control_task.h`:

```c
/* Copy the current published oven_state_t snapshot under its mutex. Safe from
 * any task; for read-only consumers like the telemetry endpoint. */
void control_get_snapshot(oven_state_t *out);
```

In `src/control/control_task.c` (after `control_post_cmd`):

```c
void control_get_snapshot(oven_state_t *out) {
    if (!s_pub || !out) return;
    xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
    *out = *s_pub;
    xSemaphoreGive(s_pub->mutex);
    out->mutex = NULL;   /* don't leak the handle to callers */
    out->curve = NULL;   /* pointer not valid outside the lock */
}
```

The JSON **string-building is extracted into a pure, host-compilable helper** (`telemetry_json`) so its exact shape is unit-testable under `env:native` without the httpd/FreeRTOS stack (spec §9 telemetry-shape test; review spec-gap #6). The helper takes a plain-data struct (no `SemaphoreHandle_t`, no FreeRTOS headers), so it compiles on host. Create `src/ota/telemetry_json.h`:

```c
/* Pure telemetry JSON serialiser — no FreeRTOS / httpd deps, host-testable.
 * Writes the GET /telemetry body into `buf`. Returns the strlen written
 * (excluding NUL), or -1 on truncation. */
#pragma once
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    int      api;
    const char *mode;          /* already-resolved mode string */
    float    temp_c, setpoint_c;
    unsigned duty, elapsed_ms, fault, run_id;
    bool     step_active;
    unsigned step_duty;
    float    step_ceiling_c;
    unsigned step_max_ms;
    const char *step_end;      /* already-resolved end-reason string */
    bool     pending_pid_valid;
    float    kp, ki, kd;
} telemetry_view_t;

int telemetry_json(const telemetry_view_t *v, char *buf, size_t n);
```

Create `src/ota/telemetry_json.c` (host-built — NOT under `#ifdef ESP_PLATFORM`):

```c
#include "ota/telemetry_json.h"
#include <stdio.h>

int telemetry_json(const telemetry_view_t *v, char *buf, size_t n) {
    int w = snprintf(buf, n,
        "{\"api\":%d,\"mode\":\"%s\",\"temp_c\":%.2f,\"setpoint_c\":%.2f,"
        "\"duty\":%u,\"elapsed_ms\":%u,\"fault\":%u,\"run_id\":%u,"
        "\"steptest\":{\"active\":%s,\"duty\":%u,\"ceiling_c\":%.1f,"
        "\"max_ms\":%u,\"end_reason\":\"%s\"},",
        v->api, v->mode, v->temp_c, v->setpoint_c,
        v->duty, v->elapsed_ms, v->fault, v->run_id,
        v->step_active ? "true" : "false", v->step_duty, v->step_ceiling_c,
        v->step_max_ms, v->step_end);
    if (w < 0 || (size_t)w >= n) return -1;            /* truncated */
    int w2 = v->pending_pid_valid
        ? snprintf(buf + w, n - w, "\"pending_pid\":{\"kp\":%.3f,\"ki\":%.3f,\"kd\":%.3f}}",
                   v->kp, v->ki, v->kd)
        : snprintf(buf + w, n - w, "\"pending_pid\":null}");
    if (w2 < 0 || (size_t)(w + w2) >= n) return -1;
    return w + w2;
}
```

In `src/ota/ota_server.c`, add the mode/end-reason string mappers and the handler that fills the view and calls the helper (after `version_handler`):

```c
#include "ota/telemetry_json.h"

static const char *mode_str(oven_mode_t m) {
    switch (m) {
        case MODE_IDLE:               return "idle";
        case MODE_RUNNING:            return "running";
        case MODE_FAULT:              return "fault";
        case MODE_PENDING_VALIDATION: return "pending_validation";
        case MODE_STEP_TEST:          return "step_test";
        default:                      return "unknown";
    }
}
static const char *step_end_str(uint8_t r) {
    switch (r) {
        case STEP_END_CEILING:      return "ceiling";
        case STEP_END_MAX_DURATION: return "max_duration";
        case STEP_END_MANUAL_STOP:  return "manual_stop";
        case STEP_END_FAULT:        return "fault";
        default:                    return "none";
    }
}

static esp_err_t telemetry_handler(httpd_req_t *req) {
    oven_state_t s;
    control_get_snapshot(&s);
    telemetry_view_t v = {
        .api = TELEMETRY_API_VERSION, .mode = mode_str(s.mode),
        .temp_c = s.temp_c, .setpoint_c = s.setpoint_c,
        .duty = s.duty_percent, .elapsed_ms = s.elapsed_ms, .fault = s.fault_flags,
        .run_id = s.run_id, .step_active = s.step_active, .step_duty = s.step_duty,
        .step_ceiling_c = s.step_ceiling_c, .step_max_ms = s.step_max_ms,
        .step_end = step_end_str(s.step_end_reason),
        .pending_pid_valid = s.pending_pid_valid,
        .kp = s.pending_pid.kp, .ki = s.pending_pid.ki, .kd = s.pending_pid.kd,
    };
    char body[384];
    if (telemetry_json(&v, body, sizeof body) < 0) {
        /* Should never happen for the bounded field set; fail safe with a
         * minimal valid object rather than emitting a truncated one. */
        snprintf(body, sizeof body, "{\"api\":%d,\"mode\":\"unknown\"}", TELEMETRY_API_VERSION);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}
```

`oven_state.h` is already pulled in transitively via `control_task.h`. The `telemetry_json.c`/`.h` pair has no FreeRTOS/httpd deps, so it builds under both `esp32dev` and `native`.

- [ ] **Step 2: Register the route**

In `ota_server_start`, raise the handler limit and register `/telemetry`:

Replace `cfg.max_uri_handlers = 4;` with a named constant so the route-count guard (Task 2.7) can reference it. Add near the top of the file (with the other `#define`s / statics in the `ESP_PLATFORM` block):

```c
/* Routes registered in ota_server_start: /, /version, /ota, /telemetry,
 * /stop, /suggest-gains, /profile. Keep OTA_MAX_URI_HANDLERS >= OTA_ROUTE_COUNT. */
#define OTA_ROUTE_COUNT        7
#define OTA_MAX_URI_HANDLERS   8
```

and set `cfg.max_uri_handlers = OTA_MAX_URI_HANDLERS;`.

After the existing `httpd_register_uri_handler(s_server, &ota);`, add:

```c
    httpd_uri_t telemetry = { .uri="/telemetry", .method=HTTP_GET, .handler=telemetry_handler };
    httpd_register_uri_handler(s_server, &telemetry);
```

- [ ] **Step 3: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32dev`
Expected: build SUCCEEDS.

- [ ] **Step 4: Commit**

```bash
git add src/ota/ota_server.c src/ota/telemetry_json.h src/ota/telemetry_json.c \
        src/control/control_task.h src/control/control_task.c
git commit -m "feat(server): GET /telemetry via host-testable telemetry_json helper"
```

### Task 2.5: Add `POST /stop`

**Files:**
- Modify: `src/ota/ota_server.c`

- [ ] **Step 1: Add the handler**

In `src/ota/ota_server.c`, add a token check + enqueue (after `telemetry_handler`):

```c
/* Shared token check: returns ESP_OK if the X-OTA-Token header matches the
 * stored token; otherwise sends a 401 and returns ESP_FAIL. Mirrors the
 * inline check in ota_handler so /stop and /suggest-gains reuse it. */
static esp_err_t require_token(httpd_req_t *req) {
    char tok[OTA_TOKEN_HEX_LEN + 1] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Token", tok, sizeof tok) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "missing token");
        return ESP_FAIL;
    }
    char expected[OTA_TOKEN_HEX_LEN + 1] = {0};
    if (!nvs_store_get_ota_token(expected) ||
        !ota_token_eq(tok, expected, OTA_TOKEN_HEX_LEN)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad token");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t stop_handler(httpd_req_t *req) {
    if (require_token(req) != ESP_OK) return ESP_OK;  /* 401 already sent */
    cmd_msg_t m = { .type = CMD_STOP };
    control_post_cmd(&m);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"stopping\"}", HTTPD_RESP_USE_STRLEN);
}
```

> **Refactor note (DRY):** `ota_handler` currently inlines the same token check at [src/ota/ota_server.c](../../../src/ota/ota_server.c) (the `X-OTA-Token` block). Leave `ota_handler` as-is in this task to avoid touching the OTA path mid-phase (§1.5). A later optional cleanup can route it through `require_token`; do NOT bundle that here.

- [ ] **Step 2: Register the route**

After the `/telemetry` registration:

```c
    httpd_uri_t stop = { .uri="/stop", .method=HTTP_POST, .handler=stop_handler };
    httpd_register_uri_handler(s_server, &stop);
```

- [ ] **Step 3: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32dev`
Expected: build SUCCEEDS.

- [ ] **Step 4: Commit**

```bash
git add src/ota/ota_server.c
git commit -m "feat(server): POST /stop (token-authed) enqueues CMD_STOP"
```

### Task 2.6: Add `POST /suggest-gains`

**Files:**
- Modify: `src/ota/ota_server.c`

- [ ] **Step 1: Add the handler**

Parse a tiny JSON body `{"kp":..,"ki":..,"kd":..}`. To avoid pulling a JSON parser, read the body into a stack buffer and scan with `sscanf`-style extraction. Add after `stop_handler`:

```c
/* Extract a float field like "kp": 1.23 from a small JSON buffer. Returns
 * true and sets *out on success. Tolerates spaces; not a general parser —
 * the body is device-generated by our own SPA and bounded. */
static bool json_get_float(const char *buf, const char *key, float *out) {
    const char *p = strstr(buf, key);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    return sscanf(p + 1, " %f", out) == 1;
}

static esp_err_t suggest_gains_handler(httpd_req_t *req) {
    if (require_token(req) != ESP_OK) return ESP_OK;   /* 401 already sent */
    if (req->content_len <= 0 || req->content_len > 127) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body length");
    }
    char buf[128] = {0};
    int r = httpd_req_recv(req, buf, req->content_len);
    if (r <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
    buf[r] = '\0';

    pid_gains_t g;
    if (!json_get_float(buf, "\"kp\"", &g.kp) ||
        !json_get_float(buf, "\"ki\"", &g.ki) ||
        !json_get_float(buf, "\"kd\"", &g.kd)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing kp/ki/kd");
    }
    if (!pid_validate(&g)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "gains out of range");
    }
    cmd_msg_t m = { .type = CMD_SUGGEST_PID, .payload.pid = g };
    control_post_cmd(&m);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"pending_confirm\"}", HTTPD_RESP_USE_STRLEN);
}
```

Ensure `#include "storage/nvs_schema.h"` (for `pid_gains_t`/`pid_validate`) is present in the `#ifdef ESP_PLATFORM` includes; add it if not.

- [ ] **Step 2: Register the route**

After the `/stop` registration:

```c
    httpd_uri_t suggest = { .uri="/suggest-gains", .method=HTTP_POST, .handler=suggest_gains_handler };
    httpd_register_uri_handler(s_server, &suggest);
```

- [ ] **Step 3: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32dev`
Expected: build SUCCEEDS.

- [ ] **Step 4: Commit**

```bash
git add src/ota/ota_server.c
git commit -m "feat(server): POST /suggest-gains validates + holds pending PID"
```

### Task 2.7: OTA-invariant regression check (handler registration)

**Files:**
- Modify: `src/ota/ota_server.c`

A full httpd integration test is impractical host-side, but we can guard the most likely regression: that the number of registered routes does not exceed `max_uri_handlers`. Encode the route count as a checked constant.

- [ ] **Step 1: Add a meaningful compile-time guard in `ota_server.c`**

At file scope (not inside a function — `_Static_assert` on these `#define`s is constant-foldable), after the `OTA_ROUTE_COUNT` / `OTA_MAX_URI_HANDLERS` defines from Task 2.4:

```c
/* §1.5 invariant: the server must allow at least as many URI handlers as we
 * register, or httpd_register_uri_handler() silently fails for the overflow
 * route. This guard fails the build if a future route bumps OTA_ROUTE_COUNT
 * past the configured limit without raising OTA_MAX_URI_HANDLERS too. */
_Static_assert(OTA_ROUTE_COUNT <= OTA_MAX_URI_HANDLERS,
               "raise OTA_MAX_URI_HANDLERS to fit all registered routes");
```

> Unlike a literal `7 <= 8`, this references the same constants that drive both the registration count (documented) and `cfg.max_uri_handlers`, so adding an 8th route (→ bump `OTA_ROUTE_COUNT`) fails the build until the limit is raised. The real end-to-end OTA regression is still the on-hardware gate in Task 5.1.

- [ ] **Step 2: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32dev`
Expected: build SUCCEEDS.

- [ ] **Step 3: Commit**

```bash
git add src/ota/ota_server.c
git commit -m "test(server): static guard that route count fits max_uri_handlers"
```

### Task 2.8: Host test for the telemetry JSON shape

**Files:**
- Test: `test/test_telemetry_json/test_telemetry_json.c` (host, `env:native`)

The `telemetry_json` helper (Task 2.4) is pure and host-compilable, so the exact `/telemetry` body shape — the spec §2.4 contract the browser depends on — is unit-testable without the httpd stack (spec §9).

- [ ] **Step 1: Write the failing test**

Create `test/test_telemetry_json/test_telemetry_json.c`:

```c
#include <unity.h>
#include <string.h>
#include "ota/telemetry_json.h"

void setUp(void) {}
void tearDown(void) {}

void test_idle_shape_has_all_fields_and_null_pending(void) {
    telemetry_view_t v = {
        .api = 1, .mode = "idle", .temp_c = 24.5f, .setpoint_c = 0.0f,
        .duty = 0, .elapsed_ms = 0, .fault = 0, .run_id = 0,
        .step_active = false, .step_duty = 0, .step_ceiling_c = 0.0f,
        .step_max_ms = 0, .step_end = "none", .pending_pid_valid = false,
    };
    char buf[384];
    int n = telemetry_json(&v, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"api\":1"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mode\":\"idle\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"run_id\":0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"steptest\":{"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"end_reason\":\"none\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pending_pid\":null}"));
}

void test_step_test_active_with_pending_pid(void) {
    telemetry_view_t v = {
        .api = 1, .mode = "step_test", .temp_c = 150.0f, .setpoint_c = 0.0f,
        .duty = 50, .elapsed_ms = 42000, .fault = 0, .run_id = 3,
        .step_active = true, .step_duty = 50, .step_ceiling_c = 180.0f,
        .step_max_ms = 600000, .step_end = "none", .pending_pid_valid = true,
        .kp = 8.40f, .ki = 0.19f, .kd = 46.0f,
    };
    char buf[384];
    int n = telemetry_json(&v, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mode\":\"step_test\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"active\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"run_id\":3"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pending_pid\":{\"kp\":8.400"));
    /* well-formed: ends with the closing brace of the object */
    TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);
}

void test_truncation_returns_negative(void) {
    telemetry_view_t v = { .api = 1, .mode = "idle", .step_end = "none" };
    char tiny[16];
    TEST_ASSERT_EQUAL_INT(-1, telemetry_json(&v, tiny, sizeof tiny));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_idle_shape_has_all_fields_and_null_pending);
    RUN_TEST(test_step_test_active_with_pending_pid);
    RUN_TEST(test_truncation_returns_negative);
    return UNITY_END();
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `~/.platformio/penv/bin/pio test -e native -f test_telemetry_json`
Expected: FAIL — `telemetry_json.c` not yet compiled into native, or (if Task 2.4 already added it) PASS. If Task 2.4 is already merged, this test should PASS immediately; the point is it now exists and guards the shape.

> Note: the native `build_src_filter` is `+<*>` minus a specific exclusion list (main.c, drivers, control_task.c, ui/, etc.). `src/ota/telemetry_json.c` is NOT excluded, so `+<*>` picks it up automatically — no `platformio.ini` change is required for it to be host-built. (`src/ota/ota_server.c` is also globbed in by `+<*>`, but its entire ESP-only body is under `#ifdef ESP_PLATFORM`, so on native it reduces to just the `ota_token_eq` host fallback and compiles cleanly — it does not pull esp_http_server on host.) Therefore the `git add platformio.ini` in Step 3 is only needed if you discover a filter tweak is actually required; otherwise drop it from the commit.

- [ ] **Step 3: Confirm pass + commit**

Run: `~/.platformio/penv/bin/pio test -e native -f test_telemetry_json`
Expected: PASS.

```bash
git add test/test_telemetry_json/test_telemetry_json.c platformio.ini
git commit -m "test(server): host test for /telemetry JSON shape contract"
```

### Task 2.9: Add `GET /profile` (planned curve, for the verification overlay)

**Files:**
- Modify: `src/control/control_task.h`, `src/control/control_task.c`
- Modify: `src/ota/ota_server.c`

The verification view (spec §2.4, success criterion 5) draws the full planned reflow curve *ahead* so the live achieved trace visibly chases it. The curve is large (~1545 floats at 1 s spacing) and changes only on APPLY_PROFILE, so it is served on its **own** endpoint fetched once per run — never on the 1 Hz telemetry path.

- [ ] **Step 1: Add a curve accessor on the control task**

The planned curve lives in `s_curve` (a `profile_curve_t *`) owned by the control task. Expose a copy-under-no-lock snapshot — the curve pointer is only swapped in `MODE_IDLE` during APPLY_PROFILE (control_task.c comment at the `s_curve` declaration), and `/profile` is fetched at run start (not idle), so the curve is stable while copied. To be safe we copy the length+samples we need. In `src/control/control_task.h`:

```c
/* Copy the active planned curve (setpoint samples at PROFILE_CURVE_STEP_MS).
 * Writes up to *io_len samples into out_temps[] and the phase-end marks into
 * out_phase_ms[STAGE_COUNT_]; sets *io_len to the actual count. Safe to call
 * from the HTTP server task. */
void control_get_curve(float *out_temps, uint16_t *io_len, uint32_t *out_phase_ms);
```

In `src/control/control_task.c` (after `control_get_snapshot`):

```c
void control_get_curve(float *out_temps, uint16_t *io_len, uint32_t *out_phase_ms) {
    if (!s_curve || !out_temps || !io_len) { if (io_len) *io_len = 0; return; }
    xSemaphoreTake(s_pub->mutex, portMAX_DELAY);   /* same mutex guards curve swap publication */
    uint16_t n = s_curve->length;
    if (n > *io_len) n = *io_len;
    for (uint16_t i = 0; i < n; i++) out_temps[i] = s_curve->samples[i];
    if (out_phase_ms)
        for (int s = 0; s < STAGE_COUNT_; s++) out_phase_ms[s] = s_curve->phase_end_ms[s];
    xSemaphoreGive(s_pub->mutex);
    *io_len = n;
}
```

> The publish() path already memcpys `s_curve->samples`/`phase_end_ms` into `s_pub` under the mutex every tick (existing code), so taking `s_pub->mutex` here serialises against that and against the APPLY_PROFILE republish. This reuses the existing lock rather than adding a new one.

- [ ] **Step 2: Add the handler (streamed in chunks to avoid a huge stack buffer)**

A 1545-float JSON array is ~10 KB — too large for a single stack buffer on the httpd worker. Use `httpd_resp_send_chunk` to stream it. In `src/ota/ota_server.c` (after `telemetry_handler`):

```c
#include "control/profile_curve.h"   /* PROFILE_CURVE_MAX_SAMPLES, STAGE_COUNT_, PROFILE_CURVE_STEP_MS via config */

static esp_err_t profile_handler(httpd_req_t *req) {
    static float    temps[PROFILE_CURVE_MAX_SAMPLES];   /* static: too big for the httpd stack */
    static uint32_t phase_ms[STAGE_COUNT_];
    uint16_t len = PROFILE_CURVE_MAX_SAMPLES;
    control_get_curve(temps, &len, phase_ms);

    httpd_resp_set_type(req, "application/json");
    char head[160];
    int n = snprintf(head, sizeof head,
        "{\"api\":%d,\"step_ms\":%d,\"len\":%u,\"phase_end_ms\":[",
        TELEMETRY_API_VERSION, PROFILE_CURVE_STEP_MS, (unsigned)len);
    httpd_resp_send_chunk(req, head, n);
    for (int s = 0; s < STAGE_COUNT_; s++) {
        char b[16]; int m = snprintf(b, sizeof b, "%s%u", s ? "," : "", (unsigned)phase_ms[s]);
        httpd_resp_send_chunk(req, b, m);
    }
    httpd_resp_send_chunk(req, "],\"temp_c\":[", 11);
    for (uint16_t i = 0; i < len; i++) {
        char b[16]; int m = snprintf(b, sizeof b, "%s%.1f", i ? "," : "", temps[i]);
        httpd_resp_send_chunk(req, b, m);
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);   /* terminate the chunked response */
    return ESP_OK;
}
```

> The two `static` arrays (≈6.4 KB + a few bytes) live in BSS, not on the 8 KB httpd worker stack (finding #7 from the OTA work — the same stack-frame concern). They are only read here; concurrent `/profile` requests are serialised by the single httpd worker, so a shared static is safe.

- [ ] **Step 3: Register the route**

After the `/suggest-gains` registration in `ota_server_start`:

```c
    httpd_uri_t profile = { .uri="/profile", .method=HTTP_GET, .handler=profile_handler };
    httpd_register_uri_handler(s_server, &profile);
```

- [ ] **Step 4: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32dev`
Expected: build SUCCEEDS.

- [ ] **Step 5: Commit**

```bash
git add src/ota/ota_server.c src/control/control_task.h src/control/control_task.c
git commit -m "feat(server): GET /profile streams the planned curve for the verification overlay"
```

---

## Phase 3 — Touchscreen UI (Settings → PID Tuning sub-screen)

> A new Settings sub-screen following the established `settings_make_subscreen` + lazy-create pattern (see `screen_pid.c` / `screen_settings.c`). It enters step parameters, STARTs the test (the **only** way to ignite the open-loop heat), shows the live temperature + a STOP button while running, and surfaces the pending-PID confirm prompt. All UI work runs on the LVGL task; commands go out via `g_cmd_queue` (the existing UI→control channel), snapshots come in via the per-screen `_update(&snap)` hook.

### Task 3.1: Create the PID Tuning sub-screen skeleton (params + START)

**Files:**
- Create: `src/ui/screen_pid_tuning.h`
- Create: `src/ui/screen_pid_tuning.c`
- Modify: `src/ui/screen_settings.c`

- [ ] **Step 1: Create the header**

Create `src/ui/screen_pid_tuning.h`:

```c
/* Settings → PID Tuning sub-screen. Enters step-test parameters, starts the
 * open-loop step test (touchscreen-only), shows the live rise + STOP while it
 * runs, and surfaces the browser-suggested-gain confirm prompt. See spec
 * 2026-05-31 §2.2 / §3 / §4.
 */
#pragma once
#include "lvgl.h"
#include "oven_state.h"

/* Lazy-created on first navigation (matches the other Settings sub-screens). */
lv_obj_t *screen_pid_tuning_create(void);

/* Called every UI tick from ui_refresh_timer with the latest snapshot.
 * No-op if the sub-screen has not been created yet. */
void screen_pid_tuning_update(const oven_state_t *snap);
```

- [ ] **Step 2: Create the implementation (params + START; live view added next task)**

Create `src/ui/screen_pid_tuning.c`:

```c
#include "ui/screen_pid_tuning.h"
#include "ui/screen_settings.h"
#include "ui/screen_keypad.h"
#include "ui/theme.h"
#include "oven_state.h"
#include "config.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <stdint.h>

extern QueueHandle_t g_cmd_queue;

/* Editable step-test parameters (touchscreen-entered). */
static uint8_t  s_duty     = STEP_TEST_DUTY_DEFAULT;
static float    s_ceiling  = STEP_TEST_CEILING_DEFAULT_C;
static uint32_t s_max_ms   = STEP_TEST_MAX_DURATION_MS;

static lv_obj_t *s_duty_lbl, *s_ceiling_lbl, *s_maxmin_lbl;
static lv_obj_t *s_start_btn, *s_stop_btn, *s_live_lbl;
static bool      s_created;

static void refresh_param_labels(void) {
    char b[24];
    snprintf(b, sizeof b, "%u %%", (unsigned)s_duty);
    lv_label_set_text(s_duty_lbl, b);
    snprintf(b, sizeof b, "%.0f \xC2\xB0""C", s_ceiling);
    lv_label_set_text(s_ceiling_lbl, b);
    snprintf(b, sizeof b, "%lu min", (unsigned long)(s_max_ms / 60000u));
    lv_label_set_text(s_maxmin_lbl, b);
}

static void duty_done(float v, void *u) { (void)u; s_duty = (uint8_t)v; refresh_param_labels(); }
static void ceiling_done(float v, void *u) { (void)u; s_ceiling = v; refresh_param_labels(); }
static void maxmin_done(float v, void *u) { (void)u; s_max_ms = (uint32_t)(v * 60000.0f); refresh_param_labels(); }

static void duty_clicked(lv_event_t *e) {
    (void)e;
    keypad_config_t c = { .field_label="Step duty", .unit="%", .min=STEP_TEST_DUTY_MIN,
        .max=STEP_TEST_DUTY_MAX, .initial=s_duty, .allow_decimal=false, .on_ok=duty_done };
    screen_keypad_open(&c);
}
static void ceiling_clicked(lv_event_t *e) {
    (void)e;
    keypad_config_t c = { .field_label="Ceiling", .unit="\xC2\xB0""C", .min=STEP_TEST_CEILING_MIN_C,
        .max=STEP_TEST_CEILING_MAX_C, .initial=s_ceiling, .allow_decimal=false, .on_ok=ceiling_done };
    screen_keypad_open(&c);
}
static void maxmin_clicked(lv_event_t *e) {
    (void)e;
    /* Entered in minutes (1..15); STEP_TEST_MAX_DURATION_MS is the default 10 min. */
    keypad_config_t c = { .field_label="Max duration", .unit="min", .min=1,
        .max=15, .initial=(float)(s_max_ms / 60000u), .allow_decimal=false, .on_ok=maxmin_done };
    screen_keypad_open(&c);
}

static void start_clicked(lv_event_t *e) {
    (void)e;
    cmd_msg_t m = { .type = CMD_START_STEP_TEST };
    m.payload.steptest.duty = s_duty;
    m.payload.steptest.ceiling_c = s_ceiling;
    m.payload.steptest.max_ms = s_max_ms;
    xQueueSend(g_cmd_queue, &m, 0);
}
static void stop_clicked(lv_event_t *e) {
    (void)e;
    cmd_msg_t m = { .type = CMD_STOP };
    xQueueSend(g_cmd_queue, &m, 0);
}

/* A tappable param row: <label> [value]. */
static lv_obj_t *make_param_row(lv_obj_t *parent, int y, const char *label,
                                lv_event_cb_t cb) {
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, LV_HOR_RES - 16, 40);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_bg_color(row, THEME_PANEL_BG, 0);
    lv_obj_set_style_border_color(row, THEME_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, THEME_FG_DIM, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *v = lv_label_create(row);
    lv_obj_set_style_text_color(v, THEME_ACCENT, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -4, 0);
    return v;
}

lv_obj_t *screen_pid_tuning_create(void) {
    lv_obj_t *sub = settings_make_subscreen("PID Tuning");
    int y = 40;
    s_duty_lbl    = make_param_row(sub, y, "Step duty",    duty_clicked);    y += 48;
    s_ceiling_lbl = make_param_row(sub, y, "Ceiling",      ceiling_clicked); y += 48;
    s_maxmin_lbl  = make_param_row(sub, y, "Max duration", maxmin_clicked);  y += 56;

    s_start_btn = lv_button_create(sub);
    lv_obj_set_size(s_start_btn, LV_HOR_RES - 16, 48);
    lv_obj_set_pos(s_start_btn, 0, y);
    lv_obj_set_style_bg_color(s_start_btn, THEME_ACCENT, 0);
    lv_obj_add_event_cb(s_start_btn, start_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(s_start_btn);
    lv_label_set_text(sl, "START step test");
    lv_obj_center(sl);

    s_stop_btn = lv_button_create(sub);
    lv_obj_set_size(s_stop_btn, LV_HOR_RES - 16, 48);
    lv_obj_set_pos(s_stop_btn, 0, y);
    lv_obj_set_style_bg_color(s_stop_btn, THEME_STOP, 0);
    lv_obj_add_event_cb(s_stop_btn, stop_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *stl = lv_label_create(s_stop_btn);
    lv_label_set_text(stl, "STOP");
    lv_obj_center(stl);
    lv_obj_add_flag(s_stop_btn, LV_OBJ_FLAG_HIDDEN);

    s_live_lbl = lv_label_create(sub);
    lv_label_set_text(s_live_lbl, "");
    lv_obj_set_style_text_color(s_live_lbl, THEME_FG, 0);
    lv_obj_set_pos(s_live_lbl, 4, y + 56);

    refresh_param_labels();
    s_created = true;
    return sub;
}

void screen_pid_tuning_update(const oven_state_t *snap) {
    if (!s_created) return;
    bool running = (snap->mode == MODE_STEP_TEST);
    if (running) {
        lv_obj_add_flag(s_start_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_stop_btn, LV_OBJ_FLAG_HIDDEN);
        char b[48];
        snprintf(b, sizeof b, "%.1f \xC2\xB0""C   duty %u%%   %lus",
                 snap->temp_c, (unsigned)snap->step_duty,
                 (unsigned long)(snap->elapsed_ms / 1000));
        lv_label_set_text(s_live_lbl, b);
    } else {
        lv_obj_clear_flag(s_start_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_stop_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_live_lbl, "");
    }
}
```

> Theme tokens used: `THEME_STOP` (deep red, the STOP button), `THEME_ACCENT` (warm orange), `THEME_PANEL_BG`, `THEME_FG_DIM`, `THEME_FG`, `THEME_BORDER` — all defined in `src/ui/theme.h`. (There is no `THEME_ERR`; the red token is `THEME_STOP`. `THEME_GO`/`THEME_OK` are the greens if a green START is preferred over the orange accent.)

- [ ] **Step 3: Add the Settings row + extern**

In `src/ui/screen_settings.c`, add the extern with the other sub-screen externs (near the top):

```c
extern lv_obj_t *screen_pid_tuning_create(void);       /* PID Tuning step-test */
```

Add a row to `s_rows[]` (after the `"PID"` row):

```c
    { LV_SYMBOL_CHARGE,   "PID Tuning",      &screen_pid_tuning_create      },
```

> `s_subscreens[]` is sized from `s_rows` via `sizeof`, so it grows automatically. Use any existing `LV_SYMBOL_*`; `LV_SYMBOL_CHARGE` reads as "tuning/energy" — pick another if preferred.

- [ ] **Step 4: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32dev`
Expected: build SUCCEEDS.

- [ ] **Step 5: Commit**

```bash
git add src/ui/screen_pid_tuning.h src/ui/screen_pid_tuning.c src/ui/screen_settings.c
git commit -m "feat(ui): PID Tuning sub-screen — params, START/STOP, live readout"
```

### Task 3.2: Wire the sub-screen update into the UI refresh loop

**Files:**
- Modify: `src/ui/ui_task.c`

- [ ] **Step 1: Include + call the update**

In `src/ui/ui_task.c`, add the include near the other screen includes:

```c
#include "ui/screen_pid_tuning.h"
```

In `ui_refresh_timer`, after `screen_pid_update(&s_snap);`, add:

```c
    screen_pid_tuning_update(&s_snap);
```

- [ ] **Step 2: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32dev`
Expected: build SUCCEEDS.

- [ ] **Step 3: Commit**

```bash
git add src/ui/ui_task.c
git commit -m "feat(ui): refresh PID Tuning sub-screen each UI tick"
```

### Task 3.3: Pending-gain confirm prompt on the sub-screen

**Files:**
- Modify: `src/ui/screen_pid_tuning.c`

When `/suggest-gains` arrives, the snapshot carries `pending_pid_valid` + `pending_pid`. Surface the existing modal confirm (`settings_confirm`) once; on **OK** enqueue the unchanged `CMD_APPLY_PID`, on **Cancel** enqueue `CMD_DISMISS_PID`. Only act while idle (per spec §4). This requires giving `settings_confirm` an optional on-cancel callback — the current version runs a callback only on OK, which would leave a cancelled suggestion stuck pending with no way to re-surface (review finding #15).

- [ ] **Step 1: Extend `settings_confirm` with an optional on-cancel callback**

In `src/ui/screen_settings.h`, update the declaration (find the existing `settings_confirm` prototype):

```c
/* Modal confirm. on_confirm runs on OK; on_cancel (may be NULL) runs on Cancel
 * or dismiss. Both fire at most once. */
void settings_confirm(const char *title, const char *msg,
                      void (*on_confirm)(void), void (*on_cancel)(void));
```

In `src/ui/screen_settings.c`, add a second stored callback. **Crucially, fire the cancel callback from `confirm_close` itself**, not only from `confirm_cancel_cb` — because `confirm_close()` is also called directly by `settings_pop_subscreen()` (screen_settings.c:157) and `settings_pop_to_root_sub()` (screen_settings.c:165). If the operator navigates **Back** while the suggested-gains modal is open, those paths must still dismiss the pending suggestion, or it gets stuck `pending_pid_valid` on the controller with no way to clear it (review finding F1).

```c
static void (*s_confirm_cb)(void) = NULL;
static void (*s_cancel_cb)(void)  = NULL;   /* NEW: runs when the modal closes without OK */
```

Rewrite `confirm_close` so closing the modal for ANY reason other than OK fires the cancel callback exactly once:

```c
static void confirm_close(void) {
    void (*cancel)(void) = s_cancel_cb;   /* capture before clearing */
    s_confirm_cb = NULL;
    s_cancel_cb  = NULL;
    if (s_confirm_box) { lv_msgbox_close(s_confirm_box); s_confirm_box = NULL; }
    if (cancel) cancel();                 /* Back-nav / dismiss path also dismisses */
}
```

Because `confirm_close` now fires the cancel callback, the OK path must NOT let it fire too. Update `confirm_ok_cb` to clear the cancel callback before closing:

```c
static void confirm_ok_cb(lv_event_t *e) {
    (void)e;
    void (*cb)(void) = s_confirm_cb;   /* capture before confirm_close clears it */
    s_cancel_cb = NULL;                /* OK path: suppress the cancel callback */
    confirm_close();
    if (cb) cb();
}
```

`confirm_cancel_cb` stays simple — it just closes, and `confirm_close` runs the cancel cb:

```c
static void confirm_cancel_cb(lv_event_t *e) {
    (void)e;
    confirm_close();   /* fires s_cancel_cb internally */
}
```

Update `settings_confirm` to store the cancel callback (note: it calls `confirm_close()` first to clear any prior dialog — at that point `s_cancel_cb` is still NULL/previous, which is fine since assigning happens after):

```c
void settings_confirm(const char *title, const char *msg,
                      void (*on_confirm)(void), void (*on_cancel)(void)) {
    confirm_close();              /* never stack two dialogs (fires any prior cancel cb) */
    s_confirm_cb = on_confirm;
    s_cancel_cb  = on_cancel;
    /* ... rest unchanged (create msgbox, add Cancel + OK buttons) ... */
}
```

> **DRY/regression check:** `settings_confirm` already has 2 callers (Forget WiFi in `screen_network.c:19`, Regenerate token in `screen_firmware_update.c:31`). Update both to pass `NULL` as the new fourth argument. Their existing on-confirm callbacks are unaffected; passing NULL on-cancel preserves today's "Cancel just closes" behavior for them. Build fails to compile until they're updated — that's the safety net. Run `grep -rn "settings_confirm(" src/ui/` to confirm exactly these two (plus the definition).

- [ ] **Step 2: Add confirm handling in the sub-screen**

In `src/ui/screen_pid_tuning.c`, add file-statics and helpers:

```c
#include "ui/screen_settings.h"   /* settings_confirm — already included above */

static bool        s_confirm_open;
static pid_gains_t s_confirm_gains;

static void apply_confirmed(void) {
    cmd_msg_t m = { .type = CMD_APPLY_PID, .payload.pid = s_confirm_gains };
    xQueueSend(g_cmd_queue, &m, 0);
    s_confirm_open = false;
}
static void suggestion_dismissed(void) {
    cmd_msg_t m = { .type = CMD_DISMISS_PID };
    xQueueSend(g_cmd_queue, &m, 0);
    s_confirm_open = false;
}
```

In `screen_pid_tuning_update`, after the running/idle block, add:

```c
    /* Surface a browser-suggested gain set for confirmation — idle only.
     * OK applies (CMD_APPLY_PID); Cancel OR Back-nav dismisses (CMD_DISMISS_PID,
     * via the settings_confirm on_cancel wired into confirm_close, finding F1)
     * so the controller's pending flag clears and the prompt does not re-pop.
     *
     * Reconciliation: s_confirm_open mirrors "is our modal currently up". Both
     * outcomes (apply_confirmed / suggestion_dismissed) set it false AND cause
     * the controller to clear pending_pid_valid within a tick or two; the line
     * below converges s_confirm_open to false once that clear is observed, so a
     * Back-nav that fired the dismiss can't leave us wedged. */
    if (snap->pending_pid_valid && !s_confirm_open && snap->mode == MODE_IDLE) {
        s_confirm_open = true;
        s_confirm_gains = snap->pending_pid;
        char msg[96];
        snprintf(msg, sizeof msg, "Apply Kp %.2f  Ki %.2f  Kd %.2f ?",
                 s_confirm_gains.kp, s_confirm_gains.ki, s_confirm_gains.kd);
        settings_confirm("Suggested gains", msg, apply_confirmed, suggestion_dismissed);
    }
    if (!snap->pending_pid_valid) s_confirm_open = false;   /* controller cleared it */
```

> With the cancel callback wired, the separate on-screen "Dismiss" button is redundant — the modal's own Cancel now dismisses. Do NOT add a separate dismiss button; the modal covers both outcomes. (This also means Task 3.1's sub-screen needs no `s_dismiss_btn` — it was never added.)

- [ ] **Step 3: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32dev`
Expected: build SUCCEEDS (existing `settings_confirm` callers updated to the 4-arg form, else it won't compile).

- [ ] **Step 4: Commit**

```bash
git add src/ui/screen_settings.h src/ui/screen_settings.c src/ui/screen_pid_tuning.c src/ui/screen_network.c src/ui/screen_firmware_update.c
git commit -m "feat(ui): confirm/dismiss browser-suggested PID gains (settings_confirm gains on_cancel)"
```

---

## Phase 4 — Web SPA (fit math TDD-first, then the page)

> The fit math is the conceptual core and the most error-prone part, so it is built TDD-first as a pure ES module (`fit.js`) with node tests, then the page (`tuning.html`) wires chart + polling + archive around it. A build step inlines `fit.js` into `tuning.html` and gzips it for embedding (replacing the Phase 2 placeholder). Spec §5 requires the fit math to carry inline comments naming the source methods.

### Task 4.1: Fit math module — failing tests first

**Files:**
- Create: `test/web/fit.test.mjs`
- Create: `package.json` (minimal, for `node --test`)

- [ ] **Step 1: Add a minimal `package.json`**

Create `package.json` at the repo root (only if one does not already exist — check first with `ls package.json`):

```json
{
  "name": "reflow-oven-web",
  "private": true,
  "type": "module",
  "scripts": {
    "test:web": "node --test test/web/"
  }
}
```

- [ ] **Step 2: Write the failing tests**

Create `test/web/fit.test.mjs`:

```js
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { fitReactionCurve, computeGains } from '../../src/web/fit.js';

/* Synthetic first-order-plus-dead-time step response with KNOWN L and slope.
 * samples: [{t_s, temp_c}], duty constant. We construct a curve whose maximum
 * slope and dead time we control, then assert the fit recovers them. */
function synth({ amb = 25, gain = 200, tau = 70, L = 20, dt = 1, n = 240 }) {
    const s = [];
    for (let i = 0; i < n; i++) {
        const t = i * dt;
        const k = t - L;
        const temp = k <= 0 ? amb : amb + gain * (1 - Math.exp(-k / tau));
        s.push({ t_s: t, temp_c: temp });
    }
    return s;
}

test('fitReactionCurve recovers dead time within tolerance', () => {
    const samples = synth({ L: 20 });
    const fit = fitReactionCurve(samples);
    assert.ok(Math.abs(fit.L - 20) <= 3, `L=${fit.L} not within 3s of 20`);
});

test('fitReactionCurve reports a positive max slope N', () => {
    const fit = fitReactionCurve(synth({}));
    assert.ok(fit.N > 0, `N=${fit.N} should be > 0`);
});

test('fitReactionCurve flags failure on a flat (no-rise) trace', () => {
    const flat = Array.from({ length: 60 }, (_, i) => ({ t_s: i, temp_c: 25 }));
    const fit = fitReactionCurve(flat);
    assert.equal(fit.ok, false);
});

test('computeZN gains match hand-computed textbook numbers', () => {
    /* Hard-coded expected values (NOT a re-derivation of the implementation,
     * so a wrong constant in the code is actually caught — review finding #16).
     * N=0.8 °C/s, ΔCO=50 %, L=20 s  =>  R = N/ΔCO = 0.016 °C/(s·%).
     * Classic ZN reaction-curve PID:
     *   Kp = 1.2/(R·L) = 1.2/(0.016·20) = 3.75
     *   Ti = 2L = 40 s   => Ki = Kp/Ti = 3.75/40 = 0.09375
     *   Td = 0.5L = 10 s => Kd = Kp·Td = 3.75·10 = 37.5  (clamped to KD_MAX=50, unaffected) */
    const g = computeGains({ N: 0.8, dCO: 50, L: 20, rule: 'zn' });
    assert.ok(Math.abs(g.kp - 3.75) < 1e-3, `kp=${g.kp}`);
    assert.ok(Math.abs(g.ki - 0.09375) < 1e-4, `ki=${g.ki}`);
    assert.ok(Math.abs(g.kd - 37.5) < 1e-3, `kd=${g.kd}`);
});

test('no-overshoot rule yields a smaller Kp than classic ZN', () => {
    const args = { N: 0.8, dCO: 50, L: 20 };
    const zn = computeGains({ ...args, rule: 'zn' });
    const no = computeGains({ ...args, rule: 'no-overshoot' });
    assert.ok(no.kp < zn.kp, 'no-overshoot Kp should be gentler than ZN');
});

test('conservative rule yields a smaller Kp than classic ZN', () => {
    const args = { N: 0.8, dCO: 50, L: 20 };
    assert.ok(computeGains({ ...args, rule: 'conservative' }).kp
              < computeGains({ ...args, rule: 'zn' }).kp);
});

test('computeGains is monotonic: a manual L/N override recomputes cleanly', () => {
    /* Drag-to-refine feeds adjusted L and N straight back into computeGains.
     * A larger dead time L must reduce Kp (Kp ∝ 1/L). */
    const a = computeGains({ N: 0.8, dCO: 50, L: 20, rule: 'zn' });
    const b = computeGains({ N: 0.8, dCO: 50, L: 40, rule: 'zn' });
    assert.ok(b.kp < a.kp, 'larger L should give smaller Kp');
});

test('gains are clamped to firmware bounds (KP_MAX=100, KI_MAX=50, KD_MAX=50)', () => {
    /* Choose inputs whose RAW gains blow past every bound so the clamp must
     * actually engage (not a tautology). N tiny, ΔCO large, L tiny:
     *   R = N/ΔCO = 0.01/100 = 1e-4;  R·L = 1e-4·0.1 = 1e-5
     *   raw Kp = 1.2/1e-5 = 120000  -> clamp to 100
     *   raw Ki = Kp/(2L) = 120000/0.2 = 600000 -> clamp to 50
     *   raw Kd = Kp·0.5L = 120000·0.05 = 6000 -> clamp to 50
     * Assert EQUALITY to the caps, proving the clamp fired. */
    const g = computeGains({ N: 0.01, dCO: 100, L: 0.1, rule: 'zn' });
    assert.equal(g.kp, 100);
    assert.equal(g.ki, 50);
    assert.equal(g.kd, 50);
});
```

- [ ] **Step 3: Run to verify failure**

Run: `node --test test/web/`
Expected: FAIL — `Cannot find module '.../src/web/fit.js'`.

### Task 4.2: Implement the fit module

**Files:**
- Create: `src/web/fit.js`

- [ ] **Step 1: Implement with the required documentation**

Create `src/web/fit.js`:

```js
/* fit.js — Ziegler–Nichols open-loop "reaction-curve" PID identification.
 *
 * METHOD (Ziegler, J.G. & Nichols, N.B., 1942, "Optimum Settings for
 * Automatic Controllers"): drive the process open-loop with a fixed step in
 * controller output ΔCO (here, % SSR duty) and record the temperature rise.
 * The process-reaction curve has:
 *   - L = dead time: delay before the output starts to move (s)
 *   - N = maximum slope of the rise at the inflection point (°C/s)
 * The "reaction rate" is R = N / ΔCO  (°C per second, per % duty).
 *
 * UNITS — why the gains drop straight into the firmware PID:
 *   ΔCO is in % duty (firmware PID output is 0–100 % duty),
 *   error is in °C (firmware PID error = setpoint − temp, °C),
 *   so Kp = 1.2/(R·L) has units (% duty)/(°C·s · 1/s) = (% duty)/°C — exactly
 *   the firmware's output/error units. No conversion needed.
 */

/* Firmware gain bounds — MUST match include/config.h (KP_MAX/KI_MAX/KD_MAX).
 * Kept in sync manually; the JS unit test asserts the clamp values. */
export const BOUNDS = { KP_MAX: 100, KI_MAX: 50, KD_MAX: 50 };

const clamp = (x, hi) => (x < 0 ? 0 : x > hi ? hi : x);

/* Find dead time L and max slope N from a reaction curve.
 * samples: [{t_s, temp_c}] at ~1 Hz. Returns {ok, L, N, tInflect, tempInflect}.
 *
 * Approach: smooth with a short moving average to suppress thermocouple
 * noise, find the point of maximum slope (the inflection of a first-order
 * rise), then extend the tangent at that point back to the ambient baseline;
 * the time-axis intercept is L. This is the standard tangent construction of
 * the reaction-curve method. */
export function fitReactionCurve(samples) {
    if (!samples || samples.length < 5) return { ok: false };
    const t = samples.map(s => s.t_s);
    const y = movingAverage(samples.map(s => s.temp_c), 3);

    const baseline = y[0];
    // Max-slope (inflection) search via central differences.
    let N = 0, idx = -1;
    for (let i = 1; i < y.length - 1; i++) {
        const slope = (y[i + 1] - y[i - 1]) / (t[i + 1] - t[i - 1]);
        if (slope > N) { N = slope; idx = i; }
    }
    // Require a meaningful rise; a flat/noisy trace has no usable inflection.
    const totalRise = y[y.length - 1] - baseline;
    if (idx < 0 || N <= 0 || totalRise < 5) return { ok: false };

    // Tangent at the inflection: temp = y[idx] + N·(t − t[idx]).
    // Solve for the time where the tangent crosses the ambient baseline.
    const L = t[idx] - (y[idx] - baseline) / N;
    return { ok: true, L: Math.max(0, L), N, tInflect: t[idx], tempInflect: y[idx] };
}

function movingAverage(arr, half) {
    const out = new Array(arr.length);
    for (let i = 0; i < arr.length; i++) {
        let sum = 0, cnt = 0;
        for (let j = Math.max(0, i - half); j <= Math.min(arr.length - 1, i + half); j++) {
            sum += arr[j]; cnt++;
        }
        out[i] = sum / cnt;
    }
    return out;
}

/* Compute PID gains from N, ΔCO (% duty step), L (s) under a tuning rule.
 * Returns {kp, ki, kd} clamped to firmware BOUNDS. All gains in firmware
 * units (Kp in %/°C; Ki = Kp/Ti per second; Kd = Kp·Td seconds).
 *
 * Rules:
 *  - 'zn'  Ziegler–Nichols classic (1942) reaction-curve PID:
 *            Kp = 1.2/(R·L),  Ti = 2L,  Td = 0.5L.
 *  - 'conservative'  A gentler variant for processes that overshoot under
 *            classic ZN. We scale the ZN proportional gain down and lengthen
 *            the integral time (the same intent as Tyreus–Luyben detuning,
 *            but note TL is properly defined for the CLOSED-LOOP ultimate-gain
 *            method, Kp=Ku/3.2, Ti=2.2·Pu — NOT this open-loop reaction curve,
 *            so we deliberately do NOT call this "Tyreus–Luyben"):
 *            Kp = (1/1.7)·(1.2/(R·L)),  Ti = 2.2·(2L),  Td = (2L)/6.3.
 *  - 'no-overshoot'  Pessen / ZN "no overshoot" open-loop variant. The
 *            tabulated no-overshoot proportional constant is ~0.2/(R·L)
 *            (markedly gentler than classic ZN's 1.2), with Ti=2L, Td=L/3.
 *            See e.g. Pessen (1994) and the ZN-variant tables in standard PID
 *            references. Chosen as the default because this oven overshoots.
 */
export function computeGains({ N, dCO, L, rule }) {
    const R = N / dCO;                  // °C/s per % duty
    const znKp = 1.2 / (R * L);         // classic ZN proportional gain
    let kp, Ti, Td;
    switch (rule) {
        case 'conservative':
            kp = znKp / 1.7; Ti = 2.2 * (2 * L); Td = (2 * L) / 6.3; break;
        case 'no-overshoot':
            kp = 0.2 / (R * L); Ti = 2 * L; Td = L / 3; break;
        case 'zn':
        default:
            kp = znKp; Ti = 2 * L; Td = 0.5 * L; break;
    }
    const ki = Ti > 0 ? kp / Ti : 0;    // 1/s
    const kd = kp * Td;                 // s
    return {
        kp: clamp(kp, BOUNDS.KP_MAX),
        ki: clamp(ki, BOUNDS.KI_MAX),
        kd: clamp(kd, BOUNDS.KD_MAX),
    };
}
```

- [ ] **Step 2: Run the tests to verify pass**

Run: `node --test test/web/`
Expected: PASS (all 6 tests).

- [ ] **Step 3: Commit**

```bash
git add package.json test/web/fit.test.mjs src/web/fit.js
git commit -m "feat(web): ZN reaction-curve fit module with node unit tests"
```

### Task 4.3: Build the SPA page (chart + polling + fit + suggest)

**Files:**
- Modify: `src/web/tuning.html`

This replaces the Phase 2 placeholder with the real dashboard (layout C). It imports nothing at runtime — `fit.js` is inlined by the build step in Task 4.5 — so during development the page references `fit.js` via a relative `<script type=module>` and the inliner swaps it for inline code at embed time.

- [ ] **Step 1: Write the page**

Replace `src/web/tuning.html` with the dashboard. (Full file — chart, right rail, archive, polling, fit wiring.)

```html
<!doctype html>
<html lang=en>
<meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Reflow oven — PID tuning</title>
<style>
  :root{--bg:#0f1419;--panel:#141a21;--border:#232833;--fg:#cdd3dc;--accent:#1f6feb;
        --temp:#f0883e;--duty:#58a6ff;--err:#c93c3c;--ok:#3fb950;--mut:#7d8590}
  *{box-sizing:border-box} body{margin:0;background:var(--bg);color:var(--fg);
     font-family:system-ui,sans-serif;font-size:14px}
  header{display:flex;justify-content:space-between;padding:8px 12px;border-bottom:1px solid var(--border)}
  .wrap{display:flex;gap:14px;padding:12px}
  .left{flex:2;min-width:0}.rail{flex:1;min-width:200px;display:flex;flex-direction:column;gap:10px}
  canvas{width:100%;height:auto;background:#0b0f14;border:1px solid var(--border);border-radius:6px}
  .card{background:var(--panel);border:1px solid var(--border);border-radius:6px;padding:10px}
  .row{display:flex;justify-content:space-between}
  button{border:0;border-radius:6px;padding:10px;font-weight:600;cursor:pointer;color:#fff}
  .stop{background:var(--err)} .suggest{background:var(--accent)}
  .legend{display:flex;gap:14px;color:var(--mut);font-size:12px;margin-top:6px;flex-wrap:wrap}
  select{width:100%;background:#0b0f14;color:var(--fg);border:1px solid var(--border);border-radius:5px;padding:6px}
  #archive .item{display:flex;gap:8px;align-items:center;padding:6px;border-bottom:1px solid var(--border);font-size:12px}
  .banner{background:var(--err);color:#fff;padding:6px 12px;text-align:center;display:none}
  .mut{color:var(--mut)}
</style>

<header>
  <div><b>Reflow oven</b> <span class=mut id=host>reflow.local</span> <span class=mut id=apiv></span></div>
  <div id=conn class=mut>connecting…</div>
</header>
<div class=banner id=banner></div>

<div class=wrap>
  <div class=left>
    <canvas id=chart width=640 height=320></canvas>
    <div class=legend>
      <span style="color:var(--temp)">■ temperature (°C, left)</span>
      <span style="color:var(--duty)">■ duty (%, right)</span>
      <span style="color:var(--mut)">┈ ZN tangent</span>
      <span style="color:var(--err)">┈ ceiling</span>
    </div>
    <div class=card style="margin-top:12px">
      <div class=row><b id=arcHdr>▾ Archive — 0 runs</b>
        <span><button id=cmp class=mut style="background:#22272e">Compare</button>
        <button id=exp style="background:#22272e">Export</button>
        <button id=imp style="background:#22272e">Import</button></span></div>
      <div id=archive></div>
    </div>
  </div>
  <div class=rail>
    <div class=card>
      <div class=mut>STATUS</div>
      <div class=row><span>mode</span><b id=st_mode>—</b></div>
      <div class=row><span>temp</span><b id=st_temp>—</b></div>
      <div class=row><span>duty</span><b id=st_duty>—</b></div>
      <div class=row><span>elapsed</span><b id=st_elapsed>—</b></div>
      <div class=row><span>ended</span><b id=st_end>—</b></div>
    </div>
    <button class=stop id=stop>■ STOP</button>
    <div class=card>
      <div class=mut>REACTION-CURVE FIT</div>
      <div class=row><span>dead time L</span><b id=f_L>—</b></div>
      <div class=row><span>slope N</span><b id=f_N>—</b></div>
      <div class=row><span>ΔCO</span><b id=f_dco>—</b></div>
    </div>
    <div class=card>
      <div class=mut>TUNING RULE</div>
      <select id=rule>
        <option value=zn>Ziegler–Nichols (classic)</option>
        <option value=conservative>Conservative (less overshoot)</option>
        <option value=no-overshoot selected>No-overshoot</option>
      </select>
      <div class=row style="margin-top:8px"><span>Kp</span><b id=g_kp>—</b></div>
      <div class=row><span>Ki</span><b id=g_ki>—</b></div>
      <div class=row><span>Kd</span><b id=g_kd>—</b></div>
    </div>
    <button class=suggest id=suggest>Suggest to oven →</button>
    <div class=mut style="text-align:center">confirm on the oven touchscreen</div>
    <input type=hidden id=token>
  </div>
</div>

<script type=module src="fit.js"></script>
<script type=module src="app.js"></script>
```

> The page references `fit.js` and `app.js` as modules during dev; the build step (Task 4.5) inlines both into a single self-contained file before gzip+embed. Keeping `app.js` separate during dev keeps the page testable in a browser pointed at a live device.

- [ ] **Step 2: Commit (page shell)**

```bash
git add src/web/tuning.html
git commit -m "feat(web): tuning dashboard page shell (layout C)"
```

### Task 4.4: App logic — polling, run lifecycle, chart, archive

**Files:**
- Create: `src/web/app.js`

- [ ] **Step 1: Write the app module**

Create `src/web/app.js`:

```js
/* app.js — wires the tuning dashboard to the device API. Imports the pure
 * fit math from fit.js (inlined at embed time). Run-lifecycle detection uses
 * run_id + steptest.active + end_reason per spec §3.1. */
import { fitReactionCurve, computeGains } from './fit.js';

const API_EXPECTED = 1;
const POLL_MS = 1000;
const $ = id => document.getElementById(id);
const LS_KEY = 'reflow_tuning_runs';

let curRunId = null, samples = [], wasActive = false, lastFit = null, curDuty = 0;
let joinedLate = false;   // true if the first sample of this run was already active (spec §3.1)
let curCeiling = 0;       // step-test ceiling for the current run (drawn as the ceiling line)

async function poll() {
    try {
        const r = await fetch('/telemetry', { cache: 'no-store' });
        const s = await r.json();
        $('conn').textContent = '● live'; $('conn').style.color = 'var(--ok)';
        $('apiv').textContent = 'api v' + s.api;
        if (s.api !== API_EXPECTED) {
            showBanner('firmware/UI version mismatch — update needed');
            $('suggest').disabled = true;   // don't push gains to a mismatched API
        } else {
            $('suggest').disabled = false;
        }
        onSample(s);
    } catch (e) {
        $('conn').textContent = '○ connection lost'; $('conn').style.color = 'var(--err)';
    } finally {
        setTimeout(poll, POLL_MS);
    }
}

function onSample(s) {
    // Status panel
    $('st_mode').textContent = s.mode;
    $('st_temp').textContent = s.temp_c.toFixed(1) + ' °C';
    $('st_duty').textContent = s.duty + ' %';
    $('st_elapsed').textContent = (s.elapsed_ms / 1000 | 0) + ' s';
    $('st_end').textContent = s.steptest.end_reason;

    const active = s.steptest.active;

    // Run lifecycle (spec §3.1). ORDER MATTERS (review finding F18):
    // 1) finalize the PREVIOUS run if it just ended — BEFORE any buffer reset,
    //    so a back-to-back run that starts within one poll can't wipe the
    //    finishing run's samples before we fit/save them;
    // 2) then handle a run_id change (new run → reset buffer);
    // 3) then capture this sample.
    const runIdChanged = (s.run_id !== curRunId);
    const endedByEdge  = wasActive && !active;            // active → inactive
    const endedByNewId = wasActive && runIdChanged;       // new run started while prev looked active
    if ((endedByEdge || endedByNewId) && samples.length > 3) {
        finalizeRun(s);   // fits + saves `samples` from the run that just ended
    }

    const firstPollEver = (curRunId === null);
    if (runIdChanged) {
        // "joined_late" = we connected to a run already in progress and missed
        // its early rise, so the tangent-based dead-time L is unreliable
        // (sample[0] is not ambient). The only case where we miss more than one
        // poll interval is when the FIRST poll of the whole page session is
        // already active — the device had been running before the page opened.
        // (A witnessed start is caught within ~1 poll of run_id appearing, so L
        //  stays trustworthy.)
        joinedLate = firstPollEver && active;
        curRunId = s.run_id; samples = []; lastFit = null; curDuty = 0; curCeiling = 0;
    }
    if (active) {
        // Latch the step duty from an ACTIVE sample only. The firmware keeps
        // steptest.duty nonzero while MODE_STEP_TEST; latching only while active
        // guarantees curDuty (ΔCO) is the real step size, so gains can never be
        // computed with ΔCO = 0 (R = N/0). (review finding F18/#18)
        if (s.steptest.duty > 0) curDuty = s.steptest.duty;
        samples.push({ t_s: s.elapsed_ms / 1000, temp_c: s.temp_c, duty: s.steptest.duty });
    }
    wasActive = active;
    drawChart();
}

function finalizeRun(s) {
    lastFit = fitReactionCurve(samples);
    if (lastFit.ok) {
        // A late-joined run missed the early rise; the dead-time L is unreliable.
        // Annotate it rather than presenting a confidently-wrong number (spec §3.1).
        $('f_L').textContent = joinedLate ? (lastFit.L.toFixed(0) + ' s ⚠ joined late')
                                          : (lastFit.L.toFixed(0) + ' s');
        $('f_N').textContent = lastFit.N.toFixed(2) + ' °C/s';
        $('f_dco').textContent = curDuty + ' %';
        if (joinedLate) showBanner('joined mid-run — dead-time L may be wrong; re-run for a clean fit');
        recompute();
    } else {
        $('f_L').textContent = $('f_N').textContent = '—';
        showBanner('couldn’t auto-fit — too little rise');
    }
    saveRun(s);
}

function recompute() {
    if (!lastFit || !lastFit.ok) return;
    const g = computeGains({ N: lastFit.N, dCO: curDuty, L: lastFit.L, rule: $('rule').value });
    $('g_kp').textContent = g.kp.toFixed(2);
    $('g_ki').textContent = g.ki.toFixed(2);
    $('g_kd').textContent = g.kd.toFixed(2);
    return g;
}

async function suggest() {
    const g = recompute(); if (!g) return;
    const token = prompt('OTA token:'); if (!token) return;
    const r = await fetch('/suggest-gains', {
        method: 'POST', headers: { 'X-OTA-Token': token, 'Content-Type': 'application/json' },
        body: JSON.stringify(g),
    });
    showBanner(r.ok ? 'sent — confirm on the touchscreen' : 'rejected (' + r.status + ')');
}

async function stop() {
    const token = prompt('OTA token:'); if (!token) return;
    await fetch('/stop', { method: 'POST', headers: { 'X-OTA-Token': token } });
}

// --- archive (localStorage) ---
function loadRuns() { try { return JSON.parse(localStorage.getItem(LS_KEY)) || []; } catch { return []; } }
function saveRun(s) {
    const runs = loadRuns();
    runs.unshift({ ts: new Date().toISOString(), duty: curDuty, ceiling: s.steptest.ceiling_c,
        rule: $('rule').value, fit: lastFit, gains: recompute(), joinedLate, samples });
    try { localStorage.setItem(LS_KEY, JSON.stringify(runs.slice(0, 50))); }
    catch { showBanner('archive full — export to free space'); }
    renderArchive();
}
function renderArchive() {
    const runs = loadRuns();
    $('arcHdr').textContent = `▾ Archive — ${runs.length} runs`;
    $('archive').innerHTML = runs.map((r, i) =>
        `<div class=item><input type=checkbox data-i=${i}>
         <b>${r.ts.replace('T', ' ').slice(0, 16)}</b>
         <span class=mut>${r.duty}% → ${r.ceiling}° · ${r.rule}</span>
         <span class=mut style="margin-left:auto">Kp ${r.gains?.kp.toFixed(1)} /
           Ki ${r.gains?.ki.toFixed(2)} / Kd ${r.gains?.kd.toFixed(0)}</span>
         <button data-del=${i} style="background:#22272e">🗑</button></div>`).join('');
    $('archive').querySelectorAll('[data-del]').forEach(b =>
        b.onclick = () => { if (confirm('Delete this run?')) { const r = loadRuns();
            r.splice(+b.dataset.del, 1); localStorage.setItem(LS_KEY, JSON.stringify(r)); renderArchive(); } });
}
function exportRuns() {
    // Stamp the API version so import can validate shape + api (spec §8).
    const doc = { api: API_EXPECTED, runs: loadRuns() };
    const blob = new Blob([JSON.stringify(doc, null, 2)], { type: 'application/json' });
    const a = document.createElement('a'); a.href = URL.createObjectURL(blob);
    a.download = 'reflow-tuning-archive.json'; a.click();
}
function importRuns() {
    const inp = document.createElement('input'); inp.type = 'file'; inp.accept = '.json';
    inp.onchange = async () => {
        let doc;
        try { doc = JSON.parse(await inp.files[0].text()); }
        catch { return showBanner('import failed — not valid JSON'); }
        // Accept the {api, runs} envelope; validate api (spec §8). A bare array
        // (no envelope) is rejected as an unrecognised structure.
        if (!doc || !Array.isArray(doc.runs)) return showBanner('import failed — expected {api,runs}');
        if (doc.api !== API_EXPECTED) return showBanner(`import failed — api v${doc.api} ≠ v${API_EXPECTED}`);
        // Validate per-run shape: a run must have a timestamp and a samples array.
        const valid = doc.runs.filter(r =>
            r && typeof r.ts === 'string' && Array.isArray(r.samples));
        if (valid.length === 0) return showBanner('import failed — no valid runs found');
        const have = loadRuns(); const seen = new Set(have.map(r => r.ts));
        const merged = have.concat(valid.filter(r => !seen.has(r.ts)));   // merge, de-dup by ts
        localStorage.setItem(LS_KEY, JSON.stringify(merged.slice(0, 50)));
        renderArchive();
        showBanner(`imported ${merged.length - have.length} new run(s)`);
    };
    inp.click();
}
function compareSelected() {
    const idxs = [...$('archive').querySelectorAll('input:checked')].map(c => +c.dataset.i);
    drawChart(loadRuns().filter((_, i) => idxs.includes(i)));
}

function showBanner(msg) { const b = $('banner'); b.textContent = msg; b.style.display = 'block';
    setTimeout(() => b.style.display = 'none', 4000); }

// --- chart (hand-rolled canvas, dual axis) ---
function drawChart(overlays) {
    const c = $('chart'), x = c.getContext('2d'), W = c.width, H = c.height;
    const pL = 44, pR = 44, pT = 10, pB = 24, pw = W - pL - pR, ph = H - pT - pB;
    const Tmax = 250, tMax = Math.max(60, (samples.at(-1)?.t_s) || 60);
    x.clearRect(0, 0, W, H);
    const sx = t => pL + t / tMax * pw, syT = T => pT + (1 - T / Tmax) * ph, syD = d => pT + (1 - d / 100) * ph;
    x.strokeStyle = '#1b2129'; x.fillStyle = '#5b6571'; x.font = '10px system-ui';
    for (let g = 0; g <= Tmax; g += 50) { const y = syT(g); x.beginPath(); x.moveTo(pL, y); x.lineTo(W - pR, y); x.stroke(); x.fillStyle = '#f0883e'; x.fillText(g + '°', 6, y + 3); }
    for (let d = 0; d <= 100; d += 25) { x.fillStyle = '#58a6ff'; x.fillText(d + '%', W - pR + 5, syD(d) + 3); }
    // Ceiling line: the temperature at which a step test auto-aborts (spec §5 legend).
    if (curCeiling > 0) {
        x.strokeStyle = '#c93c3c'; x.setLineDash([5, 4]); x.lineWidth = 1; x.beginPath();
        x.moveTo(pL, syT(curCeiling)); x.lineTo(W - pR, syT(curCeiling)); x.stroke(); x.setLineDash([]);
    }
    const line = (pts, col, sy) => { x.strokeStyle = col; x.lineWidth = 2; x.beginPath();
        pts.forEach((p, i) => { const X = sx(p.t_s), Y = sy(p.temp_c ?? p.duty); i ? x.lineTo(X, Y) : x.moveTo(X, Y); }); x.stroke(); };
    line(samples, '#f0883e', syT);
    line(samples.map(s => ({ t_s: s.t_s, duty: s.duty })), '#58a6ff', syD);
    if (overlays) overlays.forEach((r, k) => { x.globalAlpha = .6; line(r.samples, ['#888', '#3fb950', '#d29922'][k % 3], syT); x.globalAlpha = 1; });
    if (lastFit?.ok) {   // ZN tangent
        x.strokeStyle = '#8b949e'; x.setLineDash([6, 4]); x.beginPath();
        const t2 = lastFit.tInflect + (Tmax - lastFit.tempInflect) / lastFit.N;
        x.moveTo(sx(lastFit.L), syT(samples[0].temp_c)); x.lineTo(sx(t2), syT(Tmax)); x.stroke(); x.setLineDash([]);
    }
}

$('stop').onclick = stop;
$('suggest').onclick = suggest;
$('rule').onchange = recompute;
$('exp').onclick = exportRuns; $('imp').onclick = importRuns; $('cmp').onclick = compareSelected;
renderArchive();
poll();
```

- [ ] **Step 2: Manual dev check (optional, against a live device)**

If a device is reachable on the LAN, serve the `src/web/` dir locally and point a browser at the device for telemetry — or just open `tuning.html` from a local server. Full verification happens on-hardware in Phase 5.

Run (optional): `python3 -m http.server -d src/web 8000` then browse `http://localhost:8000/tuning.html` (telemetry calls will fail without a device — that exercises the connection-lost path).

- [ ] **Step 3: Commit**

```bash
git add src/web/app.js
git commit -m "feat(web): polling, run-lifecycle, chart, archive, suggest/stop"
```

### Task 4.5: Inline + gzip the real SPA into the embedded asset

**Files:**
- Modify: `tools/build_web_asset.py`

- [ ] **Step 1: Extend the build step to inline modules then gzip**

Replace `tools/build_web_asset.py` with:

```python
#!/usr/bin/env python3
"""Inline fit.js + app.js into tuning.html, then gzip -> tuning.html.gz for
CMake EMBED_FILES. Deterministic (mtime=0). The dev page references the modules
via <script src>; here we replace those tags with inline <script> blocks so the
embedded page is fully self-contained (the device serves one file)."""
import gzip, pathlib, re

web = pathlib.Path(__file__).resolve().parent.parent / "src" / "web"
html = (web / "tuning.html").read_text()
fit = (web / "fit.js").read_text()
app = (web / "app.js").read_text()

# app.js imports from './fit.js'; strip that import once fit is inlined ahead of it.
app = re.sub(r"^\s*import\s+\{[^}]*\}\s+from\s+'\./fit\.js';\s*$", "", app, flags=re.M)
# fit.js uses `export` — strip the export keywords for the inlined (non-module) scope.
fit_inl = re.sub(r"\bexport\s+", "", fit)

html = html.replace('<script type=module src="fit.js"></script>',
                    f"<script>{fit_inl}</script>")
html = html.replace('<script type=module src="app.js"></script>',
                    f"<script>{app}</script>")

(web / "tuning.html.built").write_text(html)   # debug artifact (gitignored)
dst = web / "tuning.html.gz"
with gzip.GzipFile(filename="", mode="wb", fileobj=open(dst, "wb"), mtime=0) as f:
    f.write(html.encode())
print(f"inlined+gzipped tuning.html: {len(html)} B -> {dst.stat().st_size} B")
```

> The inliner strips `export`/`import` so the two modules share one classic-script scope. Keep the function/const names unique across `fit.js` and `app.js` (they are) so concatenation is safe.

- [ ] **Step 1b: Update the CMake DEPENDS to include the now-existing JS sources**

In `src/CMakeLists.txt`, the `add_custom_command` `DEPENDS` from Task 2.2 listed only `tuning.html` + the script (because `fit.js`/`app.js` didn't exist yet). Now that they exist, extend it so edits to either trigger a rebuild of the embedded asset:

```cmake
    DEPENDS ${WEB_DIR}/tuning.html ${WEB_DIR}/fit.js ${WEB_DIR}/app.js
            ${CMAKE_SOURCE_DIR}/tools/build_web_asset.py
```

- [ ] **Step 2: Add the debug artifact to .gitignore**

Append to `.gitignore`:

```
src/web/*.html.built
```

- [ ] **Step 3: Regenerate the embedded asset and check size against the budget**

Run: `python3 tools/build_web_asset.py`
Expected: prints sizes. Confirm the `.gz` is well under the Phase-0 measured headroom (target ~12–15 KB, hard ceiling: must leave ≥30 KB slot margin). If it exceeds budget, trim the page before continuing.

- [ ] **Step 4: Rebuild firmware with the real embedded page**

Run: `~/.platformio/penv/bin/pio run -e esp32dev`
Expected: build SUCCEEDS; firmware now serves the real SPA at `/`.

- [ ] **Step 5: Commit**

```bash
git add tools/build_web_asset.py .gitignore
git commit -m "build: inline fit.js/app.js into the embedded gzipped SPA"
```

### Task 4.6: Verification overlay (achieved vs target reflow)

**Files:**
- Modify: `src/web/app.js`

Spec §5 / success criterion 5: after applying gains, the operator runs a *normal reflow* (supervised); the page overlays achieved temperature vs the published setpoint and reports peak overshoot and time-above-liquidus. The reflow run already publishes `setpoint_c` and bumps `run_id` (Phase 1), and `mode == "running"`, so the same poll loop captures it. This task extends the existing `onSample` lifecycle (from Task 4.4) to treat a reflow run as a captured run too — reusing the same finalize-before-reset ordering (finding F18) so a reflow and a step test never splice.

- [ ] **Step 1: Add `running` tracking and a reflow-aware run-end branch**

In `src/web/app.js`, make these *surgical* edits to the Task 4.4 `onSample` (do NOT re-order the lifecycle — the finalize-before-reset structure stays):

(a) Add `let wasRunning = false;` to the module-level `let` declarations (next to `wasActive`).

(b) Just after `const active = s.steptest.active;`, add:

```js
    const running = (s.mode === 'running');   // a normal reflow run (verification overlay)
```

(c) In the run-end detection, broaden the edges to cover reflow and dispatch to the right finalizer. Replace:

```js
    const runIdChanged = (s.run_id !== curRunId);
    const endedByEdge  = wasActive && !active;            // active → inactive
    const endedByNewId = wasActive && runIdChanged;       // new run started while prev looked active
    if ((endedByEdge || endedByNewId) && samples.length > 3) {
        finalizeRun(s);   // fits + saves `samples` from the run that just ended
    }
```

with:

```js
    const runIdChanged = (s.run_id !== curRunId);
    const stepEnded = (wasActive  && !active)  || (wasActive  && runIdChanged);
    const reflEnded = (wasRunning && !running) || (wasRunning && runIdChanged);
    if (stepEnded && samples.length > 3)      finalizeRun(s);   // ZN fit + save (step test)
    else if (reflEnded && samples.length > 3) verifyRun();      // overshoot/liquidus (reflow)
```

(d) Broaden the capture push to include reflow samples (with setpoint). Replace:

```js
    if (active) {
        // ... latch curDuty ...
        if (s.steptest.duty > 0) curDuty = s.steptest.duty;
        samples.push({ t_s: s.elapsed_ms / 1000, temp_c: s.temp_c, duty: s.steptest.duty });
    }
```

with:

```js
    if (active) {
        if (s.steptest.duty > 0) curDuty = s.steptest.duty;   // ΔCO from active sample only
        if (s.steptest.ceiling_c > 0) curCeiling = s.steptest.ceiling_c;  // for the ceiling line
        samples.push({ t_s: s.elapsed_ms / 1000, temp_c: s.temp_c, duty: s.steptest.duty });
    } else if (running) {
        // Reflow run: capture setpoint too so verifyRun can overlay achieved-vs-target.
        samples.push({ t_s: s.elapsed_ms / 1000, temp_c: s.temp_c,
                       duty: s.duty, setpoint_c: s.setpoint_c });
    }
```

(e) Update the trailing state line. Replace `wasActive = active;` with:

```js
    wasActive = active; wasRunning = running;
```

Add the verify function:

```js
/* Verification metrics for a reflow run: peak overshoot above the planned
 * setpoint, and time-above-liquidus (default 217 °C for SAC305; TUNE/UI later).
 * Overlays achieved vs setpoint on the chart. */
const LIQUIDUS_C = 217;
function verifyRun() {
    let maxOver = 0, aboveS = 0, dt = 0;
    for (let i = 0; i < samples.length; i++) {
        const o = samples[i].temp_c - (samples[i].setpoint_c || 0);
        if (samples[i].setpoint_c && o > maxOver) maxOver = o;
        if (i > 0) dt = samples[i].t_s - samples[i - 1].t_s;
        if (samples[i].temp_c >= LIQUIDUS_C) aboveS += dt;
    }
    showBanner(`verification: peak overshoot ${maxOver.toFixed(1)} °C, ` +
               `time above ${LIQUIDUS_C}°C = ${aboveS.toFixed(0)} s`);
    drawChart();   // setpoint overlay drawn below
}
```

In `drawChart`, after the temperature line, draw the **instantaneous setpoint trail** when present (the as-passed target, distinct from the planned-ahead curve added in Step 2):

```js
    if (samples.some(s => s.setpoint_c)) {
        line(samples.map(s => ({ t_s: s.t_s, temp_c: s.setpoint_c })), '#3fb950', syT);
    }
```

- [ ] **Step 2: Fetch the planned curve on reflow start + draw it ahead**

So the full HEAT→SOAK→REFLOW→COOL target is visible from t=0 and the achieved trace visibly chases it (the intuitive "is the tuning good?" view). The curve comes from `GET /profile` (Task 2.9).

(a) Add a module-level `let plannedCurve = null;` near the other `let`s.

(b) In `onSample`, inside the `if (runIdChanged) { … }` block (after the buffer reset, where `joinedLate`/`curCeiling` are set), fetch the curve once for a reflow run:

```js
        // On a fresh REFLOW run, fetch the planned curve once for the
        // draw-ahead target line. Step tests have no planned curve.
        plannedCurve = null;
        if (running) fetchProfile();
```

(c) Add the fetch helper at module scope:

```js
/* One-shot fetch of the active planned curve for the verification overlay.
 * Stored as { step_ms, temp_c:[…] }; temp_c[i] is the setpoint at i*step_ms. */
async function fetchProfile() {
    try {
        const p = await (await fetch('/profile', { cache: 'no-store' })).json();
        if (p && Array.isArray(p.temp_c)) plannedCurve = p;
        drawChart();
    } catch { /* leave plannedCurve null; the incremental setpoint trail still shows */ }
}
```

(d) In `drawChart`, extend `tMax` to cover the planned curve so the whole target is visible. Replace the `tMax` line:

```js
    const Tmax = 250, tMax = Math.max(60, (samples.at(-1)?.t_s) || 60);
```

with:

```js
    const plannedEnd = plannedCurve ? plannedCurve.temp_c.length * ((plannedCurve.step_ms || 1000) / 1000) : 0;
    const Tmax = 250, tMax = Math.max(60, (samples.at(-1)?.t_s) || 60, plannedEnd);
```

(e) In `drawChart`, after the ceiling-line block (and before the live trace, so the achieved line sits on top), draw the planned curve as a dashed grey reference:

```js
    if (plannedCurve && plannedCurve.temp_c.length) {
        const stepS = (plannedCurve.step_ms || 1000) / 1000;
        x.strokeStyle = '#6e7681'; x.setLineDash([4, 3]); x.lineWidth = 1.5; x.beginPath();
        plannedCurve.temp_c.forEach((T, i) => {
            const X = sx(i * stepS), Y = syT(T); i ? x.lineTo(X, Y) : x.moveTo(X, Y);
        });
        x.stroke(); x.setLineDash([]);
    }
```

(f) Add a legend entry for the planned line in `tuning.html` — add to the `.legend` div: `<span style="color:#6e7681">┈ planned target</span>`.

> Planned and achieved share the X-axis: both index time from run start (`elapsed_ms`), and planned sample `i` is at `i*step_ms`, so they align directly.

- [ ] **Step 3: Manual check (optional, against a device)**

On hardware (Phase 5) this is exercised by the verification reflow. Host-side there is nothing to run.

- [ ] **Step 4: Commit**

```bash
git add src/web/app.js src/web/tuning.html
git commit -m "feat(web): verification overlay — planned-ahead target + achieved + overshoot/liquidus"
```

### Task 4.7: Drag-to-refine the tangent fit

**Files:**
- Modify: `src/web/app.js`

Spec §2.3 step 5 / §5: the operator may drag the tangent to refine the fit when auto-detection picks a poor inflection. The tangent is defined by two points already drawn in `drawChart`: the **L-intercept** on the ambient baseline (`sx(lastFit.L), syT(samples[0].temp_c)`) and the **inflection point** (`lastFit.tInflect, lastFit.tempInflect`). Dragging either recomputes `L` and `N`, then re-runs `computeGains` — all in the browser, no device round-trip.

- [ ] **Step 1: Add draggable handles + recompute on the canvas**

In `src/web/app.js`, add pointer handling that hit-tests the two tangent control points and, on drag, updates `lastFit` and recomputes. Append after the `drawChart` definition:

```js
/* Drag-to-refine: the tangent is two control points — the inflection point
 * (drives N via its slope to the L-intercept) and the L-intercept on the
 * ambient baseline (drives dead time L). Dragging recomputes N and L from the
 * two points' geometry, then recomputes gains. Geometry mirrors drawChart's
 * sx/syT mapping so hit-testing lines up with what's drawn. */
let drag = null;   // 'L' | 'inflect' | null
function chartGeom() {
    const c = $('chart'); const W = c.width, H = c.height;
    const pL = 44, pR = 44, pT = 10, pB = 24, pw = W - pL - pR, ph = H - pT - pB;
    const Tmax = 250, tMax = Math.max(60, (samples.at(-1)?.t_s) || 60);
    return {
        c, sx: t => pL + t / tMax * pw, syT: T => pT + (1 - T / Tmax) * ph,
        ix: X => (X - pL) / pw * tMax,  iy: Y => (1 - (Y - pT) / ph) * Tmax,
    };
}
function canvasXY(ev) {
    const r = $('chart').getBoundingClientRect();
    // canvas backing store is 640×320; CSS may scale it — map client→backing px.
    return { X: (ev.clientX - r.left) * ($('chart').width / r.width),
             Y: (ev.clientY - r.top) * ($('chart').height / r.height) };
}
$('chart').addEventListener('pointerdown', ev => {
    if (!lastFit || !lastFit.ok) return;
    const g = chartGeom(), { X, Y } = canvasXY(ev);
    const near = (px, py) => Math.hypot(X - px, Y - py) < 14;
    const baseT = samples[0].temp_c;
    if (near(g.sx(lastFit.L), g.syT(baseT))) drag = 'L';
    else if (near(g.sx(lastFit.tInflect), g.syT(lastFit.tempInflect))) drag = 'inflect';
    if (drag) $('chart').setPointerCapture(ev.pointerId);
});
$('chart').addEventListener('pointermove', ev => {
    if (!drag) return;
    const g = chartGeom(), { X, Y } = canvasXY(ev);
    if (drag === 'inflect') {
        lastFit.tInflect = Math.max(0, g.ix(X));
        lastFit.tempInflect = Math.max(0, g.iy(Y));
    } else {                          // 'L' — move the baseline intercept in time
        lastFit.L = Math.max(0, g.ix(X));
    }
    // Recompute N as the slope from the L-intercept (at ambient baseline) to
    // the inflection point: N = (tempInflect − baseline) / (tInflect − L).
    const baseT = samples[0].temp_c;
    const dtime = lastFit.tInflect - lastFit.L;
    lastFit.N = dtime > 0 ? (lastFit.tempInflect - baseT) / dtime : lastFit.N;
    $('f_L').textContent = lastFit.L.toFixed(0) + ' s' + (joinedLate ? ' ⚠' : '');
    $('f_N').textContent = lastFit.N.toFixed(2) + ' °C/s';
    recompute();
    drawChart();
});
$('chart').addEventListener('pointerup', () => { drag = null; });
$('chart').addEventListener('pointercancel', () => { drag = null; });
```

- [ ] **Step 2: Draw the two handles so they're visibly grabbable**

In `drawChart`, where the tangent is drawn (`if (lastFit?.ok) { ... }`), after the dashed line add small filled circles at the two control points:

```js
        const baseT = samples[0].temp_c;
        x.fillStyle = '#8b949e';
        for (const [tt, TT] of [[lastFit.L, baseT], [lastFit.tInflect, lastFit.tempInflect]]) {
            x.beginPath(); x.arc(sx(tt), syT(TT), 5, 0, 7); x.fill();
        }
```

- [ ] **Step 3: Manual check (optional, browser only)**

Open the page with a finished run loaded (or against a device after a run). Dragging either handle should move the tangent and update L, N, and the gains live. Full check is on-hardware (Phase 5).

- [ ] **Step 4: Commit**

```bash
git add src/web/app.js
git commit -m "feat(web): drag-to-refine the reaction-curve tangent (live L/N/gains)"
```

### Task 4.8: Document the web build step in the OTA flash workflow

**Files:**
- Modify: `src/web/README.md` (create)

- [ ] **Step 1: Create a short README so the build step isn't forgotten**

Create `src/web/README.md`:

```markdown
# Web tuning UI

Source files:
- `tuning.html` — dashboard markup + CSS (layout C)
- `app.js` — polling, run-lifecycle, chart, archive
- `fit.js` — pure ZN reaction-curve fit math (node-tested: `node --test test/web/`)

## Build (required before `pio run` if any of the above changed)

```
python3 tools/build_web_asset.py
```

This inlines `fit.js` + `app.js` into `tuning.html`, gzips to `tuning.html.gz`
(gitignored), which CMake embeds via `EMBED_FILES`. The device serves the
gzipped page at `GET /` with `Content-Encoding: gzip`.

Keep the gzipped size well under the app-slot headroom (see spec §6).
```

- [ ] **Step 2: Commit**

```bash
git add src/web/README.md
git commit -m "docs(web): document the embedded-asset build step"
```

---

## Phase 5 — Hardware integration & OTA regression gate

> Not host-testable; these are the on-hardware verifications the spec requires (§1.5, §9). Each is a manual checklist item, executed on the enclosed oven over WiFi.

### Task 5.1: OTA regression — the merge gate (§1.5)

**Files:** none (hardware verification)

- [ ] **Step 1: Bump version + flash via OTA over WiFi**

Follow the OTA flash workflow (CMakeLists version bump, `tools/ota_upload.py`). Run: `~/.platformio/penv/bin/pio run -e esp32dev-ota -t upload`.
Expected: upload succeeds, device validates and reboots into the new image. **If OTA fails, STOP — this is the §1.5 brick risk; do not merge.**

- [ ] **Step 2: Confirm all pre-existing routes still serve**

Run: `curl -s http://reflow.local/version` and `curl -s -o /dev/null -w '%{http_code}' http://reflow.local/`
Expected: `/version` returns the JSON with the new version; `/` returns 200.

- [ ] **Step 3: Concurrent telemetry-during-OTA socket-contention check (§1.5/§6)**

The spec requires that `/telemetry` polling not starve an in-progress `/ota` upload (shared `max_open_sockets`). With the oven idle, start polling telemetry in a loop in one terminal while uploading firmware in another:

```bash
# terminal A — hammer telemetry at the SPA's cadence during the upload
while true; do curl -s -o /dev/null -w '%{http_code} ' http://reflow.local/telemetry; sleep 1; done
# terminal B — perform the OTA upload concurrently
~/.platformio/penv/bin/pio run -e esp32dev-ota -t upload
```

Expected: the OTA upload completes successfully (not stalled/aborted) AND telemetry keeps returning 200 throughout. If the upload stalls or telemetry starts failing, the socket budget needs raising (`max_open_sockets`) before merge.

### Task 5.2: Step-test happy path + run-lifecycle on hardware

**Files:** none (hardware verification)

- [ ] **Step 1: Run a supervised step test**

At the oven: Settings → PID Tuning → set duty 50 %, ceiling 180 °C → START. Watch the live readout rise; open `reflow.local` in a browser and confirm the chart tracks.
Expected: temperature rises; test auto-ends at the ceiling; browser shows L, N, and gains.

- [ ] **Step 2: Verify run_id boundary**

STOP a test early, then immediately START another. In the browser, confirm the second run starts a **fresh** trace (no splice).
Expected: the chart resets at the new run; archive shows two distinct runs.

- [ ] **Step 3: Suggest + confirm gains**

In the browser pick a rule, click **Suggest to oven →**, enter the token. At the oven, confirm the prompt.
Expected: touchscreen shows "✓ saved"; `GET /telemetry` shows `pending_pid:null` after confirm.

- [ ] **Step 4: Verify OTA is refused while a step test runs (symmetric exclusion, spec §1.5/§3.2)**

Start a step test at the oven, then while it is running attempt an OTA upload (`pio run -e esp32dev-ota -t upload`, or `curl -X POST … http://reflow.local/ota`).
Expected: the upload is refused with HTTP 409 ("oven not idle"); the step test is unaffected.

- [ ] **Step 5: Confirm booted build matches**

Compare the booted firmware's reported version/build to the uploaded build (stale-flash gotcha).
Expected: match.

- [ ] **Step 6: No commit** (verification only). Record results in the PR description.


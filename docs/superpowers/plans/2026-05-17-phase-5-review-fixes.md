# Phase 5 Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Resolve every Critical/Important/Minor finding from the Phase-5
code + spec review pass on branch `mvp-implementation`, bringing the UI
into compliance with spec §3.3 / §7.x / §8 / §9.

**Architecture:** Three-layer change set:
1. **IPC contract extension** — `oven_state_t` gains `phase_end_ms[STAGE_COUNT_]`
   (stage timing chip) and two `*_applied_seq` counters (post-NVS-save ACK).
   These resolve the two spec bugs the review surfaced.
2. **Controller-side persistence** — NVS writes for profile + PID move
   from "missing in the UI" to `control_task.apply_non_sm_command`, after
   validation, with the applied-seq counter bumped on success. Eliminates
   the Critical "edits never persist" finding while giving the UI the ACK
   channel it needs for §7.4 / §7.6.
3. **UI screen updates** — Home cached-mode dispatch, `stage_name()` /
   `fault_text()` helpers, real graph (planned curve + live trace + FAULT
   dim + axis labels), keypad auto-close, four-stage profile layout, mode-
   gated PREVIEW/SAVE buttons, seq-driven toasts, mutex-safe curve copy.

**Tech Stack:** ESP-IDF (≥5.0), FreeRTOS, LVGL 9 (vendored under
`managed_components/lvgl__lvgl/`), `esp_lvgl_port` 2.x, PlatformIO.

**Branch:** continue on `mvp-implementation` (do **not** start a new
branch; this is a fix pass over already-merged Phase-5 commits).

---

## Phase A — IPC contract extension

### Task 1: Extend `oven_state_t` with stage timing + apply-seq fields

**Files:**
- Modify: `src/oven_state.h`

- [ ] **Step 1: Add the new fields**

Open `src/oven_state.h` and replace the entire `oven_state_t` struct
definition (currently lines 24–35) with:

```c
/* Snapshot published by control_task, read by ui_task under mutex.
 *
 * Locking: hold `mutex` for the entire read. Do not retain `curve` past
 * the unlock — control_task swaps the active curve buffer on APPLY_PROFILE
 * (Task 22), so a previously-held pointer can suddenly address what is
 * now the *idle* buffer and be overwritten by the next curve generation.
 * Either consume the curve while the lock is held, or memcpy the relevant
 * samples into a UI-owned buffer before releasing.
 *
 * `temp_c` is intentionally left at its previous value during faults
 * (the UI shows the last-known reading while a fault is latched); all
 * other fields are refreshed every tick.
 *
 * `phase_end_ms[]` is the running copy of the active curve's per-stage
 * end-time marks (in ms from start of cycle). Used by the UI to render
 * the spec §7.2 stage chip "Stage SOAK · 42 / 100 s" without re-reading
 * the curve. `phase_end_ms[STAGE_COOL_DOWN]` is left at 0 — cool-down
 * has no planned duration, so the UI hides the denominator there.
 *
 * `profile_applied_seq` / `pid_applied_seq` are monotonically-increasing
 * counters bumped by control_task *after* the corresponding NVS save
 * succeeds. The UI watches them as a one-way ACK channel: when a seq
 * advances past a remembered baseline, the corresponding "✓ saved"
 * toast can be shown. See spec §7.4 / §7.6 / §8.
 */
typedef struct {
    SemaphoreHandle_t mutex;
    float       temp_c;
    float       setpoint_c;
    stage_t     stage;
    uint32_t    elapsed_ms;
    uint8_t     duty_percent;
    uint8_t     fault_flags;
    oven_mode_t mode;
    const float *curve;                 /* see locking note above */
    uint16_t    curve_len;
    uint32_t    phase_end_ms[STAGE_COUNT_];  /* copy of active_curve->phase_end_ms */
    uint16_t    profile_applied_seq;    /* bumped after nvs_store_save_profile */
    uint16_t    pid_applied_seq;        /* bumped after nvs_store_save_pid */
} oven_state_t;
```

- [ ] **Step 2: Confirm native build still passes**

The native env builds `oven_state.h`-including files via `nvs_schema.c`
indirectly through `profile_curve.h`. Run:

```bash
~/.platformio/penv/bin/pio test -e native 2>&1 | tail -5
```

Expected: `42 test cases: 42 succeeded` (no test reads the new fields
yet; we just verify nothing breaks).

- [ ] **Step 3: Commit**

```bash
git add src/oven_state.h
git commit -m "Phase 5 review fix 1: extend oven_state_t with phase_end_ms[] + apply-seq counters

Resolves the two spec bugs the spec-compliance review surfaced (no stage
timing, no controller→UI ACK channel). Sets up §7.2 stage-chip rendering
and §7.4/§7.6 saved-toast plumbing in later tasks."
```

---

## Phase B — Controller-side persistence

### Task 2: `control_task` populates `phase_end_ms[]` + initialises seq fields

**Files:**
- Modify: `src/control/control_task.c`

- [ ] **Step 1: Extend `publish()` to copy the curve's phase_end_ms array**

In `src/control/control_task.c`, replace the current `publish()` function
(lines 30–42) with:

```c
/* Publish a full snapshot under one mutex acquisition. Pass `temp_c` only
 * when the sensor read succeeded this tick; pass NULL to retain the
 * previously-published value (the UI keeps showing the last known temp
 * across a fault). `profile_applied_seq` / `pid_applied_seq` are NOT
 * touched here — they're bumped only by apply_non_sm_command after a
 * successful NVS save. */
static void publish(uint8_t duty, float setpoint, const float *temp_c) {
    xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
    if (temp_c) s_pub->temp_c = *temp_c;
    s_pub->setpoint_c   = setpoint;
    s_pub->stage        = s_sm.stage;
    s_pub->elapsed_ms   = s_sm.elapsed_ms;
    s_pub->duty_percent = duty;
    s_pub->fault_flags  = s_sm.fault_flags;
    s_pub->mode         = s_sm.mode;
    s_pub->curve        = active_curve->samples;
    s_pub->curve_len    = active_curve->length;
    memcpy(s_pub->phase_end_ms, active_curve->phase_end_ms,
           sizeof(s_pub->phase_end_ms));
    xSemaphoreGive(s_pub->mutex);
}
```

- [ ] **Step 2: Add `#include <string.h>` at the top**

If not already present, add right after `#include "freertos/task.h"`:

```c
#include <string.h>
```

- [ ] **Step 3: Initialise seq counters at task entry**

The seq counters live in `oven_state_t` which `main.c` zero-initialises
(static storage). No explicit init needed, but document the contract in
the existing comment block in `control_task()`. After the
`nvs_store_load_*` calls (line ~111), append:

```c
    /* Seq counters start at 0 (oven_state_t is static-zero'd in main).
     * They're bumped on every successful APPLY_*-then-NVS-save below;
     * the UI compares against its remembered baseline. */
```

- [ ] **Step 4: Verify build**

```bash
~/.platformio/penv/bin/pio run -e esp32dev 2>&1 | tail -10
```

Expected: SUCCESS, footprint unchanged within ±0.1 %.

- [ ] **Step 5: Commit**

```bash
git add src/control/control_task.c
git commit -m "Phase 5 review fix 2: control_task copies phase_end_ms[] into snapshot

Enables §7.2 stage chip 'Stage SOAK · 42 / 100 s' rendering. Seq
counters left at zero-init; they'll be bumped in the next commit by
the NVS-save path."
```

---

### Task 3: `control_task` writes NVS on `CMD_APPLY_*` + bumps `*_applied_seq`

**Files:**
- Modify: `src/control/control_task.c`

This is the **Critical** fix from the spec review — currently no code
path calls `nvs_store_save_profile` / `nvs_store_save_pid`.

- [ ] **Step 1: Update `apply_non_sm_command` for `CMD_APPLY_PROFILE`**

In `src/control/control_task.c`, replace the `CMD_APPLY_PROFILE` case
(lines 61–69) with:

```c
        case CMD_APPLY_PROFILE: {
            if (s_sm.mode != MODE_IDLE) break;
            profile_t candidate = msg->payload.profile;
            if (!profile_validate(&candidate)) break;
            if (!profile_curve_generate(&candidate, idle_curve)) break;
            s_profile = candidate;
            swap_curves();
            /* §7.6 / §8: persist after validate+apply, then ACK the UI
             * by bumping the seq counter. The seq bump is the signal
             * the UI uses to fire its "✓ saved" toast. */
            nvs_store_save_profile(&candidate);
            xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
            s_pub->profile_applied_seq++;
            xSemaphoreGive(s_pub->mutex);
            break;
        }
```

- [ ] **Step 2: Update the `CMD_APPLY_PID` case**

In the same function, replace the `CMD_APPLY_PID` case (lines 70–74)
with:

```c
        case CMD_APPLY_PID:
            if (s_sm.mode == MODE_FAULT) break;
            if (!pid_validate(&msg->payload.pid)) break;
            s_pid_gains = msg->payload.pid;
            pid_set_gains(&s_pid, s_pid_gains.kp, s_pid_gains.ki, s_pid_gains.kd);
            /* §7.4 / §8: persist + ACK only when the control loop is
             * idle. nvs_set_blob + nvs_commit can stall this tick by
             * tens to hundreds of ms (flash erase / program). Doing
             * that mid-run would delay the PID step and break the
             * fixed-dt assumption in control_task (dt = CONTROL_TICK_MS).
             * Live-tuning in MODE_RUNNING still APPLIES the gains
             * immediately (pid_set_gains above) — only the NVS write
             * defers until the operator commits while idle. The UI's
             * "saved" toast won't fire on RUNNING commits, which is
             * the expected behaviour per spec §7.4 amendment. */
            if (s_sm.mode == MODE_IDLE) {
                nvs_store_save_pid(&s_pid_gains);
                xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
                s_pub->pid_applied_seq++;
                xSemaphoreGive(s_pub->mutex);
            }
            break;
```

Note: the `pid_validate` call is new here. Previously the UI was the
only validator; now that the UI no longer carries the responsibility
of persisting, the controller must validate before saving to NVS.
The `MODE_IDLE` gate on the NVS write was added in plan-review pass 2
(see Task 13 §5.1 amendment).

- [ ] **Step 3: Verify build + native tests**

```bash
~/.platformio/penv/bin/pio run -e esp32dev 2>&1 | tail -5
~/.platformio/penv/bin/pio test -e native 2>&1 | tail -5
```

Expected: both succeed, 42/42 tests still passing.

- [ ] **Step 4: Commit**

```bash
git add src/control/control_task.c
git commit -m "Phase 5 review fix 3: control_task persists profile + PID on APPLY, bumps applied_seq

Resolves the Critical 'edits never persist' finding (spec §7.6 + §8).
After successful validate+apply, control_task now calls
nvs_store_save_profile / nvs_store_save_pid and bumps the corresponding
seq counter. The seq counter is the controller→UI ACK channel that
§7.4 / §7.6 require for the 'saved' toast."
```

---

### Task 4: Document `nvs_store_load_*` defaults-on-miss contract

**Files:**
- Modify: `src/storage/nvs_store.h`

- [ ] **Step 1: Replace the file with the documented contract**

Replace `src/storage/nvs_store.h` entirely with:

```c
/* NVS I/O wrapper. Uses nvs_schema for serialisation; falls back to
 * defaults from config.h on any read error. See spec §8.
 */
#pragma once
#include "control/profile_curve.h"
#include "storage/nvs_schema.h"

void nvs_store_init(void);                            /* call once at boot */

/* nvs_store_load_*: always populates *out.
 *
 * On NVS miss, schema mismatch, or stored-value validation failure,
 * *out is overwritten with the defaults from profile_defaults() /
 * pid_defaults() and a WARN log is emitted. The functions return void
 * deliberately: callers cannot distinguish "loaded from NVS" from
 * "fell back to defaults", but they don't need to — *out is always
 * safe to use.
 *
 * Implementation contract (nvs_store.c): the *_defaults() call is the
 * first statement of each function, before any NVS I/O is attempted.
 * Every early-return path therefore leaves the defaults in place. */
void nvs_store_load_profile(profile_t *out);
void nvs_store_save_profile(const profile_t *p);

void nvs_store_load_pid(pid_gains_t *out);
void nvs_store_save_pid(const pid_gains_t *g);
```

- [ ] **Step 2: Commit**

```bash
git add src/storage/nvs_store.h
git commit -m "Phase 5 review fix 4: document nvs_store defaults-on-miss contract

The implementation already overwrites *out with defaults before any
I/O, but the header didn't say so. Making the contract explicit closes
the code reviewer's flag without any behaviour change."
```

---

## Phase C — Control helpers

### Task 5: `stage_name()` + `fault_text()` helpers in `reflow_sm`

**Files:**
- Modify: `src/control/reflow_sm.h`
- Modify: `src/control/reflow_sm.c`
- Create: `test/test_reflow_sm_helpers/test_reflow_sm_helpers.c`

The UI needs human-readable strings for the stage chip and fault line.
These belong with the enums they translate, not in a UI-side switch.

- [ ] **Step 1: Add declarations to `reflow_sm.h`**

Append to `src/control/reflow_sm.h` (after line 42, before EOF):

```c
/* Human-readable name for a stage_t. Returns a static string; never
 * returns NULL. Caller must NOT free. Used by the UI stage chip
 * (spec §7.2). */
const char *stage_name(stage_t s);

/* Human-readable reason for fault_flags. Returns the first set bit's
 * description ("thermocouple open", "stale sensor", "over-temperature",
 * etc.) — if multiple bits are set, only the highest-priority one is
 * shown. Returns a static string; never returns NULL ("unknown" on
 * all-zero or unrecognised). Used by the UI fault line (spec §7.2). */
const char *fault_text(uint8_t fault_flags);
```

- [ ] **Step 2: Add the implementations to `reflow_sm.c`**

Locate the end of `src/control/reflow_sm.c` and append:

```c
const char *stage_name(stage_t s) {
    switch (s) {
        case STAGE_NONE:      return "IDLE";
        case STAGE_HEAT_UP_1: return "HEAT_UP_1";
        case STAGE_SOAK:      return "SOAK";
        case STAGE_HEAT_UP_2: return "HEAT_UP_2";
        case STAGE_REFLOW:    return "REFLOW";
        case STAGE_COOL_DOWN: return "COOL_DOWN";
        case STAGE_COUNT_:    return "?";  /* not a real stage */
    }
    return "?";
}

const char *fault_text(uint8_t fault_flags) {
    if (fault_flags & FAULT_TC_OPEN)   return "thermocouple open";
    if (fault_flags & FAULT_TC_SCG)    return "thermocouple short to GND";
    if (fault_flags & FAULT_TC_SCV)    return "thermocouple short to VCC";
    if (fault_flags & FAULT_TC_STALE)  return "stale sensor";
    if (fault_flags & FAULT_OVER_TEMP) return "over-temperature";
    return "unknown";
}
```

- [ ] **Step 3: Write the failing test**

Create `test/test_reflow_sm_helpers/test_reflow_sm_helpers.c`:

```c
#include "unity.h"
#include "control/reflow_sm.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_stage_name_covers_every_value(void) {
    TEST_ASSERT_EQUAL_STRING("IDLE",      stage_name(STAGE_NONE));
    TEST_ASSERT_EQUAL_STRING("HEAT_UP_1", stage_name(STAGE_HEAT_UP_1));
    TEST_ASSERT_EQUAL_STRING("SOAK",      stage_name(STAGE_SOAK));
    TEST_ASSERT_EQUAL_STRING("HEAT_UP_2", stage_name(STAGE_HEAT_UP_2));
    TEST_ASSERT_EQUAL_STRING("REFLOW",    stage_name(STAGE_REFLOW));
    TEST_ASSERT_EQUAL_STRING("COOL_DOWN", stage_name(STAGE_COOL_DOWN));
}

static void test_stage_name_never_returns_null(void) {
    /* Out-of-range value still returns a string. */
    TEST_ASSERT_NOT_NULL(stage_name((stage_t)42));
}

static void test_fault_text_none(void) {
    TEST_ASSERT_EQUAL_STRING("unknown", fault_text(0));
}

static void test_fault_text_each_flag(void) {
    TEST_ASSERT_EQUAL_STRING("thermocouple open",        fault_text(FAULT_TC_OPEN));
    TEST_ASSERT_EQUAL_STRING("thermocouple short to GND", fault_text(FAULT_TC_SCG));
    TEST_ASSERT_EQUAL_STRING("thermocouple short to VCC", fault_text(FAULT_TC_SCV));
    TEST_ASSERT_EQUAL_STRING("stale sensor",             fault_text(FAULT_TC_STALE));
    TEST_ASSERT_EQUAL_STRING("over-temperature",         fault_text(FAULT_OVER_TEMP));
}

static void test_fault_text_priority_when_multiple_set(void) {
    /* TC_OPEN has highest priority. */
    uint8_t both = FAULT_TC_OPEN | FAULT_OVER_TEMP;
    TEST_ASSERT_EQUAL_STRING("thermocouple open", fault_text(both));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_stage_name_covers_every_value);
    RUN_TEST(test_stage_name_never_returns_null);
    RUN_TEST(test_fault_text_none);
    RUN_TEST(test_fault_text_each_flag);
    RUN_TEST(test_fault_text_priority_when_multiple_set);
    return UNITY_END();
}
```

- [ ] **Step 4: Run the tests**

```bash
~/.platformio/penv/bin/pio test -e native 2>&1 | tail -10
```

Expected: `47 test cases: 47 succeeded` (existing 42 plus 5 new).

- [ ] **Step 5: Commit**

```bash
git add src/control/reflow_sm.h src/control/reflow_sm.c \
        test/test_reflow_sm_helpers/test_reflow_sm_helpers.c
git commit -m "Phase 5 review fix 5: stage_name() and fault_text() helpers + native tests

Moves enum-to-string translation out of the UI and next to the enums.
Five new native tests cover every stage and every fault flag plus the
multi-bit priority rule."
```

---

## Phase D — UI Home screen

### Task 6: `screen_home` — cached-mode dispatch, helpers, format casts, no-flash boot

**Files:**
- Modify: `src/ui/screen_home.c`

- [ ] **Step 1: Replace the file**

Replace `src/ui/screen_home.c` entirely with:

```c
#include "ui/screen_home.h"
#include "ui/theme.h"
#include "oven_state.h"
#include "control/reflow_sm.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>

extern QueueHandle_t g_cmd_queue;   /* defined in ui_task.c */

static lv_obj_t *lbl_temp, *lbl_target, *lbl_stage, *btn_action, *btn_action_lbl;
static lv_obj_t *graph_canvas;
static oven_mode_t s_mode_cached = MODE_IDLE;   /* updated by screen_home_update */

static void action_clicked_cb(lv_event_t *e) {
    (void)e;
    cmd_msg_t msg = {0};
    /* Dispatch by cached mode, not label text — label is refreshed
     * every 250 ms so a strcmp on it can lag the real state. */
    switch (s_mode_cached) {
        case MODE_IDLE:    msg.type = CMD_START;     break;
        case MODE_RUNNING: msg.type = CMD_STOP;      break;
        case MODE_FAULT:   msg.type = CMD_ACK_FAULT; break;
        default: return;
    }
    xQueueSend(g_cmd_queue, &msg, 0);
}

void screen_home_create(lv_obj_t *parent) {
    lv_obj_set_style_bg_color(parent, THEME_BG, 0);
    lv_obj_set_style_pad_all(parent, 12, 0);

    /* Big temp — initialise to the IDLE colour to avoid a one-tick
     * orange flash at boot before the first refresh tick repaints. */
    lbl_temp = lv_label_create(parent);
    lv_label_set_text(lbl_temp, "--C");
    lv_obj_set_style_text_color(lbl_temp, THEME_FG_DIM, 0);
    lv_obj_set_style_text_font(lbl_temp, &lv_font_montserrat_48, 0);
    lv_obj_set_pos(lbl_temp, 0, 0);

    /* Target line */
    lbl_target = lv_label_create(parent);
    lv_label_set_text(lbl_target, "Ambient - ready to start");
    lv_obj_set_style_text_color(lbl_target, THEME_FG_DIM, 0);
    lv_obj_set_pos(lbl_target, 0, 70);

    /* Stage chip */
    lbl_stage = lv_label_create(parent);
    lv_label_set_text(lbl_stage, "Stage IDLE");
    lv_obj_set_style_text_color(lbl_stage, THEME_FG_DIM, 0);
    lv_obj_set_style_bg_color(lbl_stage, THEME_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(lbl_stage, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(lbl_stage, 5, 0);
    lv_obj_set_pos(lbl_stage, 0, 95);

    /* Graph placeholder (filled in Task 7). */
    graph_canvas = lv_obj_create(parent);
    lv_obj_set_size(graph_canvas, 296, 220);
    lv_obj_set_style_bg_color(graph_canvas, THEME_PANEL_BG, 0);
    lv_obj_set_style_border_color(graph_canvas, THEME_BORDER, 0);
    lv_obj_set_pos(graph_canvas, 0, 130);

    /* Action button */
    btn_action = lv_button_create(parent);
    lv_obj_set_size(btn_action, 296, 50);
    lv_obj_set_pos(btn_action, 0, 360);
    lv_obj_set_style_bg_color(btn_action, THEME_GO, 0);
    lv_obj_add_event_cb(btn_action, action_clicked_cb, LV_EVENT_CLICKED, NULL);
    btn_action_lbl = lv_label_create(btn_action);
    lv_label_set_text(btn_action_lbl, "START");
    lv_obj_set_style_text_color(btn_action_lbl, THEME_FG, 0);
    lv_obj_center(btn_action_lbl);
}

void screen_home_update(const oven_state_t *s) {
    s_mode_cached = s->mode;

    char buf[80];
    snprintf(buf, sizeof(buf), "%.0fC", s->temp_c);
    lv_label_set_text(lbl_temp, buf);

    switch (s->mode) {
    case MODE_IDLE:
        lv_label_set_text(lbl_target, "Ambient - ready to start");
        lv_label_set_text(lbl_stage,  "Stage IDLE");
        lv_label_set_text(btn_action_lbl, "START");
        lv_obj_set_style_bg_color(btn_action, THEME_GO, 0);
        lv_obj_set_style_text_color(lbl_temp, THEME_FG_DIM, 0);
        break;
    case MODE_RUNNING: {
        /* Cast uint32_t to unsigned for %u (xtensa-gcc treats uint32_t as
         * long unsigned; bare %u would trip -Werror=format). */
        snprintf(buf, sizeof(buf), "Target %.0f C - elapsed %u:%02u",
                 s->setpoint_c,
                 (unsigned)(s->elapsed_ms / 60000),
                 (unsigned)((s->elapsed_ms / 1000) % 60));
        lv_label_set_text(lbl_target, buf);

        /* Stage chip per spec §7.2: "Stage SOAK - 42 / 100 s".
         * stage_start = phase_end_ms[stage-1] (or 0 for the first stage);
         * stage_dur   = phase_end_ms[stage] - stage_start.
         * COOL_DOWN has no planned duration — show elapsed only. */
        const char *name = stage_name(s->stage);
        if (s->stage == STAGE_COOL_DOWN) {
            uint32_t stage_start = s->phase_end_ms[STAGE_REFLOW];
            uint32_t stage_elapsed = (s->elapsed_ms > stage_start)
                                    ? (s->elapsed_ms - stage_start) : 0;
            snprintf(buf, sizeof(buf), "Stage %s - %us",
                     name, (unsigned)(stage_elapsed / 1000));
        } else if (s->stage >= STAGE_HEAT_UP_1 && s->stage < STAGE_COOL_DOWN) {
            uint32_t stage_start = (s->stage > STAGE_HEAT_UP_1)
                                 ? s->phase_end_ms[s->stage - 1] : 0;
            uint32_t stage_dur = s->phase_end_ms[s->stage] - stage_start;
            uint32_t stage_elapsed = (s->elapsed_ms > stage_start)
                                    ? (s->elapsed_ms - stage_start) : 0;
            snprintf(buf, sizeof(buf), "Stage %s - %u / %u s",
                     name,
                     (unsigned)(stage_elapsed / 1000),
                     (unsigned)(stage_dur / 1000));
        } else {
            snprintf(buf, sizeof(buf), "Stage %s", name);
        }
        lv_label_set_text(lbl_stage, buf);

        lv_label_set_text(btn_action_lbl, "STOP");
        lv_obj_set_style_bg_color(btn_action, THEME_STOP, 0);
        lv_obj_set_style_text_color(lbl_temp, THEME_ACCENT, 0);
        break;
    }
    case MODE_FAULT:
        snprintf(buf, sizeof(buf), "FAULT: %s",
                 fault_text(s->fault_flags));
        lv_label_set_text(lbl_target, buf);
        lv_label_set_text(lbl_stage,  "Tap ACKNOWLEDGE to clear");
        lv_label_set_text(btn_action_lbl, "ACKNOWLEDGE");
        lv_obj_set_style_bg_color(btn_action, THEME_STAGE, 0);
        lv_obj_set_style_text_color(lbl_temp, THEME_STOP, 0);
        break;
    }
    /* Graph rendering: see Task 7. */
}
```

Key changes from the previous version:
- `s_mode_cached` file-static, written at the top of every update,
  read by the click handler — no more `strcmp` on label text.
- Stage chip uses `stage_name()` + `phase_end_ms[]` per spec §7.2.
- FAULT line uses `fault_text()` per spec §7.2.
- `(unsigned)` casts already present from earlier round.
- Initial `lbl_temp` colour is `THEME_FG_DIM` (no boot flash).
- Glyphs `·` and `–` replaced with plain `-` (defensive: not all
  Montserrat builds carry U+00B7 / U+2013).
- `°C` replaced with plain `C` for the same reason.

- [ ] **Step 2: Verify build**

```bash
~/.platformio/penv/bin/pio run -e esp32dev 2>&1 | tail -5
```

Expected: SUCCESS.

- [ ] **Step 3: Commit**

```bash
git add src/ui/screen_home.c
git commit -m "Phase 5 review fix 6: screen_home — cached mode, stage_name, fault_text, ASCII glyphs

- Click handler dispatches on cached oven_mode_t (set by every refresh
  tick), no more strcmp on the button label text.
- Stage chip uses stage_name() + phase_end_ms[] for the spec §7.2
  'Stage SOAK - 42 / 100 s' shape.
- FAULT line uses fault_text() instead of raw hex bitmask.
- lbl_temp initial colour is THEME_FG_DIM (no orange flash at boot).
- Glyphs · / – / ° pre-emptively swapped for ASCII to remove the risk
  that they're not in the configured Montserrat subset."
```

---

### Task 7: `screen_home` — graph (planned curve, live trace, FAULT dim, gridlines + labels)

**Files:**
- Modify: `include/config.h`
- Modify: `src/ui/screen_home.h`
- Modify: `src/ui/screen_home.c`

This is the largest single change in this plan. We add two `lv_line`
objects on the existing graph canvas — one for the planned curve, one
for the live trace — plus four gridlines and four labels. Both lines
are stored as `lv_point_precise_t[]` in file-static buffers.

- [ ] **Step 1a: Add the tunable graph constants to `include/config.h`**

Per the project's "config.h single source" rule
(`memory/feedback_config_h_single_source.md`), tunable constants live
in `include/config.h`. Append to the `/* ADJUSTABLE PARAMETER BOUNDS */`
section of `include/config.h`:

```c
/* ============================================================
 * UI — graph rendering (spec §7.2)
 * ============================================================ */
/* Live-trace ring buffer depth. At 250 ms refresh, 600 samples = ~2.5 min
 * visible — enough to see soak + reflow at once. Static cost: 600 * 8 B
 * = 4.8 KB. */
#define GRAPH_GRAPH_LIVE_RING_CAP   600

/* UI refresh tick. Independent of CONTROL_TICK_MS; the UI just needs
 * to be fast enough that the user perceives the temperature label as
 * "live". Cheap to lower; expensive to raise much above 100 ms only
 * if LVGL render becomes the bottleneck. */
#define UI_REFRESH_MS         250
```

The graph's upper temperature rail is **not** a new constant — it
references `OVER_TEMP_LIMIT_C` (already defined in `config.h` §SAFETY
LIMITS) directly. The earlier plan draft had a duplicate
`GRAPH_TEMP_MAX_C = 260.0f` next to it; that's been removed.

- [ ] **Step 1b: Add `<stdint.h>` and the graph state at the top of `screen_home.c`**

In `src/ui/screen_home.c`, after the existing static widget pointers
(around line 11), add:

```c
/* Graph layout: 296 × 220 canvas at (0, 130). Use an inner plot area
 * inset for axis labels. These are render-only pixel coordinates, not
 * tunable parameters, so they stay local to this file. */
#define GRAPH_W            296
#define GRAPH_H            220
#define GRAPH_INSET_LEFT    28   /* room for "260" Y-axis label */
#define GRAPH_INSET_RIGHT    4
#define GRAPH_INSET_TOP      4
#define GRAPH_INSET_BOTTOM  14   /* room for time X-axis label */
#define GRAPH_PLOT_W (GRAPH_W - GRAPH_INSET_LEFT - GRAPH_INSET_RIGHT)
#define GRAPH_PLOT_H (GRAPH_H - GRAPH_INSET_TOP  - GRAPH_INSET_BOTTOM)

/* Temperature plot range. Lower rail at 0 C; upper rail uses the
 * over-temp limit so the plot covers the whole safe operating range
 * without duplicating the constant. */
#define GRAPH_TEMP_MIN_C   0.0f

/* Planned-curve buffer. Static so it lives for the program's lifetime
 * and so lv_line_set_points is safe to call without copy.
 * PROFILE_CURVE_MAX_SAMPLES comes from profile_curve.h. Sized for the
 * worst-case curve length so we never need to grow it dynamically. */
static lv_point_precise_t s_plan_pts[PROFILE_CURVE_MAX_SAMPLES];

/* Live trace ring buffer — overwrite-oldest when full. Capacity in
 * include/config.h as GRAPH_GRAPH_LIVE_RING_CAP. */
static lv_point_precise_t s_live_pts[GRAPH_GRAPH_LIVE_RING_CAP];
static uint16_t           s_live_n;

static lv_obj_t *line_planned, *line_live;
static lv_obj_t *lbl_axis_t_min, *lbl_axis_t_max, *lbl_axis_y_top, *lbl_axis_y_bot;
static lv_obj_t *grid_lines[4];
```

Note: `s_plan_n` was dropped — the planned-curve point count is passed
straight to `lv_line_set_points` from the local `n` parameter in
`screen_home_set_curve` (Step 4) and never read elsewhere.

- [ ] **Step 2: Helper functions for coordinate mapping**

Place these `static` helpers just above `action_clicked_cb`:

```c
static int32_t graph_x_from_ms(uint32_t t_ms, uint32_t t_max_ms) {
    if (t_max_ms == 0) return GRAPH_INSET_LEFT;
    if (t_ms > t_max_ms) t_ms = t_max_ms;
    return GRAPH_INSET_LEFT +
           (int32_t)((uint64_t)t_ms * GRAPH_PLOT_W / t_max_ms);
}

static int32_t graph_y_from_c(float t_c) {
    if (t_c < GRAPH_TEMP_MIN_C) t_c = GRAPH_TEMP_MIN_C;
    if (t_c > OVER_TEMP_LIMIT_C) t_c = OVER_TEMP_LIMIT_C;
    float ratio = (t_c - GRAPH_TEMP_MIN_C) / (OVER_TEMP_LIMIT_C - GRAPH_TEMP_MIN_C);
    /* Invert: higher temp = lower Y pixel. */
    return GRAPH_INSET_TOP + (int32_t)((1.0f - ratio) * GRAPH_PLOT_H);
}
```

- [ ] **Step 3: Extend `screen_home_create` with the graph widgets**

Replace the existing graph-placeholder block (the `graph_canvas = lv_obj_create…`
block) with:

```c
    /* === Graph === per spec §7.2.
     *   - lvl gridlines + axis labels for orientation
     *   - planned curve in grey (THEME_FG_FAINT)
     *   - live measured trace in orange (THEME_ACCENT)
     *   - dimmed in FAULT (handled in screen_home_update)
     */
    graph_canvas = lv_obj_create(parent);
    lv_obj_set_size(graph_canvas, GRAPH_W, GRAPH_H);
    lv_obj_set_style_bg_color(graph_canvas, THEME_PANEL_BG, 0);
    lv_obj_set_style_border_color(graph_canvas, THEME_BORDER, 0);
    lv_obj_set_style_pad_all(graph_canvas, 0, 0);
    lv_obj_clear_flag(graph_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(graph_canvas, 0, 130);

    /* Horizontal gridlines at 25 %, 50 %, 75 % of the plot height. */
    for (int i = 0; i < 4; i++) {
        static lv_point_precise_t pts[4][2];
        int32_t y = GRAPH_INSET_TOP + (int32_t)((i + 1) * GRAPH_PLOT_H / 5);
        pts[i][0].x = GRAPH_INSET_LEFT;
        pts[i][0].y = y;
        pts[i][1].x = GRAPH_INSET_LEFT + GRAPH_PLOT_W;
        pts[i][1].y = y;
        grid_lines[i] = lv_line_create(graph_canvas);
        lv_line_set_points(grid_lines[i], pts[i], 2);
        lv_obj_set_style_line_color(grid_lines[i], THEME_BORDER, 0);
        lv_obj_set_style_line_width(grid_lines[i], 1, 0);
    }

    /* Y-axis labels (top = 260 C, bottom = 0 C). */
    lbl_axis_y_top = lv_label_create(graph_canvas);
    lv_label_set_text(lbl_axis_y_top, "260");
    lv_obj_set_style_text_color(lbl_axis_y_top, THEME_FG_FAINT, 0);
    lv_obj_set_pos(lbl_axis_y_top, 2, GRAPH_INSET_TOP);

    lbl_axis_y_bot = lv_label_create(graph_canvas);
    lv_label_set_text(lbl_axis_y_bot, "0");
    lv_obj_set_style_text_color(lbl_axis_y_bot, THEME_FG_FAINT, 0);
    lv_obj_set_pos(lbl_axis_y_bot, 2, GRAPH_INSET_TOP + GRAPH_PLOT_H - 12);

    /* X-axis labels (filled in by screen_home_update once we know
     * the cycle length). Both anchored at the same Y so they share a
     * baseline. */
    lbl_axis_t_min = lv_label_create(graph_canvas);
    lv_label_set_text(lbl_axis_t_min, "0s");
    lv_obj_set_style_text_color(lbl_axis_t_min, THEME_FG_FAINT, 0);
    lv_obj_set_pos(lbl_axis_t_min, GRAPH_INSET_LEFT, GRAPH_H - 12);

    lbl_axis_t_max = lv_label_create(graph_canvas);
    lv_label_set_text(lbl_axis_t_max, "--");
    lv_obj_set_style_text_color(lbl_axis_t_max, THEME_FG_FAINT, 0);
    lv_obj_align(lbl_axis_t_max, LV_ALIGN_BOTTOM_RIGHT, -4, -2);

    /* Planned curve line. Populated by screen_home_set_curve. */
    line_planned = lv_line_create(graph_canvas);
    lv_obj_set_style_line_color(line_planned, THEME_FG_FAINT, 0);
    lv_obj_set_style_line_width(line_planned, 2, 0);

    /* Live trace line. */
    line_live = lv_line_create(graph_canvas);
    lv_obj_set_style_line_color(line_live, THEME_ACCENT, 0);
    lv_obj_set_style_line_width(line_live, 2, 0);
```

- [ ] **Step 4: Add `screen_home_set_curve` — called from ui_task when the curve buffer is refreshed**

Add to `src/ui/screen_home.h` (after `screen_home_update`):

```c
/* Re-render the planned-curve line. Called from ui_task whenever the
 * snapshot's curve buffer was copied (i.e. every refresh tick, since
 * the UI owns its own copy). The caller must NOT hold the state mutex
 * during this call — LVGL operations are routed through the LVGL port
 * lock which the refresh timer already owns. */
void screen_home_set_curve(const float *samples, uint16_t n,
                           uint32_t cycle_total_ms);
```

Add to `src/ui/screen_home.c` (place near `screen_home_update`):

```c
void screen_home_set_curve(const float *samples, uint16_t n,
                           uint32_t cycle_total_ms) {
    if (n > PROFILE_CURVE_MAX_SAMPLES) n = PROFILE_CURVE_MAX_SAMPLES;
    if (n == 0 || cycle_total_ms == 0) {
        lv_line_set_points(line_planned, NULL, 0);
        lv_label_set_text(lbl_axis_t_max, "--");
        return;
    }
    /* Each sample represents PROFILE_CURVE_STEP_MS of elapsed time. */
    for (uint16_t i = 0; i < n; i++) {
        uint32_t t_ms = (uint32_t)i * PROFILE_CURVE_STEP_MS;
        s_plan_pts[i].x = graph_x_from_ms(t_ms, cycle_total_ms);
        s_plan_pts[i].y = graph_y_from_c(samples[i]);
    }
    lv_line_set_points(line_planned, s_plan_pts, n);

    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "%us", (unsigned)(cycle_total_ms / 1000));
    lv_label_set_text(lbl_axis_t_max, tbuf);
}
```

Note: `PROFILE_CURVE_STEP_MS` lives in `include/config.h`. Add an
explicit `#include "config.h"` at the top of `src/ui/screen_home.c`
if not already present.

- [ ] **Step 5: Extend `screen_home_update` to append the live sample + recolour on FAULT**

Inside `screen_home_update`, after the `s_mode_cached = s->mode;` line,
insert:

```c
    /* === Graph live trace === */
    /* Recolour both lines based on mode (FAULT dims everything to
     * convey "this trace is frozen / not authoritative"). */
    lv_color_t plan_col = (s->mode == MODE_FAULT) ? THEME_BORDER  : THEME_FG_FAINT;
    lv_color_t live_col = (s->mode == MODE_FAULT) ? THEME_FG_DIM  : THEME_ACCENT;
    lv_obj_set_style_line_color(line_planned, plan_col, 0);
    lv_obj_set_style_line_color(line_live,    live_col, 0);

    if (s->mode == MODE_RUNNING) {
        /* Append (elapsed_ms, temp_c) into the ring. Once full, shift
         * left by one slot — cheap, the cap is 600 samples. */
        if (s_live_n < GRAPH_LIVE_RING_CAP) {
            uint32_t t_max_ms = s->phase_end_ms[STAGE_REFLOW];
            s_live_pts[s_live_n].x = graph_x_from_ms(s->elapsed_ms, t_max_ms);
            s_live_pts[s_live_n].y = graph_y_from_c(s->temp_c);
            s_live_n++;
        } else {
            memmove(s_live_pts, s_live_pts + 1,
                    (GRAPH_LIVE_RING_CAP - 1) * sizeof(s_live_pts[0]));
            uint32_t t_max_ms = s->phase_end_ms[STAGE_REFLOW];
            s_live_pts[GRAPH_LIVE_RING_CAP - 1].x =
                graph_x_from_ms(s->elapsed_ms, t_max_ms);
            s_live_pts[GRAPH_LIVE_RING_CAP - 1].y = graph_y_from_c(s->temp_c);
        }
        lv_line_set_points(line_live, s_live_pts, s_live_n);
    } else if (s->mode == MODE_IDLE) {
        /* Returning to IDLE — clear the live trace so the next run
         * starts from an empty canvas. */
        s_live_n = 0;
        lv_line_set_points(line_live, NULL, 0);
    }
    /* MODE_FAULT: leave the live trace as it stood at the moment of
     * the fault (already drawn, just recoloured). */
```

- [ ] **Step 6: Verify build**

```bash
~/.platformio/penv/bin/pio run -e esp32dev 2>&1 | tail -10
```

Expected: SUCCESS. RAM usage rises by **~16.8 KB**:
- `s_plan_pts[PROFILE_CURVE_MAX_SAMPLES=1500]` of `lv_point_precise_t`
  (8 B per point on a 32-bit target) = **12.0 KB**.
- `s_live_pts[GRAPH_LIVE_RING_CAP=600]` of `lv_point_precise_t`
  = **4.8 KB**.

Flash up by ~2 KB. Cross-check: post-build "RAM used" should be in the
range 33–35 % (started at 28.4 %). The remaining ~6 KB RAM growth this
plan introduces (the UI-owned `s_curve_copy` in Task 12) lands then.
See Task 15 Step 2 for the consolidated post-plan budget.

- [ ] **Step 7: Commit**

```bash
git add src/ui/screen_home.h src/ui/screen_home.c
git commit -m "Phase 5 review fix 7: screen_home graph — planned curve, live trace, FAULT dim, axis labels

Resolves spec §7.2 bullet 4. Two lv_line objects on the existing graph
canvas: planned curve (grey, populated by screen_home_set_curve which
ui_task calls when the curve buffer refreshes) and live trace (orange,
ring-buffered append). Four gridlines + four axis labels for
orientation. FAULT mode recolours both lines via THEME_FG_DIM. IDLE
clears the live ring."
```

---

## Phase E — UI Keypad

### Task 8: `screen_keypad` — auto-close on re-open, grow header buffer, ASCII glyphs

**Files:**
- Modify: `src/ui/screen_keypad.c`

- [ ] **Step 1: Guard `screen_keypad_open` against re-entry**

In `src/ui/screen_keypad.c`, locate the `screen_keypad_open` function.
At the very top of the function body — before `s_cfg = *cfg;` — insert:

```c
    if (s_modal) screen_keypad_close();
```

This makes the modal single-instance from the keypad's perspective.
Callers don't need to remember to check; the keypad enforces it.

- [ ] **Step 2: Grow the header buffer 64 → 128**

In the same function, change:

```c
    char hdrbuf[64];
```

to:

```c
    char hdrbuf[128];
```

`hdrbuf` is on the stack; the extra 64 bytes is trivial.

- [ ] **Step 3: Swap `·` / `–` for ASCII in the header**

In the same function, change:

```c
    snprintf(hdrbuf, sizeof(hdrbuf), "%s · %s · %g–%g",
             cfg->field_label, cfg->unit, cfg->min, cfg->max);
```

to:

```c
    snprintf(hdrbuf, sizeof(hdrbuf), "%s - %s - %g to %g",
             cfg->field_label, cfg->unit, cfg->min, cfg->max);
```

(Same defensive ASCII-only stance as `screen_home.c`.)

- [ ] **Step 4: Verify build**

```bash
~/.platformio/penv/bin/pio run -e esp32dev 2>&1 | tail -5
```

Expected: SUCCESS.

- [ ] **Step 5: Commit**

```bash
git add src/ui/screen_keypad.c
git commit -m "Phase 5 review fix 8: screen_keypad — auto-close on re-open, larger hdrbuf, ASCII glyphs

- Single-instance guard at the top of screen_keypad_open: closes the
  prior modal before creating a new one. Eliminates the heap-leak
  tripwire the code reviewer flagged.
- hdrbuf 64 -> 128: 'Reflow Temperature - °C - 150 to 250' fits with
  headroom for future field-name growth.
- Glyphs · / – swapped for plain ASCII '-' / 'to' to remove the risk
  of missing-glyph rendering."
```

---

## Phase F — UI Profile screen

### Task 9: `screen_profile` — restructure to four stage cards

**Files:**
- Modify: `src/ui/screen_profile.c`

Spec §7.3: "four stage cards stacked — Heat-up 1, Soak (Temp + Time),
Heat-up 2, Reflow (Temp + Time)". We currently render six independent
flat cards. The fix is two new layout helpers (single-value card,
dual-value card) that together compose the four spec cards.

- [ ] **Step 1: Replace the field-creation block in `screen_profile_create`**

In `src/ui/screen_profile.c`, locate the `make_field` static function
(currently ~lines 75–95). Delete it and replace with two new helpers:

```c
/* Single-value stage card: one tap target spanning the whole card. */
static void make_single_field_card(lv_obj_t *parent, const char *title,
                                   const char *unit, int y, int h,
                                   field_id_t id) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 296, h);
    lv_obj_set_pos(card, 0, y);
    lv_obj_set_style_bg_color(card, THEME_PANEL_BG, 0);
    lv_obj_set_style_border_color(card, THEME_BORDER, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, field_clicked_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)id);

    lv_obj_t *l = lv_label_create(card);
    lv_label_set_text(l, title);
    lv_obj_set_style_text_color(l, THEME_FG, 0);
    lv_obj_set_pos(l, 8, 12);

    lv_obj_t *u = lv_label_create(card);
    lv_label_set_text(u, unit);
    lv_obj_set_style_text_color(u, THEME_FG_FAINT, 0);
    lv_obj_set_pos(u, 8, 30);

    lv_obj_t *v = lv_label_create(card);
    lv_obj_set_style_text_color(v, THEME_ACCENT, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -8, 0);
    s_val_labels[id] = v;
}

/* Dual-value stage card: title at top, two clickable sub-rows below. */
static void make_dual_field_card(lv_obj_t *parent, const char *title,
                                 const char *sub_a, const char *unit_a,
                                 field_id_t id_a,
                                 const char *sub_b, const char *unit_b,
                                 field_id_t id_b,
                                 int y) {
    /* Outer card 296 x 90: title (24), sub-row a (33), sub-row b (33). */
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 296, 90);
    lv_obj_set_pos(card, 0, y);
    lv_obj_set_style_bg_color(card, THEME_PANEL_BG, 0);
    lv_obj_set_style_border_color(card, THEME_BORDER, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(card, 0, 0);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, THEME_FG, 0);
    lv_obj_set_pos(t, 8, 4);

    /* Sub-row helper. */
    struct { const char *sub; const char *unit; field_id_t id; int y_off; }
        rows[2] = {
            { sub_a, unit_a, id_a, 26 },
            { sub_b, unit_b, id_b, 56 },
        };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *row = lv_obj_create(card);
        lv_obj_set_size(row, 296, 30);
        lv_obj_set_pos(row, 0, rows[i].y_off);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, field_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)rows[i].id);

        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, rows[i].sub);
        lv_obj_set_style_text_color(l, THEME_FG_DIM, 0);
        lv_obj_set_pos(l, 16, 6);

        lv_obj_t *u = lv_label_create(row);
        lv_label_set_text(u, rows[i].unit);
        lv_obj_set_style_text_color(u, THEME_FG_FAINT, 0);
        lv_obj_set_pos(u, 100, 6);

        lv_obj_t *v = lv_label_create(row);
        lv_obj_set_style_text_color(v, THEME_ACCENT, 0);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, -8, 0);
        s_val_labels[rows[i].id] = v;
    }
}
```

- [ ] **Step 2: Replace the six `make_field` calls with the four-card layout**

Inside `screen_profile_create`, replace the six existing `make_field`
calls with:

```c
    /* Four stage cards per spec §7.3:
     *   Heat-up 1    (single-value: rate)              y=0,   h=56
     *   Soak         (dual-value: temp + time)          y=62
     *   Heat-up 2    (single-value: rate)              y=158, h=56
     *   Reflow       (dual-value: temp + time)          y=220
     */
    make_single_field_card(parent, "Heat-up 1 rate",  "C/s",  0,   56, F_RATE1);
    make_dual_field_card  (parent, "Soak",
                           "Temperature", "C",        F_SOAK_T,
                           "Time",        "s",        F_SOAK_TIME,
                           62);
    make_single_field_card(parent, "Heat-up 2 rate",  "C/s",  158, 56, F_RATE2);
    make_dual_field_card  (parent, "Reflow",
                           "Temperature", "C",        F_REFLOW_T,
                           "Time",        "s",        F_REFLOW_TIME,
                           220);
```

- [ ] **Step 3: ASCII-ize the keypad unit strings in `field_clicked_cb`**

`field_clicked_cb` builds `cfg.unit` literals that flow into the
keypad header. The existing strings use the `°` glyph
(U+00B0) which may not be present in the configured Montserrat
subset. Tasks 6 + 8 already pre-emptively swapped these glyphs
elsewhere; close the gap here so the defensive ASCII-only stance is
end-to-end.

In `src/ui/screen_profile.c`, locate `field_clicked_cb` (currently
lines 44–56) and replace the unit literals:

| Old | New |
|---|---|
| `"°C/s"` (F_RATE1 + F_RATE2) | `"C/s"` |
| `"°C"`   (F_SOAK_T + F_REFLOW_T) | `"C"` |

Concretely, the four affected case lines become:

```c
    case F_RATE1:       cfg = (keypad_config_t){"Heat-up 1 rate","C/s",HEATUP_RATE_MIN,HEATUP_RATE_MAX,s_buf.first_heatup_rate,true,keypad_done,(void*)(intptr_t)id};  break;
    case F_SOAK_T:      cfg = (keypad_config_t){"Soak Temperature","C",SOAK_TEMP_MIN,SOAK_TEMP_MAX,s_buf.soak_temp,false,keypad_done,(void*)(intptr_t)id}; break;
    case F_RATE2:       cfg = (keypad_config_t){"Heat-up 2 rate","C/s",HEATUP_RATE_MIN,HEATUP_RATE_MAX,s_buf.second_heatup_rate,true,keypad_done,(void*)(intptr_t)id}; break;
    case F_REFLOW_T:    cfg = (keypad_config_t){"Reflow Temperature","C",REFLOW_TEMP_MIN,REFLOW_TEMP_MAX,s_buf.reflow_temp,false,keypad_done,(void*)(intptr_t)id}; break;
```

(The F_SOAK_TIME and F_REFLOW_TIME cases already use `"s"` — no
change.)

- [ ] **Step 4: Update the action-row Y coordinates**

Total occupied height now = 0 + 56 + 6 + 90 + 6 + 56 + 6 + 90 = 310 →
action row at y = 318 (was 348). Lines previously read:

```c
    lv_obj_set_pos(reset, 0, 348);
    ...
    lv_obj_set_pos(prev, 102, 348);
    ...
    lv_obj_set_pos(save, 204, 348);
```

Change all three to y = 318:

```c
    lv_obj_set_pos(reset, 0, 318);
    ...
    lv_obj_set_pos(prev, 102, 318);
    ...
    lv_obj_set_pos(save, 204, 318);
```

- [ ] **Step 5: Verify build**

```bash
~/.platformio/penv/bin/pio run -e esp32dev 2>&1 | tail -5
```

Expected: SUCCESS.

- [ ] **Step 6: Commit**

```bash
git add src/ui/screen_profile.c
git commit -m "Phase 5 review fix 9: screen_profile — four stage cards per spec §7.3 + ASCII units

Restructure from six flat cards to four stage cards: Heat-up 1 and
Heat-up 2 are single-value cards; Soak and Reflow are dual-value cards
(temp + time sub-rows, each independently tappable). Action row shifts
up from y=348 to y=318 to match the new vertical footprint. Also swap
the °C / °C/s keypad unit literals in field_clicked_cb to plain ASCII
C / C/s — closes the gap Task 8 left when it de-glyphed only the
keypad header separator."
```

---

### Task 10: `screen_profile` — add `screen_profile_update` for gating + saved-toast

**Files:**
- Modify: `src/ui/screen_profile.h`
- Modify: `src/ui/screen_profile.c`

The PREVIEW and SAVE buttons must be visually disabled while
`mode != MODE_IDLE` (spec §7.6). A "saved" toast must fire when the
controller bumps `profile_applied_seq`.

- [ ] **Step 1: Add the prototype + extend the header**

Replace `src/ui/screen_profile.h` entirely with:

```c
#pragma once
#include "lvgl.h"
#include "oven_state.h"

void screen_profile_create(lv_obj_t *parent);
void screen_profile_set_initial(const profile_t *p);   /* loaded from NVS at boot */

/* Called every refresh tick from ui_task. Watches snap->mode to
 * enable/disable PREVIEW + SAVE per spec §7.6, and watches
 * snap->profile_applied_seq to flash a "saved" toast when the
 * controller acknowledges a SAVE. */
void screen_profile_update(const oven_state_t *snap);
```

- [ ] **Step 2: Add file-static handles + toast state to `screen_profile.c`**

Near the top of `src/ui/screen_profile.c`, after the existing
`static profile_t s_buf;` and `static lv_obj_t *s_val_labels[6];`
declarations, add:

```c
static lv_obj_t *s_btn_preview;
static lv_obj_t *s_btn_save;
static lv_obj_t *s_saved_label;
static uint16_t  s_last_seen_profile_seq = 0;

static void clear_saved(lv_timer_t *t) {
    lv_obj_add_flag(s_saved_label, LV_OBJ_FLAG_HIDDEN);
    lv_timer_delete(t);
}

static void show_saved(void) {
    lv_obj_remove_flag(s_saved_label, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(clear_saved, 1500, NULL);
}
```

- [ ] **Step 3: Rename local handles `prev` and `save` to point at the file-statics**

In `screen_profile_create`, locate the action-row code. Change:

```c
    lv_obj_t *prev = lv_button_create(parent);
    ...
    lv_obj_t *save = lv_button_create(parent);
```

to:

```c
    s_btn_preview = lv_button_create(parent);
    ...
    s_btn_save = lv_button_create(parent);
```

Update all other references in that block: `prev` → `s_btn_preview`,
`save` → `s_btn_save`. The `reset` button stays a local because it's
always enabled.

- [ ] **Step 4: Create the saved-toast widget at the end of `screen_profile_create`**

Append to `screen_profile_create`:

```c
    /* "Saved" toast — hidden by default. */
    s_saved_label = lv_label_create(parent);
    lv_label_set_text(s_saved_label, LV_SYMBOL_OK " saved");
    lv_obj_set_style_text_color(s_saved_label, THEME_OK, 0);
    lv_obj_align(s_saved_label, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_add_flag(s_saved_label, LV_OBJ_FLAG_HIDDEN);
```

- [ ] **Step 5: Add the `screen_profile_update` implementation**

Append to `src/ui/screen_profile.c`:

```c
void screen_profile_update(const oven_state_t *snap) {
    /* §7.6: PREVIEW + SAVE disabled while mode != MODE_IDLE. */
    if (snap->mode == MODE_IDLE) {
        lv_obj_remove_state(s_btn_preview, LV_STATE_DISABLED);
        lv_obj_remove_state(s_btn_save,    LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_btn_preview, LV_STATE_DISABLED);
        lv_obj_add_state(s_btn_save,    LV_STATE_DISABLED);
    }

    /* §7.6: toast on controller ACK (seq advance). */
    if (snap->profile_applied_seq != s_last_seen_profile_seq) {
        s_last_seen_profile_seq = snap->profile_applied_seq;
        show_saved();
    }
}
```

- [ ] **Step 6: Remove the UI-side `profile_validate` short-circuit in `save_clicked_cb`**

Now that the controller is the sole gatekeeper for both validation and
persistence, the UI's pre-validate is redundant. Either remove it or
keep it as a fast-fail before queueing. The plan keeps it as a fast-fail
to avoid round-tripping clearly-bogus values through the queue, but
documents the new ownership:

In `save_clicked_cb`, the function body is currently:

```c
static void save_clicked_cb(lv_event_t *e) {
    if (!profile_validate(&s_buf)) return;   /* TODO: show error toast */
    cmd_msg_t msg = {.type = CMD_APPLY_PROFILE, .payload.profile = s_buf};
    xQueueSend(g_cmd_queue, &msg, 0);
}
```

Replace it with:

```c
static void save_clicked_cb(lv_event_t *e) {
    (void)e;
    /* Fast-fail before queue send for clearly-invalid values. The
     * controller revalidates and is the sole NVS writer; this is just
     * a UI-side ergonomic. The 'saved' toast fires only when the
     * controller bumps profile_applied_seq (see screen_profile_update). */
    if (!profile_validate(&s_buf)) return;
    cmd_msg_t msg = {.type = CMD_APPLY_PROFILE, .payload.profile = s_buf};
    xQueueSend(g_cmd_queue, &msg, 0);
}
```

- [ ] **Step 7: Verify build**

```bash
~/.platformio/penv/bin/pio run -e esp32dev 2>&1 | tail -5
```

Expected: SUCCESS.

- [ ] **Step 8: Commit**

```bash
git add src/ui/screen_profile.h src/ui/screen_profile.c
git commit -m "Phase 5 review fix 10: screen_profile — mode-gated buttons + saved toast on controller ACK

- New screen_profile_update(snap) called every refresh tick.
- PREVIEW and SAVE buttons add LV_STATE_DISABLED whenever
  mode != MODE_IDLE (spec §7.6).
- 'Saved' toast fires only when control_task bumps
  profile_applied_seq — i.e. after NVS write succeeds — matching
  the spec §7.6 'after controller ACK' contract."
```

---

## Phase G — UI PID screen

### Task 11: `screen_pid` — `screen_pid_update` for toast on `pid_applied_seq` advance

**Files:**
- Modify: `src/ui/screen_pid.h`
- Modify: `src/ui/screen_pid.c`

The toast must fire on the controller's seq bump, not on the keypad
commit. Otherwise we'd show "saved" before the value has actually been
persisted (or — if validation fails — when nothing has been persisted).

- [ ] **Step 1: Add the prototype**

Replace `src/ui/screen_pid.h` entirely with:

```c
#pragma once
#include "lvgl.h"
#include "oven_state.h"
#include "storage/nvs_schema.h"

void screen_pid_create(lv_obj_t *parent);
void screen_pid_set_initial(const pid_gains_t *g);

/* Called every refresh tick from ui_task. Watches
 * snap->pid_applied_seq and triggers the "✓ saved" toast when the
 * controller acknowledges a commit (i.e. after NVS save). */
void screen_pid_update(const oven_state_t *snap);
```

- [ ] **Step 2: Add seq state + the update function in `screen_pid.c`**

In `src/ui/screen_pid.c`, near the top with the other file-statics,
add:

```c
static uint16_t s_last_seen_pid_seq = 0;
```

- [ ] **Step 3: Remove the immediate `show_saved()` call from `keypad_done`**

The current `keypad_done` ends with:

```c
    cmd_msg_t msg = {.type = CMD_APPLY_PID, .payload.pid = s_buf};
    xQueueSend(g_cmd_queue, &msg, 0);
    show_saved();
```

Change to:

```c
    cmd_msg_t msg = {.type = CMD_APPLY_PID, .payload.pid = s_buf};
    xQueueSend(g_cmd_queue, &msg, 0);
    /* Toast fires from screen_pid_update once controller bumps
     * pid_applied_seq — i.e. once NVS save has actually succeeded. */
```

- [ ] **Step 4: Add `screen_pid_update`**

Append to `src/ui/screen_pid.c`:

```c
void screen_pid_update(const oven_state_t *snap) {
    if (snap->pid_applied_seq != s_last_seen_pid_seq) {
        s_last_seen_pid_seq = snap->pid_applied_seq;
        show_saved();
    }
}
```

- [ ] **Step 5: Verify build**

```bash
~/.platformio/penv/bin/pio run -e esp32dev 2>&1 | tail -5
```

Expected: SUCCESS.

- [ ] **Step 6: Commit**

```bash
git add src/ui/screen_pid.h src/ui/screen_pid.c
git commit -m "Phase 5 review fix 11: screen_pid — toast on pid_applied_seq advance

Move the show_saved() call out of keypad_done (fired on enqueue) and
into the new screen_pid_update (fired on controller ACK). Faithful to
spec §7.4 'after the controller acknowledges'."
```

---

## Phase H — `ui_task` wiring

### Task 12: `ui_task` — rename to `ui_init`, UI-owned curve copy buffer, call all `_update` functions

**Files:**
- Modify: `src/ui/ui_task.h`
- Modify: `src/ui/ui_task.c`
- Modify: `src/main.c`

Three changes in one task because they're tightly coupled — the rename
and the new `_update` dispatch both touch the same five lines, and the
curve-copy buffer changes the locking discipline used by all three
`screen_*_update` functions.

- [ ] **Step 1: Rename the public function in the header**

Replace `src/ui/ui_task.h` entirely with:

```c
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "oven_state.h"

/* Initialise the LVGL UI: display + touch + ports + screens. Does NOT
 * spawn a FreeRTOS task — esp_lvgl_port owns its own task; this
 * function just sets it up and returns. Naming retained as ui_init
 * (not ui_task_create) because the implementation does not call
 * xTaskCreate. */
void ui_init(QueueHandle_t cmd_queue, oven_state_t *state);
```

- [ ] **Step 2: Replace `ui_task.c` with the curve-copy + multi-screen refresh implementation**

Replace `src/ui/ui_task.c` entirely with:

```c
#include "ui/ui_task.h"
#include "ui/theme.h"
#include "ui/screen_home.h"
#include "ui/screen_profile.h"
#include "ui/screen_pid.h"
#include "ui/screen_keypad.h"
#include "drivers/display.h"
#include "drivers/touch.h"
#include "storage/nvs_store.h"
#include "control/profile_curve.h"
#include "config.h"
#include "esp_lvgl_port.h"
#include "esp_err.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include <string.h>

QueueHandle_t g_cmd_queue;          /* externed by the screen modules */
static oven_state_t *s_state;

/* UI-owned snapshot + curve copy. The refresh timer holds the state
 * mutex only long enough to memcpy these out of the publisher's
 * buffers; LVGL rendering then runs lock-free against the copy. This
 * keeps the control_task critical section bounded to a deterministic
 * memcpy regardless of how heavy the graph render becomes (spec
 * §3.3 curve-pointer ownership). */
static oven_state_t s_snap;
static float        s_curve_copy[PROFILE_CURVE_MAX_SAMPLES];

static void ui_refresh_timer(lv_timer_t *t) {
    (void)t;

    /* ---- critical section: memcpy snapshot + curve ---- */
    xSemaphoreTake(s_state->mutex, portMAX_DELAY);
    s_snap = *s_state;
    uint16_t n = s_state->curve_len;
    if (n > PROFILE_CURVE_MAX_SAMPLES) n = PROFILE_CURVE_MAX_SAMPLES;
    if (s_state->curve && n) {
        memcpy(s_curve_copy, s_state->curve, n * sizeof(float));
    }
    xSemaphoreGive(s_state->mutex);
    /* ---- end critical section ---- */

    /* Re-anchor the snapshot's curve pointer to the UI-owned copy. */
    s_snap.curve = s_curve_copy;
    s_snap.curve_len = n;

    /* Drive the planned-curve line. cycle_total_ms = end-of-reflow. */
    screen_home_set_curve(s_curve_copy, n,
                          s_snap.phase_end_ms[STAGE_REFLOW]);

    screen_home_update(&s_snap);
    screen_profile_update(&s_snap);
    screen_pid_update(&s_snap);
}

void ui_init(QueueHandle_t cmd_queue, oven_state_t *state) {
    g_cmd_queue = cmd_queue;
    s_state = state;

    display_handles_t d = display_init();
    esp_lcd_touch_handle_t tp = touch_init();

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = d.io, .panel_handle = d.panel,
        .buffer_size = 320 * 48, .double_buffer = true,
        .hres = 320, .vres = 480,
        .rotation = {.swap_xy = false, .mirror_x = true, .mirror_y = false},
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    lvgl_port_touch_cfg_t touch_cfg = {.disp = disp, .handle = tp};
    lvgl_port_add_touch(&touch_cfg);

    /* Build tabview */
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();
    theme_apply_to_screen(scr);
    lv_obj_t *tv = lv_tabview_create(scr);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_BOTTOM);
    lv_tabview_set_tab_bar_size(tv, 36);
    lv_obj_set_size(tv, 320, 480);

    lv_obj_t *tab_home    = lv_tabview_add_tab(tv, "HOME");
    lv_obj_t *tab_profile = lv_tabview_add_tab(tv, "PROFILE");
    lv_obj_t *tab_pid     = lv_tabview_add_tab(tv, "PID");

    screen_home_create(tab_home);
    screen_profile_create(tab_profile);
    screen_pid_create(tab_pid);

    /* Load persisted values into the editors. */
    profile_t p; nvs_store_load_profile(&p);
    pid_gains_t g; nvs_store_load_pid(&g);
    screen_profile_set_initial(&p);
    screen_pid_set_initial(&g);

    lv_timer_create(ui_refresh_timer, UI_REFRESH_MS, NULL);
    lvgl_port_unlock();
}
```

Three behaviour changes:
- The publisher mutex is held only across `*s_state` struct copy plus
  `memcpy(curve, n*sizeof(float))`. Render runs fully decoupled.
- The snapshot's `curve` pointer is redirected to `s_curve_copy` after
  unlock — so screens can dereference it freely.
- `screen_profile_update` and `screen_pid_update` are now called every
  tick alongside `screen_home_update`.

- [ ] **Step 3: Update `main.c` to call the renamed function**

In `src/main.c`, locate:

```c
#include "ui/ui_task.h"
...
    ui_task_create(s_cmd_queue, &s_state);
```

Change the call to:

```c
    ui_init(s_cmd_queue, &s_state);
```

(The header include line is unchanged — same header name.)

- [ ] **Step 4: Verify build + native tests**

```bash
~/.platformio/penv/bin/pio run -e esp32dev 2>&1 | tail -10
~/.platformio/penv/bin/pio test -e native 2>&1 | tail -5
```

Expected: esp32dev SUCCESS (RAM up ~6 KB for the curve copy); native
47/47 still green.

- [ ] **Step 5: Commit**

```bash
git add src/ui/ui_task.h src/ui/ui_task.c src/main.c
git commit -m "Phase 5 review fix 12: ui_task — rename to ui_init, UI-owned curve copy, dispatch _update on every screen

- ui_task_create -> ui_init: the function never xTaskCreate'd anything;
  the name was misleading.
- Refresh timer now memcpy's both the oven_state_t snapshot AND the
  active curve buffer into UI-owned static storage under the publisher
  mutex. Mutex hold time is now bounded by deterministic memcpy +
  struct copy regardless of how heavy the LVGL render becomes
  (resolves the spec §3.3 curve-pointer tripwire).
- Refresh timer now calls screen_home_set_curve (Task 7),
  screen_home_update, screen_profile_update (Task 10), and
  screen_pid_update (Task 11)."
```

---

## Phase I — Spec + plan refresh

### Task 13: Patch the spec to match the new IPC contract + persistence ownership

**Files:**
- Modify: `docs/superpowers/specs/2026-05-15-reflow-oven-mvp-design.md`

Spec amendments tied to findings the review surfaced — including
**three places where the spec's existing prose contradicts the new
controller-side persistence model** and must be rewritten, not just
appended to. We patch the spec in place rather than versioning — this
is still pre-MVP, and the spec history is captured in git.

- [ ] **Step 1: Amend §3.3 to include the new IPC fields**

Find the `oven_state_t` struct definition in the spec's §3.3 section
and replace its body with the post-Task-1 schema. **The spec's struct
sketch deliberately omits the `mutex` handle** (prose above says
"mutex-protected"; the FreeRTOS handle is an implementation detail) —
preserve that convention. The fields the spec struct should list are
exactly:

```c
struct oven_state_t {
    float       temp_c;
    float       setpoint_c;
    stage_t     stage;
    uint32_t    elapsed_ms;
    uint8_t     duty_percent;
    uint8_t     fault_flags;
    oven_mode_t mode;
    const float *curve;                  /* see ownership note below */
    uint16_t    curve_len;
    uint32_t    phase_end_ms[STAGE_COUNT_];
    uint16_t    profile_applied_seq;
    uint16_t    pid_applied_seq;
};
```

Add a paragraph immediately below explaining the new fields:

```markdown
`phase_end_ms[]` is a per-tick copy of the active `profile_curve_t`'s
end-of-stage marks. It exists so the UI can render the §7.2 stage chip
("Stage SOAK · 42 / 100 s") without dereferencing the curve pointer,
which has the buffer-swap caveat called out above.

`profile_applied_seq` and `pid_applied_seq` are monotonically
increasing counters bumped by `control_task` **after** the corresponding
NVS save succeeds (see §7.4 / §7.6 / §8). The UI watches them as a
one-way ACK channel — when the counter advances past a UI-remembered
baseline, the screens fire the appropriate "✓ saved" toast.
```

- [ ] **Step 2: Amend §5.1 step 5 — tick-budget claim for NVS writes**

§5.1 step 5 currently asserts that the cmd-queue drain finishes in the
low-millisecond range. Moving `nvs_store_save_*` into the controller
(Task 3) breaks that claim: a worst-case flash erase/program is in the
~50–500 ms range. Find the step-5 paragraph in §5.1 and append:

```markdown
Note (post-review): `CMD_APPLY_PROFILE` and `CMD_APPLY_PID` now invoke
`nvs_store_save_*` synchronously inside the drain, so this step can
take up to ~500 ms in the worst case (flash erase + program). That's
well inside the 2 s task-WDT bound (§6.1 item 5), but it does mean
single-tick determinism cannot be assumed when an APPLY message is
queued. Mitigations: `APPLY_PROFILE` is rejected outside `MODE_IDLE`
(§5.1 step 5 already states this), and `APPLY_PID` only persists when
`mode == MODE_IDLE` — the gains are applied immediately in
`MODE_RUNNING` but the NVS write defers until the operator next
commits while idle.
```

- [ ] **Step 3: Amend §7.4 (PID screen)**

Change the bullet that currently reads "Auto-saves on every successful
keypad commit (per-field, on OK); shows a brief '✓ saved' toast after
the controller acknowledges." to:

```markdown
Per-field commit on OK. In `MODE_IDLE`, `control_task` re-validates,
applies the gains, calls `nvs_store_save_pid()`, then bumps
`oven_state_t.pid_applied_seq` — the UI fires its "✓ saved" toast on
that seq advance. In `MODE_RUNNING` ("live tune"), gains are applied
immediately but the NVS write is deferred until the next idle commit
to keep the control tick deterministic (see §5.1 step 5); the toast
will not fire on live-tune commits.
```

- [ ] **Step 4: Amend §7.6 (mapping table) for the SAVE + PID rows**

Replace the SAVE row in the §7.6 control-mapping table:

| Old `Profile SAVE` mapping | New mapping |
|---|---|
| `cmd_queue.send(CMD_APPLY_PROFILE)`, then `storage.save_profile()` after controller ACK | `cmd_queue.send(CMD_APPLY_PROFILE)`. `control_task` re-validates, applies, calls `nvs_store_save_profile()`, then bumps `profile_applied_seq` — that bump is the UI's ACK channel; the SAVE toast fires off it. |

Replace the PID OK row similarly:

| Old PID OK mapping | New mapping |
|---|---|
| `cmd_queue.send(CMD_APPLY_PID)`, then `storage.save_pid()` after controller ACK | `cmd_queue.send(CMD_APPLY_PID)`. `control_task` re-validates, applies, and (only in `MODE_IDLE`) calls `nvs_store_save_pid()` + bumps `pid_applied_seq`. UI fires "✓ saved" off the seq advance. |

**Also rewrite the §7.6 paragraph immediately above this table** that
currently reads:

> Saving to NVS *after* the controller confirms acceptance keeps NVS
> in sync with the controller's live state — the UI never persists
> values the controller hasn't accepted.

…to:

```markdown
NVS persistence is performed by `control_task` (sole writer) after
its own `*_validate` check succeeds. The UI never writes NVS; it
enqueues `CMD_APPLY_*` and consumes the seq-counter ACK in the
published snapshot. This collapses validate-apply-persist into one
critical section and removes the need for a UI←controller ACK queue.
```

- [ ] **Step 5: Amend §8 (NVS) — single writer and rewrite the stale "after controller ACK" line**

§8 currently contains a sentence saying "Write only on explicit user
action (Profile SAVE; PID per-field commit, after controller ACK —
see §7.6)." That wording presupposes UI-side writes; with the new
model the UI is no longer a writer at all. **Rewrite that sentence** —
don't just add a paragraph next to it — to:

```markdown
Writes happen only on explicit user action: Profile SAVE, or PID
per-field commit. The writer is always `control_task` (sole-writer
ownership; see §3.3 + §7.6); the UI signals intent via `CMD_APPLY_*`
and consumes the post-save ACK from `oven_state_t.*_applied_seq`. PID
commits performed during `MODE_RUNNING` apply the gains immediately
but defer the NVS write to the next `MODE_IDLE` commit, so flash I/O
never lengthens a control tick mid-run.
```

- [ ] **Step 6: Amend §7.3 to clarify the sub-row layout**

§7.3 already reads "Single column, four stage cards stacked: Heat-up 1,
Soak (Temp + Time), Heat-up 2, Reflow (Temp + Time)." — i.e. the
four-card claim is already in the spec. **No "six fields" wording
exists to replace.** What the spec is missing is a clarification on
sub-row touch behaviour. Append this sentence at the end of the §7.3
layout paragraph:

```markdown
Soak and Reflow are dual-value cards: the Temperature and Time
sub-rows are independently tappable, each opening the keypad popup
for its own field.
```

- [ ] **Step 7: Commit the spec amendments**

```bash
git add docs/superpowers/specs/2026-05-15-reflow-oven-mvp-design.md
git commit -m "Phase 5 review fix 13: spec amendments — IPC ACK channel, controller-side NVS writes, tick-budget caveat, dual-row sub-cards

- §3.3: add phase_end_ms[] + *_applied_seq fields to oven_state_t.
  (Preserve the spec's no-mutex-in-struct convention.)
- §5.1 step 5: acknowledge that NVS writes inside the drain can stall
  the tick up to ~500 ms; still inside the 2 s WDT bound; APPLY_PID
  defers the write outside MODE_IDLE.
- §7.4: PID toast fires on pid_applied_seq advance; live-tune writes
  defer to next idle commit.
- §7.6: Profile SAVE + PID OK mappings updated; rewrite the
  contradicting 'UI persists after controller ACK' prose above the
  table.
- §8: rewrite the 'UI writes on user action' line — UI is no longer
  a writer; control_task is.
- §7.3: append a sentence clarifying that Soak/Reflow sub-rows are
  independently tappable. (The four-card claim was already in the
  spec.)"
```

---

### Task 14: Update Phase 5 of the implementation plan to match

**Files:**
- Modify: `docs/superpowers/plans/2026-05-16-reflow-oven-mvp-implementation.md`

The Phase 5 task code blocks in that plan show the pre-review
implementations. We update them in place so future executors don't
re-introduce the bugs we just fixed. **Tasks affected: 21, 22, 24,
25, 26, 27, 28, 29.** Tasks 24 (theme) and 25 (keypad) need patches
too — they hold code the keypad / ASCII fixes touched.

- [ ] **Step 1: Patch Task 21 (`oven_state.h`)**

Replace the Task 21 struct definition (which currently lists only
`mutex`, `temp_c`, `setpoint_c`, `stage`, `elapsed_ms`,
`duty_percent`, `fault_flags`, `mode`, `curve`, `curve_len`) with the
post-Task-1 version that adds `phase_end_ms[STAGE_COUNT_]`,
`profile_applied_seq`, and `pid_applied_seq`. Also update the locking
comment block to mention the seq counters.

- [ ] **Step 2: Patch Task 22 (`control/control_task`)**

Replace Task 22 Step 4's `apply_non_sm_command` snippet so that:
- `CMD_APPLY_PROFILE` adds the `nvs_store_save_profile` call +
  `profile_applied_seq` bump.
- `CMD_APPLY_PID` adds the `pid_validate` precondition, the
  `MODE_IDLE`-gated `nvs_store_save_pid` call, and the
  `pid_applied_seq` bump.
- The existing comment block above the function ("Acceptance rules per
  spec §5.1 step 5…") is preserved verbatim; the new behaviour
  amendments are added to it, not in place of it.

Replace Task 22 Step 1's `publish()` snippet so that the snapshot
write includes `memcpy(s_pub->phase_end_ms, active_curve->phase_end_ms,
sizeof(s_pub->phase_end_ms));`.

- [ ] **Step 3: Patch Task 24 (`ui/theme`)** — no code changes

Add a paragraph to Task 24's prose noting that the theme now also
holds `THEME_MODAL_BG` and `THEME_BTN_NEUTRAL` (already present after
the original-plan refinement pass, but the task body should call them
out explicitly so an executor doesn't drop them when re-running the
task on a fresh checkout).

- [ ] **Step 4: Patch Task 25 (`ui/screen_keypad`)**

Replace Task 25 Step 2's `screen_keypad_open` body so it:
- Begins with `if (s_modal) screen_keypad_close();` (single-instance
  guard).
- Uses `char hdrbuf[128];` not `char hdrbuf[64];`.
- Uses the ASCII separators: `"%s - %s - %g to %g"` rather than
  `"%s · %s · %g–%g"`.

This propagates Task 8's fixes back into the source-of-truth plan so
a re-runner doesn't re-introduce I-C1 / M3.

- [ ] **Step 5: Patch Task 26 (`ui/screen_home`)**

Replace the Task 26 Step 2 code block with the post-Task-6 +
post-Task-7 content (cached mode dispatch, `stage_name` +
`phase_end_ms` chip, `fault_text`, ASCII glyphs, graph). The Task 26
file list already includes `Create: src/ui/screen_home.h` — extend
that line so the header carries the new `screen_home_set_curve`
prototype too:

```
- Create: src/ui/screen_home.h  (declares screen_home_create,
                                 screen_home_update,
                                 screen_home_set_curve)
```

- [ ] **Step 6: Patch Task 27 (`ui/screen_profile`)**

Replace the Task 27 Step 2 code block with the post-Task-9 +
post-Task-10 content (four stage cards + `screen_profile_update` +
ASCII unit literals). Also update the Task 27 `Create:` line for the
header to mention the new `screen_profile_update` prototype:

```
- Create: src/ui/screen_profile.h  (declares screen_profile_create,
                                    screen_profile_set_initial,
                                    screen_profile_update)
```

Add a note to Task 27's prose: "The UI sends `CMD_APPLY_PROFILE`
only; `control_task` validates + persists + bumps
`profile_applied_seq` — see updated spec §7.6."

- [ ] **Step 7: Patch Task 28 (`ui/screen_pid`)**

Replace the Task 28 Step 2 code block with the post-Task-11 content
(`screen_pid_update`, no immediate `show_saved` in `keypad_done`).
Update the Task 28 header `Create:` line for the new
`screen_pid_update` prototype:

```
- Create: src/ui/screen_pid.h  (declares screen_pid_create,
                                screen_pid_set_initial,
                                screen_pid_update)
```

Add a note: "PID commits during `MODE_RUNNING` apply gains
immediately but defer the NVS write to the next idle commit — see
spec §5.1 step 5 + §7.4."

- [ ] **Step 8: Patch Task 29 (`ui/ui_task`)**

Replace the Task 29 Step 2 code block with the post-Task-12 version
(rename `ui_init`, curve copy, multi-screen update). Update the
file-list entries:
- `Create: src/ui/ui_task.h  (declares ui_init)`
- `Modify: src/main.c  (calls ui_init, not ui_task_create)`

…and Task 29 Step 3's `main.c` snippet to call `ui_init(s_cmd_queue,
&s_state);`.

- [ ] **Step 9: Append a "Phase 5 review refinements (2026-05-17)" section**

After Phase 5's existing "Phase 5 implementation refinements
(2026-05-17)" subsection, append the following block verbatim
(replace `<this-plan>` with the actual filename
`2026-05-17-phase-5-review-fixes.md`):

```markdown
### Phase 5 review-pass refinements (2026-05-17, evening)

Driven by a parallel code + spec review of the merged Phase 5 commits.
The full task-by-task fix plan lives at
`docs/superpowers/plans/<this-plan>`. Every bullet below is folded
into the corresponding Phase 5 task above; this list is the audit
trail.

- IPC: `oven_state_t` gains `phase_end_ms[STAGE_COUNT_]` and two
  `*_applied_seq` ACK counters. Resolves the two spec bugs surfaced
  by the spec-compliance review.
- Controller: `apply_non_sm_command` now performs `nvs_store_save_*`
  + seq bump after validate+apply. PID save is gated to `MODE_IDLE`
  to keep the control tick deterministic during live-tune.
- Helpers: `stage_name(stage_t)` and `fault_text(uint8_t)` in
  `reflow_sm.{h,c}` with Unity tests for every enum value.
- Home: cached-mode dispatch (no more strcmp-on-label), spec-faithful
  stage chip "Stage SOAK - 42 / 100 s", fault_text for FAULT line,
  initial colour fix, real graph (planned curve, live trace, FAULT
  dim, axis labels).
- Keypad: single-instance guard, 128-byte header buffer, ASCII-only
  glyphs.
- Profile: four stage cards (Soak + Reflow dual-row), ASCII unit
  literals, `screen_profile_update` for PREVIEW/SAVE gating + saved
  toast on `profile_applied_seq` advance.
- PID: `screen_pid_update` for saved toast on `pid_applied_seq`
  advance; immediate `show_saved` removed.
- ui_task: renamed `ui_task_create` → `ui_init`; UI-owned curve copy
  buffer (memcpy under mutex, render lock-free); calls all
  `screen_*_update` per tick.
- Spec: §3.3 (IPC fields), §5.1 step 5 (tick-budget caveat for NVS
  writes), §7.3 (sub-row clarification), §7.4 / §7.6 / §8 rewritten
  to reflect controller-side persistence.
- Tunable constants: `GRAPH_LIVE_RING_CAP` and `UI_REFRESH_MS` moved
  to `include/config.h` per the project's single-source rule.
```

- [ ] **Step 10: Commit**

```bash
git add docs/superpowers/plans/2026-05-16-reflow-oven-mvp-implementation.md
git commit -m "Phase 5 review fix 14: fold review-driven refinements into the original implementation plan

Updates Tasks 21, 22, 24, 25, 26, 27, 28, 29 of the implementation
plan to match the post-review code (incl. propagating the keypad
fixes back into Task 25 so re-runners don't re-introduce the bugs),
and appends a 'Phase 5 review-pass refinements (2026-05-17, evening)'
audit-trail section."
```

---

## Phase J — Verification

### Task 15: Final build, host tests, footprint sanity check

**Files:** none modified.

- [ ] **Step 1: Clean native test run**

```bash
~/.platformio/penv/bin/pio test -e native 2>&1 | tail -10
```

Expected: `47 test cases: 47 succeeded`.

- [ ] **Step 2: Clean esp32dev build run**

```bash
~/.platformio/penv/bin/pio run -e esp32dev 2>&1 | tail -15
```

Expected: SUCCESS. Footprint budget:

| Resource | Pre-plan | Post-plan (target) | Allowance |
|---|---|---|---|
| RAM | 28.4 % | ≤ 35.5 % | Plan adds ~22.8 KB static (12.0 KB `s_plan_pts` + 4.8 KB `s_live_pts` + 6.0 KB `s_curve_copy`); on a 320 KB DRAM region that's +7.1 percentage points. |
| Flash | 63.0 % | ≤ 65.5 % | Plan adds ~2 KB of code (helpers + graph render). |

If RAM lands above 36 % or Flash above 66 %, investigate before
claiming success.

- [ ] **Step 3: Verify clean git state**

```bash
git status
git log --oneline 2a70a4b..HEAD
```

Expected: clean working tree. Commits since `2a70a4b`:

- 6 Phase-5 implementation commits (already on branch before this
  plan started).
- 1 plan-creation commit (`1db382a`).
- 14 task commits (Tasks 1–14; Task 5 is one commit covering helpers
  + tests, Task 13 is one commit covering all spec amendments, Task
  14 is one commit covering all impl-plan patches).
- 1 empty milestone-marker commit (Task 15 Step 4).

= **~22 commits total** since `2a70a4b`. The exact count may shift by
±1 if the plan was edited during execution (each plan-edit commit
adds one); the salient invariant is "all 14 fix-task commits are
present and verified."

- [ ] **Step 4: Final empty-commit milestone marker**

```bash
git commit --allow-empty -m "Phase 5 review-fix pass complete; ready for bench (Tasks 30 + 31 of impl plan)"
```

---

## Self-review notes

**Coverage of review findings:**

| Finding (review) | Resolved by |
|---|---|
| Critical: NVS save never called | Tasks 1 + 3 (controller-side write, seq counters) |
| Code review #1: keypad re-open leak | Task 8 |
| Code review #2: label-strcmp brittle dispatch | Task 6 (cached mode) |
| Code review #3: stage chip integer code | Tasks 1 + 5 + 6 |
| Code review #4: uint8_t %u / %02x cast | Task 6 |
| Code review #5: NVS load defaults-on-miss contract | Task 4 |
| Code review minor #6 (keypad hdrbuf) | Task 8 |
| Code review minor #7 (ui_task_create name) | Task 12 |
| Code review minor #8 (orange-flash on boot) | Task 6 |
| Code review minor #9 (graph_canvas dead) | Task 7 |
| Spec review Important #2 (stage chip name+timing) | Tasks 1 + 5 + 6 |
| Spec review Important #3 (FAULT bitmask) | Tasks 5 + 6 |
| Spec review Important #4 (graph missing) | Task 7 |
| Spec review Important #5 (curve pointer past unlock) | Task 12 |
| Spec review Important #6 (PREVIEW not disabled) | Task 10 |
| Spec review Important #7 (PID toast before ACK) | Tasks 3 + 11 |
| Spec review Important #8 (keypad single-instance) | Task 8 |
| Spec review Minor #9 (six vs four cards) | Task 9 |
| Spec review Minor #10 (· / – / ° glyph risk) | Tasks 6 + 8 + 9 (Step 3: °C / °C/s in `field_clicked_cb`) |
| Spec bug: no stage timing in IPC | Tasks 1 + 13 (spec amend) |
| Spec bug: no ACK channel | Tasks 1 + 3 + 13 |
| Spec bug: §7.3 layout ambiguity | Tasks 9 + 13 |

**Findings from the plan-review pass (folded into v2):**

| Finding (plan reviewer) | Resolved by |
|---|---|
| Code: NVS save during MODE_RUNNING stalls control loop | Task 3 Step 2 (gates `nvs_store_save_pid` to `MODE_IDLE`) + Task 13 Step 2 (spec §5.1 amendment) |
| Code: RAM budget claim too low | Task 7 Step 6 + Task 15 Step 2 (recomputed to ~35.5 %) |
| Code: tunable constants inlined (MEMORY.md rule) | Task 7 Step 1a (`GRAPH_LIVE_RING_CAP`, `UI_REFRESH_MS` moved to `include/config.h`); `OVER_TEMP_LIMIT_C` referenced directly instead of duplicated as `GRAPH_TEMP_MAX_C`. |
| Spec + code: `°` glyph survives in `screen_profile.c` | Task 9 Step 3 |
| Code: X-axis labels misaligned 4 px | Task 7 Step 3 (`LV_ALIGN_BOTTOM_RIGHT, -4, -2`) |
| Code: dead `s_plan_n` variable | Task 7 Step 1b + Step 4 |
| Spec: `SemaphoreHandle_t mutex` injected into spec struct | Task 13 Step 1 (mutex omitted) |
| Spec: §7.6 line 397 + §8 line 421 contradict new model | Task 13 Steps 4 + 5 (rewrite, not append) |
| Spec: §7.3 already says "four stage cards" | Task 13 Step 6 (clarify sub-row tappability) |
| Spec: §5.1 step 5 tick budget claim broken by NVS write | Task 13 Step 2 |
| Plan: Task 14 didn't propagate keypad fixes back to Task 25 | Task 14 Step 4 |
| Plan: header files not mentioned in original-plan patches | Task 14 Steps 5, 6, 7, 8 |
| Plan: Task 14 Step 7 vague "list every fix" | Task 14 Step 9 (verbatim text block) |
| Plan: Task 15 commit count claim wrong | Task 15 Step 3 (~22 commits, breakdown given) |

Every finding has a task. No placeholders. Type / function-name
consistency:

- `stage_name(stage_t)` declared in Task 5, used in Tasks 6.
- `fault_text(uint8_t)` declared in Task 5, used in Task 6.
- `phase_end_ms[STAGE_COUNT_]` added to `oven_state_t` in Task 1, used
  in Tasks 6, 7, 12.
- `profile_applied_seq` / `pid_applied_seq` added in Task 1, written in
  Task 3, read in Tasks 10, 11.
- `screen_home_set_curve` declared in Task 7, called in Task 12.
- `screen_profile_update` declared in Task 10, called in Task 12.
- `screen_pid_update` declared in Task 11, called in Task 12.
- `ui_init` renamed in Task 12, call site updated in same task.

**Build / test verification:** every code-modifying task runs the
appropriate `pio run` or `pio test` step before committing.

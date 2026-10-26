/* Shared IPC types between control_task and ui_task. See spec §3.3. */
#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "control/reflow_sm.h"
#include "control/profile_curve.h"
#include "storage/nvs_schema.h"

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
    float       cj_temp_c;   /* MAX31855 cold junction (electronics temp); NAN until
                              * the first successful SPI read, then retains the last
                              * value across transport failures (like temp_c). Valid
                              * even while a TC fault is latched. */
    float       setpoint_c;
    stage_t     stage;
    uint32_t    elapsed_ms;
    uint8_t     duty_percent;
    uint8_t     fault_flags;
    oven_mode_t mode;
    const float *curve;                 /* see locking note above */
    uint16_t    curve_len;
    uint32_t    phase_end_ms[STAGE_COUNT_];  /* copy of s_curve->phase_end_ms (control_task.c) */
    uint16_t    profile_applied_seq;    /* bumped after nvs_store_save_profile */
    uint16_t    pid_applied_seq;        /* bumped after nvs_store_save_pid */
    uint32_t    run_id;             /* bumped on each run start (step test + reflow); §3.1 */
    /* Step-test sub-status (valid when mode == MODE_STEP_TEST or just ended). */
    bool        step_active;
    uint8_t     step_duty;
    float       step_ceiling_c;
    uint32_t    step_max_ms;
    uint8_t     step_end_reason;    /* steptest_end_reason_t, copied as raw */
    /* Drying sub-status (valid when mode == MODE_DRYING or just ended).
     * Scalars only — no pointer is published here, so the `curve` locking
     * caveat above does not extend to these. */
    bool        dry_active;
    uint8_t     dry_stage;            /* dry_stage_t, copied as raw */
    float       dry_target_c;
    uint32_t    dry_hold_ms;
    uint32_t    dry_hold_elapsed_ms;
    uint8_t     dry_end_reason;       /* dry_end_reason_t, copied as raw */
    /* Pending PID suggestion from POST /suggest-gains (touchscreen confirms). */
    bool        pending_pid_valid;
    pid_gains_t pending_pid;
    /* Active PID/ff gains + the ambient reference captured at run start —
     * published for GET /profile so run archives are self-describing. */
    pid_gains_t pid_active_gains;
    float       pid_ambient_ref_c;
} oven_state_t;

/* UI -> control commands. */
typedef enum {
    CMD_START,
    CMD_STOP,
    CMD_ACK_FAULT,
    CMD_APPLY_PROFILE,
    CMD_APPLY_PID,
    CMD_CLEAR_PENDING,  /* user confirms a pending-validation update */
    CMD_BEGIN_OTA,      /* OTA server asks to reserve the (idle) oven for an update */
    CMD_END_OTA,        /* OTA server releases the reservation (upload failed) */
    CMD_START_STEP_TEST,   /* touchscreen-only: begin an open-loop step test */
    CMD_START_DRY,         /* touchscreen-only: begin a filament dry */
    CMD_SUGGEST_PID,       /* browser: hold a pending PID suggestion */
    CMD_DISMISS_PID,       /* touchscreen: discard the pending suggestion */
} cmd_type_t;

typedef struct {
    uint8_t  duty;          /* % */
    float    ceiling_c;
    uint32_t max_ms;
} steptest_params_t;

typedef struct {
    float    target_c;
    uint32_t hold_ms;
} dry_params_t;

typedef struct {
    cmd_type_t type;
    union {
        profile_t         profile;   /* APPLY_PROFILE */
        pid_gains_t       pid;       /* APPLY_PID, SUGGEST_PID */
        steptest_params_t steptest;  /* START_STEP_TEST */
        dry_params_t      dry;       /* START_DRY */
    } payload;
} cmd_msg_t;

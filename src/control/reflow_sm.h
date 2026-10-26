/* Reflow state machine. Pure logic — caller passes elapsed_ms and current temp;
 * the SM returns a recommended action and stage. See spec §5.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "control/profile_curve.h"

typedef enum {
    MODE_IDLE,
    MODE_RUNNING,
    MODE_FAULT,
    MODE_PENDING_VALIDATION,
    MODE_STEP_TEST,
    MODE_DRYING,
} oven_mode_t;

/* Firmware-validation gate, tracked independently of oven_mode_t. A fault
 * forces MODE_FAULT and clearing it would otherwise drop straight back to
 * MODE_IDLE; keeping the gate in its own field means a fault that trips
 * while an OTA image is unvalidated cannot silently re-enable START
 * (findings #2/#3). The PENDING vs FAILED distinction also decides whether
 * the user is even allowed to confirm (finding #4). */
typedef enum {
    SM_VALIDATION_NONE = 0,   /* no pending OTA validation; START allowed */
    SM_VALIDATION_PENDING,    /* self-tests passed; awaiting user confirm  */
    SM_VALIDATION_FAILED,     /* self-tests failed; rollback on next reboot */
} sm_validation_t;

/* Why a step test ended. STEP_END_NONE = still running or never run. */
typedef enum {
    STEP_END_NONE = 0,
    STEP_END_CEILING,       /* temp reached the configured ceiling (normal) */
    STEP_END_MAX_DURATION,  /* hit the max-duration backstop */
    STEP_END_MANUAL_STOP,   /* CMD_STOP from touchscreen or browser */
    STEP_END_FAULT,         /* a safety gate tripped */
} steptest_end_reason_t;

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

typedef enum {
    FAULT_NONE       = 0,
    FAULT_TC_OPEN    = 1 << 0,
    FAULT_TC_SCG     = 1 << 1,
    FAULT_TC_SCV     = 1 << 2,
    FAULT_TC_STALE   = 1 << 3,
    FAULT_OVER_TEMP  = 1 << 4,
    /* Cold-junction (electronics bay) over-temperature; see CJ_LIMIT_C.
     * Appended, never renumbered — /telemetry publishes "fault" as the raw
     * fault_flags bitmask (ota_server.c), which external loggers decode. */
    FAULT_ELECTRONICS_HOT = 1 << 5,
    /* Highest defined flag, for the fault_text coverage loop in
     * test_reflow_sm_helpers. Not a flag: never set it, never store it.
     * Keep it equal to the last entry above when appending. */
    FAULT_FLAG_LAST_ = FAULT_ELECTRONICS_HOT,
} fault_flag_t;

typedef struct {
    oven_mode_t     mode;
    stage_t         stage;
    uint32_t        elapsed_ms;
    uint32_t        cool_down_start_ms;
    uint8_t         fault_flags;
    sm_validation_t validation;   /* OTA validation gate; see sm_validation_t */
    bool            ota_reserved; /* an OTA upload reserved the oven; START rejected (finding #13) */
    /* Step test (open-loop PID identification). Valid only in MODE_STEP_TEST. */
    uint8_t               step_duty;        /* fixed duty %, clamped to STEP_TEST_DUTY_* */
    float                 step_ceiling_c;   /* abort temperature */
    uint32_t              step_max_ms;      /* abort elapsed time */
    steptest_end_reason_t step_end_reason;  /* set when the test ends */
    /* Filament drying. Valid only in MODE_DRYING (or just ended). */
    float            dry_target_c;
    float            dry_start_temp_c;    /* oven temp at START; ramp origin */
    uint32_t         dry_hold_ms;         /* requested hold duration */
    uint32_t         dry_hold_elapsed_ms; /* accrues in DRY_STAGE_HOLD only */
    dry_stage_t      dry_stage;
    dry_end_reason_t dry_end_reason;
} sm_state_t;

typedef enum {
    SM_EVENT_NONE,
    SM_EVENT_CMD_START,
    SM_EVENT_CMD_STOP,
    SM_EVENT_CMD_ACK_FAULT,
    SM_EVENT_CMD_CLEAR_PENDING,   /* user confirms a pending-validation update */
    SM_EVENT_CMD_START_STEP_TEST,
    SM_EVENT_CMD_START_DRY,
} sm_event_t;

void sm_init(sm_state_t *s);
void sm_trip_fault(sm_state_t *s, uint8_t flag);

bool sm_step(sm_state_t *s,
             sm_event_t event,
             float temp_c,
             const profile_curve_t *curve);

/* Begin a step test. Returns true if accepted (was idle, not reserved, not
 * gated). Clamps duty to [STEP_TEST_DUTY_MIN, STEP_TEST_DUTY_MAX]. On accept,
 * sets mode = MODE_STEP_TEST, stage = STAGE_NONE, elapsed = 0,
 * step_end_reason = STEP_END_NONE, and stores the parameters. */
bool sm_start_step_test(sm_state_t *s, uint8_t duty, float ceiling_c, uint32_t max_ms);

/* Advance a step test by one CONTROL_TICK_MS. Returns true if the SSR should
 * be driven at step_duty this tick; false if the test has ended (mode set back
 * to MODE_IDLE and step_end_reason populated). The CALLER must run all safety
 * gates BEFORE this — a fault path uses sm_trip_fault, which records
 * STEP_END_FAULT itself when tripping out of a step test. Pure logic; the
 * caller owns the SSR. */
bool sm_step_test_tick(sm_state_t *s, float temp_c);

/* Begin a dry. Clamps target_c to [DRY_TEMP_MIN_C, DRY_TEMP_MAX_C] and
 * hold_ms to [DRY_HOLD_MIN_MS, DRY_DURATION_MAX_MS] FIRST, then returns true
 * if accepted: idle, not OTA-reserved, not gated, and temp_c <= (clamped
 * target) + DRY_START_MARGIN_C. The start gate uses the clamped target, so it
 * tracks the run that will actually happen — sm_start_dry(s, 1.0f, …, 25.0f)
 * is accepted, because the target it stores is DRY_TEMP_MIN_C, not 1.0.
 * A non-finite target_c or temp_c is refused outright: NaN would slip through
 * every clamp and gate below unchanged. */
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

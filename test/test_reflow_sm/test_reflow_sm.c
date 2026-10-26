#include <unity.h>
#include <math.h>
#include <string.h>
#include "control/reflow_sm.h"
#include "control/profile_curve.h"
#include "config.h"

static sm_state_t sm;
static profile_t prof;
static profile_curve_t curve;

void setUp(void) {
    sm_init(&sm);
    profile_defaults(&prof);
    profile_curve_generate(&prof, PROFILE_START_DEFAULT_C, &curve);
}
void tearDown(void) {}

void test_init_is_idle_no_fault(void) {
    TEST_ASSERT_EQUAL(MODE_IDLE, sm.mode);
    TEST_ASSERT_EQUAL(STAGE_NONE, sm.stage);
    TEST_ASSERT_EQUAL(0, sm.fault_flags);
}

void test_start_transitions_to_heat_up_1(void) {
    sm_step(&sm, SM_EVENT_CMD_START, 25.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_RUNNING, sm.mode);
    TEST_ASSERT_EQUAL(STAGE_HEAT_UP_1, sm.stage);
}

void test_stop_returns_to_idle(void) {
    sm_step(&sm, SM_EVENT_CMD_START, 25.0f, &curve);
    sm_step(&sm, SM_EVENT_CMD_STOP, 100.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_IDLE, sm.mode);
    TEST_ASSERT_EQUAL(STAGE_NONE, sm.stage);
}

void test_stages_advance_at_phase_end_ms(void) {
    sm_step(&sm, SM_EVENT_CMD_START, 25.0f, &curve);
    /* Step until the SM advances out of HEAT_UP_1. Using stage as the loop
     * condition avoids overshooting into HEAT_UP_2 on profiles with a short
     * SOAK phase. */
    while (sm.stage == STAGE_HEAT_UP_1) {
        sm_step(&sm, SM_EVENT_NONE, 100.0f, &curve);
    }
    TEST_ASSERT_EQUAL(STAGE_SOAK, sm.stage);
}

void test_fault_is_sticky_until_ack(void) {
    sm_trip_fault(&sm, FAULT_TC_OPEN);
    TEST_ASSERT_EQUAL(MODE_FAULT, sm.mode);
    sm_step(&sm, SM_EVENT_CMD_START, 25.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_FAULT, sm.mode);
    sm_step(&sm, SM_EVENT_CMD_ACK_FAULT, 25.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_IDLE, sm.mode);
    TEST_ASSERT_EQUAL(0, sm.fault_flags);
}

void test_cool_down_exits_on_temp_threshold(void) {
    sm.mode = MODE_RUNNING;
    sm.stage = STAGE_COOL_DOWN;
    sm.cool_down_start_ms = 0;
    sm.elapsed_ms = 0;
    sm_step(&sm, SM_EVENT_NONE, COOL_DOWN_THRESHOLD_C - 1.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_IDLE, sm.mode);
}

void test_cool_down_exits_on_timeout(void) {
    sm.mode = MODE_RUNNING;
    sm.stage = STAGE_COOL_DOWN;
    sm.cool_down_start_ms = 0;
    sm.elapsed_ms = COOL_DOWN_TIMEOUT_MS + CONTROL_TICK_MS;
    sm_step(&sm, SM_EVENT_NONE, 200.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_IDLE, sm.mode);
}

void test_pid_runs_only_in_active_stages(void) {
    sm_step(&sm, SM_EVENT_CMD_START, 25.0f, &curve);
    TEST_ASSERT_TRUE(sm_step(&sm, SM_EVENT_NONE, 30.0f, &curve));

    sm.stage = STAGE_COOL_DOWN;
    sm.cool_down_start_ms = sm.elapsed_ms;
    TEST_ASSERT_FALSE(sm_step(&sm, SM_EVENT_NONE, 200.0f, &curve));
}

/* ── OTA validation gate (findings #2/#3/#4) ─────────────────────────────── */

void test_start_blocked_while_validation_pending(void) {
    sm.mode = MODE_PENDING_VALIDATION;
    sm.validation = SM_VALIDATION_PENDING;
    sm_step(&sm, SM_EVENT_CMD_START, 25.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_PENDING_VALIDATION, sm.mode);  /* START rejected */
}

void test_start_blocked_while_validation_failed(void) {
    /* Failed self-tests must also block START, not silently stay IDLE (#2). */
    sm.mode = MODE_PENDING_VALIDATION;
    sm.validation = SM_VALIDATION_FAILED;
    sm_step(&sm, SM_EVENT_CMD_START, 25.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_PENDING_VALIDATION, sm.mode);
}

void test_clear_pending_confirms_when_passed(void) {
    sm.mode = MODE_PENDING_VALIDATION;
    sm.validation = SM_VALIDATION_PENDING;
    sm_step(&sm, SM_EVENT_CMD_CLEAR_PENDING, 25.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_IDLE, sm.mode);
    TEST_ASSERT_EQUAL(SM_VALIDATION_NONE, sm.validation);
}

void test_clear_pending_ignored_when_failed(void) {
    /* A broken image cannot be confirmed; it must roll back on reboot (#4). */
    sm.mode = MODE_PENDING_VALIDATION;
    sm.validation = SM_VALIDATION_FAILED;
    sm_step(&sm, SM_EVENT_CMD_CLEAR_PENDING, 25.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_PENDING_VALIDATION, sm.mode);
    TEST_ASSERT_EQUAL(SM_VALIDATION_FAILED, sm.validation);
}

void test_fault_during_pending_returns_to_pending_after_ack(void) {
    /* Gate must survive a transient fault: ack lands back in the blocked
     * state, not IDLE, so rollback safety is not lost (#3). */
    sm.mode = MODE_PENDING_VALIDATION;
    sm.validation = SM_VALIDATION_PENDING;
    sm_trip_fault(&sm, FAULT_TC_OPEN);
    TEST_ASSERT_EQUAL(MODE_FAULT, sm.mode);
    sm_step(&sm, SM_EVENT_CMD_ACK_FAULT, 25.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_PENDING_VALIDATION, sm.mode);
    TEST_ASSERT_EQUAL(SM_VALIDATION_PENDING, sm.validation);
}

void test_start_blocked_while_ota_reserved(void) {
    /* An OTA upload reserved the oven; START must be rejected (finding #13). */
    sm.ota_reserved = true;
    sm_step(&sm, SM_EVENT_CMD_START, 25.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_IDLE, sm.mode);
    /* Reservation released → START works again. */
    sm.ota_reserved = false;
    sm_step(&sm, SM_EVENT_CMD_START, 25.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_RUNNING, sm.mode);
}

void test_start_blocked_while_too_hot(void) {
    /* A run may only start from a near-ambient oven so every run has a
     * comparable soak preheat and the ff hold model isn't seeded hot. */
    sm_step(&sm, SM_EVENT_CMD_START, RUN_START_MAX_TEMP_C + 1.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_IDLE, sm.mode);       /* refused while hot */
    /* At the threshold it is allowed (boundary is inclusive). */
    sm_step(&sm, SM_EVENT_CMD_START, RUN_START_MAX_TEMP_C, &curve);
    TEST_ASSERT_EQUAL(MODE_RUNNING, sm.mode);
}

/* ── Step test: entry guard ──────────────────────────────────────────────── */

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

/* ── Step test: per-tick advance + aborts ────────────────────────────────── */

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

/* The closed-loop PID must NOT engage during a step test: sm_step returns false
 * in MODE_STEP_TEST. This keeps the step open-loop AND is what stops
 * control_task's reflow run-start edge (pid_active false->true bumps run_id and
 * resets the PID) from firing mid-step — otherwise a step test would double-bump
 * run_id and drive the closed-loop PID over the open-loop step. A START event
 * arriving mid-step is ignored, not promoted to a reflow run. */
void test_step_test_does_not_engage_pid(void) {
    sm_start_step_test(&sm, 50, 180.0f, 600000u);
    TEST_ASSERT_FALSE(sm_step(&sm, SM_EVENT_NONE, 100.0f, &curve));      /* PID off, mid-step */
    TEST_ASSERT_EQUAL(MODE_STEP_TEST, sm.mode);
    TEST_ASSERT_FALSE(sm_step(&sm, SM_EVENT_CMD_START, 120.0f, &curve)); /* START ignored, still off */
    TEST_ASSERT_EQUAL(MODE_STEP_TEST, sm.mode);
}


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

    /* Exactly on the ceiling is accepted: the gate is `>`, and 45.0f + 5.0f
     * is exactly 50.0f. Re-init first — the accepted start above left
     * MODE_DRYING, which the idle guard would reject for the wrong reason. */
    sm_init(&s);
    TEST_ASSERT_TRUE(sm_start_dry(&s, 45.0f, 3600000u, 50.0f));
}

void test_dry_start_rejects_non_finite_inputs(void) {
    /* NaN compares false against both clamps AND the start gate, so without an
     * explicit guard a NaN target reaches dry_target_c intact. The caller's
     * over-temperature guard is "temp > dry_target_c + margin", which is false
     * for every temperature once the target is NaN — the run would heat with
     * only the global 260 C limit left as a backstop. */
    sm_state_t s; sm_init(&s);
    TEST_ASSERT_FALSE(sm_start_dry(&s, NAN, 3600000u, 25.0f));
    TEST_ASSERT_EQUAL_INT(MODE_IDLE, s.mode);
    TEST_ASSERT_FALSE(sm_start_dry(&s, INFINITY, 3600000u, 25.0f));
    TEST_ASSERT_EQUAL_INT(MODE_IDLE, s.mode);
    TEST_ASSERT_FALSE(sm_start_dry(&s, -INFINITY, 3600000u, 25.0f));
    TEST_ASSERT_EQUAL_INT(MODE_IDLE, s.mode);
    /* A NaN sensor read slips past the start gate the same way. */
    TEST_ASSERT_FALSE(sm_start_dry(&s, 45.0f, 3600000u, NAN));
    TEST_ASSERT_EQUAL_INT(MODE_IDLE, s.mode);
    /* The guard must not reject a legitimate start. */
    TEST_ASSERT_TRUE(sm_start_dry(&s, 45.0f, 3600000u, 25.0f));
    TEST_ASSERT_EQUAL_INT(MODE_DRYING, s.mode);
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

    /* Exactly on the band edge enters HOLD: the check is `>=`, and
     * 80.0f - 2.0f is exactly 78.0f. */
    sm_init(&s);
    sm_start_dry(&s, 80.0f, 3600000u, 25.0f);
    dry_ticks(&s, 78.0f, 1);
    TEST_ASSERT_EQUAL_INT(DRY_STAGE_HOLD, s.dry_stage);

    /* A second target, so the band edge is pinned to dry_target_c rather
     * than to the 78.0 that every case above happens to share. */
    sm_init(&s);
    sm_start_dry(&s, 50.0f, 3600000u, 25.0f);
    dry_ticks(&s, 47.9f, 1);
    TEST_ASSERT_EQUAL_INT(DRY_STAGE_HEAT_UP, s.dry_stage);
    dry_ticks(&s, 48.1f, 1);
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
    /* A multiple of the shortest accepted hold, not the minimum itself: the
     * completion check must be pinned to dry_hold_ms, not to DRY_HOLD_MIN_MS
     * (clamping is covered by test_dry_start_clamps_target_and_duration). */
    const uint32_t hold      = 4u * DRY_HOLD_MIN_MS;
    const int      ticks     = (int)(hold / CONTROL_TICK_MS);
    const int      min_ticks = (int)(DRY_HOLD_MIN_MS / CONTROL_TICK_MS);
    sm_start_dry(&s, 80.0f, hold, 25.0f);
    TEST_ASSERT_EQUAL_UINT32(hold, s.dry_hold_ms);
    /* 79.0 is inside the band, so the hold timer runs from the first tick. */
    dry_ticks(&s, 79.0f, min_ticks);
    TEST_ASSERT_EQUAL_INT(MODE_DRYING, s.mode);   /* the minimum is not the end */
    dry_ticks(&s, 79.0f, ticks - 1 - min_ticks);
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
    /* The fault exit must leave the stage as clean as every other exit does:
     * oven_state_t publishes dry_stage to the UI and telemetry. */
    TEST_ASSERT_EQUAL_INT(DRY_STAGE_NONE, s.dry_stage);
}

void test_dry_tick_reports_setpoint(void) {
    sm_state_t s; sm_init(&s);
    sm_start_dry(&s, 80.0f, 3600000u, 25.0f);
    /* Tick far enough that the ramp clears the tolerance — one tick moves the
     * setpoint by 0.0125 C, so a 0.05 tolerance around it would assert nothing
     * but sp == dry_start_temp_c and any clock would pass. 30 C stays below
     * the 78 C band and 100.25 s below DRY_HEATUP_TIMEOUT_MS, so the run is
     * still ramping in HEAT_UP. */
    dry_ticks(&s, 30.0f, 400);          /* 100 s at 0.05 C/s = +5.0 C */
    float sp = 0.0f;
    sm_dry_tick(&s, 30.0f, &sp);        /* 100.25 s */
    TEST_ASSERT_EQUAL_INT(DRY_STAGE_HEAT_UP, s.dry_stage);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 30.0125f, sp);
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

/* A newly appended fault bit is only usable if the ack path clears it: an
 * electronics trip during a multi-hour dry must leave an oven the operator can
 * restart once the bay has cooled, not one latched in MODE_FAULT. */
void test_electronics_fault_acked_leaves_oven_startable(void) {
    sm_start_dry(&sm, 80.0f, 3600000u, 25.0f);
    sm_trip_fault(&sm, FAULT_ELECTRONICS_HOT);
    TEST_ASSERT_EQUAL(MODE_FAULT, sm.mode);
    TEST_ASSERT_TRUE(sm.fault_flags & FAULT_ELECTRONICS_HOT);

    sm_step(&sm, SM_EVENT_CMD_ACK_FAULT, 25.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_IDLE, sm.mode);
    TEST_ASSERT_EQUAL(0, sm.fault_flags & FAULT_ELECTRONICS_HOT);

    /* Startable, not merely un-faulted. */
    sm_step(&sm, SM_EVENT_CMD_START, 25.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_RUNNING, sm.mode);
    TEST_ASSERT_EQUAL(STAGE_HEAT_UP_1, sm.stage);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_is_idle_no_fault);
    RUN_TEST(test_start_blocked_while_ota_reserved);
    RUN_TEST(test_start_blocked_while_too_hot);
    RUN_TEST(test_start_transitions_to_heat_up_1);
    RUN_TEST(test_stop_returns_to_idle);
    RUN_TEST(test_stages_advance_at_phase_end_ms);
    RUN_TEST(test_fault_is_sticky_until_ack);
    RUN_TEST(test_cool_down_exits_on_temp_threshold);
    RUN_TEST(test_cool_down_exits_on_timeout);
    RUN_TEST(test_pid_runs_only_in_active_stages);
    RUN_TEST(test_start_blocked_while_validation_pending);
    RUN_TEST(test_start_blocked_while_validation_failed);
    RUN_TEST(test_clear_pending_confirms_when_passed);
    RUN_TEST(test_clear_pending_ignored_when_failed);
    RUN_TEST(test_fault_during_pending_returns_to_pending_after_ack);
    RUN_TEST(test_step_test_starts_from_idle);
    RUN_TEST(test_step_test_rejected_when_running);
    RUN_TEST(test_step_test_rejected_when_ota_reserved);
    RUN_TEST(test_step_test_clamps_duty);
    RUN_TEST(test_step_test_rejected_when_fault);
    RUN_TEST(test_step_test_rejected_when_pending_validation);
    RUN_TEST(test_step_tick_drives_below_ceiling);
    RUN_TEST(test_step_tick_aborts_on_ceiling);
    RUN_TEST(test_step_tick_aborts_on_max_duration);
    RUN_TEST(test_step_stop_event_ends_with_manual_reason);
    RUN_TEST(test_step_fault_ends_with_fault_reason);
    RUN_TEST(test_step_test_does_not_engage_pid);
    RUN_TEST(test_dry_start_requires_idle);
    RUN_TEST(test_dry_start_rejected_when_ota_reserved);
    RUN_TEST(test_dry_start_rejected_when_validation_pending);
    RUN_TEST(test_dry_start_rejected_when_too_hot);
    RUN_TEST(test_dry_start_rejects_non_finite_inputs);
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
    RUN_TEST(test_electronics_fault_acked_leaves_oven_startable);
    return UNITY_END();
}

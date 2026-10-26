#include <unity.h>
#include "audio/audio_events.h"

static audio_event_t out[4];

void setUp(void)    {}
void tearDown(void) {}

static audio_state_snapshot_t snap(oven_mode_t m, stage_t s, uint8_t f) {
    audio_state_snapshot_t r = { .mode = m, .stage = s, .fault_flags = f,
                                 .dry_end_reason = DRY_END_NONE };
    return r;
}

static audio_state_snapshot_t snap_dry(oven_mode_t m, dry_end_reason_t r) {
    audio_state_snapshot_t o = { .mode = m, .stage = STAGE_NONE,
                                 .fault_flags = 0, .dry_end_reason = r };
    return o;
}

void test_no_change_emits_nothing(void) {
    audio_state_snapshot_t s = snap(MODE_IDLE, STAGE_NONE, 0);
    TEST_ASSERT_EQUAL_size_t(0, audio_detect_events(s, s, out));
}

void test_start_emits_stage_transition(void) {
    audio_state_snapshot_t p = snap(MODE_IDLE,    STAGE_NONE,      0);
    audio_state_snapshot_t c = snap(MODE_RUNNING, STAGE_HEAT_UP_1, 0);
    TEST_ASSERT_EQUAL_size_t(1, audio_detect_events(p, c, out));
    TEST_ASSERT_EQUAL(AUDIO_EVT_STAGE_TRANSITION, out[0]);
}

void test_mid_run_stage_advance_emits_stage_transition(void) {
    audio_state_snapshot_t p = snap(MODE_RUNNING, STAGE_SOAK,      0);
    audio_state_snapshot_t c = snap(MODE_RUNNING, STAGE_HEAT_UP_2, 0);
    TEST_ASSERT_EQUAL_size_t(1, audio_detect_events(p, c, out));
    TEST_ASSERT_EQUAL(AUDIO_EVT_STAGE_TRANSITION, out[0]);
}

void test_cool_down_to_idle_emits_run_complete_not_stage(void) {
    audio_state_snapshot_t p = snap(MODE_RUNNING, STAGE_COOL_DOWN, 0);
    audio_state_snapshot_t c = snap(MODE_IDLE,    STAGE_NONE,      0);
    TEST_ASSERT_EQUAL_size_t(1, audio_detect_events(p, c, out));
    TEST_ASSERT_EQUAL(AUDIO_EVT_RUN_COMPLETE, out[0]);
}

void test_fault_rising_edge_emits_fault(void) {
    audio_state_snapshot_t p = snap(MODE_RUNNING, STAGE_SOAK,  0);
    audio_state_snapshot_t c = snap(MODE_FAULT,   STAGE_SOAK,  FAULT_TC_OPEN);
    TEST_ASSERT_EQUAL_size_t(1, audio_detect_events(p, c, out));
    TEST_ASSERT_EQUAL(AUDIO_EVT_FAULT, out[0]);
}

void test_fault_ack_emits_fault_ack(void) {
    audio_state_snapshot_t p = snap(MODE_FAULT, STAGE_SOAK, FAULT_TC_OPEN);
    audio_state_snapshot_t c = snap(MODE_IDLE,  STAGE_NONE, 0);
    TEST_ASSERT_EQUAL_size_t(1, audio_detect_events(p, c, out));
    TEST_ASSERT_EQUAL(AUDIO_EVT_FAULT_ACK, out[0]);
}

void test_simultaneous_fault_and_stage_change_suppresses_stage(void) {
    /* Over-temp trip exactly at SOAK->HEAT_UP_2 boundary. */
    audio_state_snapshot_t p = snap(MODE_RUNNING, STAGE_SOAK,      0);
    audio_state_snapshot_t c = snap(MODE_FAULT,   STAGE_HEAT_UP_2, FAULT_OVER_TEMP);
    size_t n = audio_detect_events(p, c, out);
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL(AUDIO_EVT_FAULT, out[0]);
}

void test_transition_into_stage_none_emits_nothing(void) {
    /* Defensive: STAGE_NONE is not in the trigger set. */
    audio_state_snapshot_t p = snap(MODE_RUNNING, STAGE_HEAT_UP_1, 0);
    audio_state_snapshot_t c = snap(MODE_RUNNING, STAGE_NONE,      0);
    TEST_ASSERT_EQUAL_size_t(0, audio_detect_events(p, c, out));
}

void test_reflow_to_cool_down_emits_stage_transition(void) {
    /* Entering COOL_DOWN is the door-open cue and must pip. It fires minutes
     * before RUN_COMPLETE (which waits for temp < COOL_DOWN_THRESHOLD_C), so
     * the two are not redundant — RUN_COMPLETE alone leaves the actionable
     * moment silent. */
    audio_state_snapshot_t p = snap(MODE_RUNNING, STAGE_REFLOW,    0);
    audio_state_snapshot_t c = snap(MODE_RUNNING, STAGE_COOL_DOWN, 0);
    TEST_ASSERT_EQUAL_size_t(1, audio_detect_events(p, c, out));
    TEST_ASSERT_EQUAL(AUDIO_EVT_STAGE_TRANSITION, out[0]);
}

void test_completed_dry_emits_run_complete(void) {
    /* A dry runs 6-16 h; the user is asleep or out for nearly all of it, so
     * the completion chime is the feature's only notification. */
    audio_state_snapshot_t p = snap_dry(MODE_DRYING, DRY_END_NONE);
    audio_state_snapshot_t c = snap_dry(MODE_IDLE,   DRY_END_COMPLETE);
    TEST_ASSERT_EQUAL_size_t(1, audio_detect_events(p, c, out));
    TEST_ASSERT_EQUAL(AUDIO_EVT_RUN_COMPLETE, out[0]);
}

void test_manually_stopped_dry_is_silent(void) {
    /* Every dry exit lands in MODE_IDLE, so the mode transition alone cannot
     * carry this: without the dry_end_reason gate a manual stop would chime
     * "your filament is ready" at a user who just cancelled. */
    audio_state_snapshot_t p = snap_dry(MODE_DRYING, DRY_END_NONE);
    audio_state_snapshot_t c = snap_dry(MODE_IDLE,   DRY_END_MANUAL_STOP);
    TEST_ASSERT_EQUAL_size_t(0, audio_detect_events(p, c, out));
}

void test_dry_heatup_timeout_is_silent(void) {
    audio_state_snapshot_t p = snap_dry(MODE_DRYING, DRY_END_NONE);
    audio_state_snapshot_t c = snap_dry(MODE_IDLE,   DRY_END_HEATUP_TIMEOUT);
    TEST_ASSERT_EQUAL_size_t(0, audio_detect_events(p, c, out));
}

void test_dry_still_running_emits_nothing(void) {
    /* dry_end_reason is DRY_END_NONE for the whole run; nothing must fire
     * before the mode actually leaves MODE_DRYING. */
    audio_state_snapshot_t p = snap_dry(MODE_DRYING, DRY_END_NONE);
    TEST_ASSERT_EQUAL_size_t(0, audio_detect_events(p, p, out));
}

void test_dry_fault_emits_fault_not_run_complete(void) {
    /* DRYING -> FAULT: the fault pip, and no completion chime. */
    audio_state_snapshot_t p = snap_dry(MODE_DRYING, DRY_END_NONE);
    audio_state_snapshot_t c = { .mode = MODE_FAULT, .stage = STAGE_NONE,
                                 .fault_flags = FAULT_ELECTRONICS_HOT,
                                 .dry_end_reason = DRY_END_FAULT };
    TEST_ASSERT_EQUAL_size_t(1, audio_detect_events(p, c, out));
    TEST_ASSERT_EQUAL(AUDIO_EVT_FAULT, out[0]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_no_change_emits_nothing);
    RUN_TEST(test_start_emits_stage_transition);
    RUN_TEST(test_mid_run_stage_advance_emits_stage_transition);
    RUN_TEST(test_cool_down_to_idle_emits_run_complete_not_stage);
    RUN_TEST(test_fault_rising_edge_emits_fault);
    RUN_TEST(test_fault_ack_emits_fault_ack);
    RUN_TEST(test_simultaneous_fault_and_stage_change_suppresses_stage);
    RUN_TEST(test_transition_into_stage_none_emits_nothing);
    RUN_TEST(test_reflow_to_cool_down_emits_stage_transition);
    RUN_TEST(test_completed_dry_emits_run_complete);
    RUN_TEST(test_manually_stopped_dry_is_silent);
    RUN_TEST(test_dry_heatup_timeout_is_silent);
    RUN_TEST(test_dry_still_running_emits_nothing);
    RUN_TEST(test_dry_fault_emits_fault_not_run_complete);
    return UNITY_END();
}

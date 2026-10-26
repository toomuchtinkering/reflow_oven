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

/* The ramp baseline is start_temp_c, not a constant: same elapsed time, a
 * 15 C warmer start must give a 15 C higher setpoint. Every other ramping
 * test uses (25, 80), which an implementation hardcoding either endpoint
 * would satisfy. */
void test_ramp_is_offset_from_start_temp(void) {
    /* Same elapsed time, 15 C warmer start -> 15 C higher setpoint. */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 45.0f, dry_setpoint(40.0f, 90.0f, 100000u));
}

/* The clamp ceiling is target_c, not a constant. */
void test_clamp_tracks_target(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, dry_setpoint(40.0f, 50.0f, 5000000u));
}

/* Boundary between the ramping and the already-at-target paths. */
void test_start_equal_to_target_returns_target(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 45.0f, dry_setpoint(45.0f, 45.0f, 0u));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 45.0f, dry_setpoint(45.0f, 45.0f, 10000u));
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
    /* Strictly above, not just equal: no feedforward into a descent. */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, dry_setpoint_rate(48.0f, 45.0f, 0u));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_at_start_temp);
    RUN_TEST(test_ramps_at_configured_rate);
    RUN_TEST(test_clamps_at_target);
    RUN_TEST(test_ramp_is_offset_from_start_temp);
    RUN_TEST(test_clamp_tracks_target);
    RUN_TEST(test_start_equal_to_target_returns_target);
    RUN_TEST(test_start_above_target_returns_target);
    RUN_TEST(test_rate_is_ramp_rate_then_zero);
    RUN_TEST(test_rate_zero_when_starting_at_or_above_target);
    return UNITY_END();
}

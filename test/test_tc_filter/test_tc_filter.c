/* test/test_tc_filter/test_tc_filter.c
 * Median-of-3 filter on successive thermocouple readings. Motivated by the
 * 2026-06-11 test run: single-sample dips of up to -9 °C (e.g. 228.5 →
 * 219.5 → 227.5 at t≈425 s) slammed the PID to 100 % duty. A 3-sample
 * median removes any lone spike while tracking a ramp with one-sample lag.
 */
#include <unity.h>
#include "drivers/tc_filter.h"

void setUp(void)    {}
void tearDown(void) {}

void test_first_sample_passes_through(void) {
    tc_filter_t f;
    tc_filter_reset(&f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 150.0f, tc_filter_push(&f, 150.0f));
}

void test_single_negative_spike_is_rejected(void) {
    /* The exact sequence from the 2026-06-11 run at t≈425 s. */
    tc_filter_t f;
    tc_filter_reset(&f);
    tc_filter_push(&f, 228.5f);
    /* Window seeded with 228.5 → median(228.5, 228.5, 219.5) = 228.5 */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 228.5f, tc_filter_push(&f, 219.5f));
    /* median(228.5, 219.5, 227.5) = 227.5 — spike never surfaced */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 227.5f, tc_filter_push(&f, 227.5f));
}

void test_single_positive_spike_is_rejected(void) {
    tc_filter_t f;
    tc_filter_reset(&f);
    tc_filter_push(&f, 100.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, tc_filter_push(&f, 180.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 101.0f, tc_filter_push(&f, 101.0f));
}

void test_ramp_tracks_with_one_sample_lag(void) {
    tc_filter_t f;
    tc_filter_reset(&f);
    tc_filter_push(&f, 100.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, tc_filter_push(&f, 101.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 101.0f, tc_filter_push(&f, 102.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 102.0f, tc_filter_push(&f, 103.0f));
}

void test_sustained_step_passes_on_second_sample(void) {
    /* Two agreeing samples beat one old one — a real fast rise (door shut,
     * element on) is delayed by exactly one tick, never suppressed. */
    tc_filter_t f;
    tc_filter_reset(&f);
    tc_filter_push(&f, 100.0f);
    tc_filter_push(&f, 200.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 200.0f, tc_filter_push(&f, 200.0f));
}

void test_reset_discards_history(void) {
    tc_filter_t f;
    tc_filter_reset(&f);
    tc_filter_push(&f, 250.0f);
    tc_filter_push(&f, 251.0f);
    tc_filter_reset(&f);
    /* Post-reset the window must reseed — no blend with pre-reset values. */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 50.0f, tc_filter_push(&f, 50.0f));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_first_sample_passes_through);
    RUN_TEST(test_single_negative_spike_is_rejected);
    RUN_TEST(test_single_positive_spike_is_rejected);
    RUN_TEST(test_ramp_tracks_with_one_sample_lag);
    RUN_TEST(test_sustained_step_passes_on_second_sample);
    RUN_TEST(test_reset_discards_history);
    UNITY_END();
    return 0;
}

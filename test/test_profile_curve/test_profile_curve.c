#include <unity.h>
#include <string.h>
#include "control/profile_curve.h"
#include "config.h"

static profile_t prof;
static profile_curve_t curve;

void setUp(void)    { profile_defaults(&prof); memset(&curve, 0, sizeof(curve)); }
void tearDown(void) {}

void test_defaults_validate(void) {
    TEST_ASSERT_TRUE(profile_validate(&prof));
}

void test_defaults_match_config_h(void) {
    TEST_ASSERT_EQUAL_FLOAT(FIRST_HEATUP_DEFAULT, prof.first_heatup_rate);
    TEST_ASSERT_EQUAL_UINT16(SOAK_TEMP_DEFAULT, prof.soak_temp);
    TEST_ASSERT_EQUAL_UINT16(SOAK_TIME_DEFAULT, prof.soak_time);
    TEST_ASSERT_EQUAL_FLOAT(SECOND_HEATUP_DEFAULT, prof.second_heatup_rate);
    TEST_ASSERT_EQUAL_UINT16(REFLOW_TEMP_DEFAULT, prof.reflow_temp);
    TEST_ASSERT_EQUAL_UINT16(REFLOW_TIME_DEFAULT, prof.reflow_time);
}

void test_reflow_below_soak_rejected(void) {
    prof.reflow_temp = 100;
    prof.soak_temp = 150;
    TEST_ASSERT_FALSE(profile_validate(&prof));
}

void test_zero_rate_rejected(void) {
    prof.first_heatup_rate = 0.0f;
    TEST_ASSERT_FALSE(profile_validate(&prof));
}

void test_curve_generates_for_valid_profile(void) {
    TEST_ASSERT_TRUE(profile_curve_generate(&prof, PROFILE_START_DEFAULT_C, &curve));
    TEST_ASSERT_GREATER_THAN(0, curve.length);
}

void test_phase_end_ms_monotonic(void) {
    profile_curve_generate(&prof, PROFILE_START_DEFAULT_C, &curve);
    TEST_ASSERT_LESS_THAN(curve.phase_end_ms[STAGE_SOAK],       curve.phase_end_ms[STAGE_HEAT_UP_1]);
    TEST_ASSERT_LESS_THAN(curve.phase_end_ms[STAGE_HEAT_UP_2],  curve.phase_end_ms[STAGE_SOAK]);
    TEST_ASSERT_LESS_THAN(curve.phase_end_ms[STAGE_REFLOW],     curve.phase_end_ms[STAGE_HEAT_UP_2]);
    TEST_ASSERT_LESS_THAN(curve.phase_end_ms[STAGE_COOL_DOWN],  curve.phase_end_ms[STAGE_REFLOW]);
}

void test_cool_down_descends_to_cool_threshold(void) {
    profile_curve_generate(&prof, PROFILE_START_DEFAULT_C, &curve);
    /* The very last planned sample is the cool-down endpoint, which
     * append_ramp_down forces to COOL_DOWN_THRESHOLD_C — the temperature where
     * the run completes. Allow 1°C slack for the linear-step quantisation. */
    float last = curve.samples[curve.length - 1];
    TEST_ASSERT_FLOAT_WITHIN(1.0f, COOL_DOWN_THRESHOLD_C, last);
}

void test_setpoint_at_zero_tracks_start_temp(void) {
    /* Heat-up 1 begins at the passed start temperature (in-range), so the
     * setpoint at t=0 equals it — this is what removes the standing error. */
    profile_curve_generate(&prof, 30.0f, &curve);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 30.0f, profile_curve_setpoint(&curve, 0));
}

void test_start_temp_floored(void) {
    /* A start below the floor clamps up to PROFILE_START_MIN_C so a cold room
     * can't lengthen ramp 1 past the sample buffer. */
    profile_curve_generate(&prof, 5.0f, &curve);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, PROFILE_START_MIN_C, curve.samples[0]);
}

void test_start_temp_capped_at_soak(void) {
    /* A start at/above soak_temp can't invert ramp 1; it caps at soak_temp and
     * heat-up 1 collapses toward a single sample (still <= soak boundary). */
    prof.soak_temp = 120;
    profile_curve_generate(&prof, 200.0f, &curve);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 120.0f, curve.samples[0]);
    TEST_ASSERT_TRUE(curve.phase_end_ms[STAGE_HEAT_UP_1]
                     <= curve.phase_end_ms[STAGE_SOAK]);
}

void test_warmer_start_shortens_heatup1(void) {
    /* A warmer start has less distance to the soak target, so ramp 1 is shorter. */
    profile_curve_t cold, warm;
    profile_curve_generate(&prof, 25.0f, &cold);
    profile_curve_generate(&prof, 45.0f, &warm);
    TEST_ASSERT_TRUE(warm.phase_end_ms[STAGE_HEAT_UP_1]
                     < cold.phase_end_ms[STAGE_HEAT_UP_1]);
}

void test_setpoint_oob_clamps_to_last(void) {
    profile_curve_generate(&prof, PROFILE_START_DEFAULT_C, &curve);
    float last = curve.samples[curve.length - 1];
    float oob  = profile_curve_setpoint(&curve, 99999999u);
    TEST_ASSERT_EQUAL_FLOAT(last, oob);
}

void test_cool_down_rate_below_min_rejected(void) {
    prof.cool_down_rate = COOL_DOWN_RATE_MIN - 0.01f;
    TEST_ASSERT_FALSE(profile_validate(&prof));
}

void test_cool_down_rate_above_max_rejected(void) {
    prof.cool_down_rate = COOL_DOWN_RATE_MAX + 1.0f;
    TEST_ASSERT_FALSE(profile_validate(&prof));
}

void test_reflow_temp_above_over_temp_headroom_rejected(void) {
    /* Spec §9 cross-bound: reflow target must leave >=10 C headroom below
     * OVER_TEMP_LIMIT_C. A reflow_temp greater than that boundary must be
     * rejected even if it passes the per-field range check. */
    prof.reflow_temp = (uint16_t)(OVER_TEMP_LIMIT_C - 10.0f + 1.0f);
    TEST_ASSERT_FALSE(profile_validate(&prof));
}

/* stage_rate[] is filled by generate: signed rates per stage (spec §2). */
void test_stage_rates_filled(void) {
    TEST_ASSERT_TRUE(profile_curve_generate(&prof, PROFILE_START_DEFAULT_C, &curve));
    TEST_ASSERT_EQUAL_FLOAT(prof.first_heatup_rate,  curve.stage_rate[STAGE_HEAT_UP_1]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f,                    curve.stage_rate[STAGE_SOAK]);
    TEST_ASSERT_EQUAL_FLOAT(prof.second_heatup_rate, curve.stage_rate[STAGE_HEAT_UP_2]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f,                    curve.stage_rate[STAGE_REFLOW]);
    TEST_ASSERT_EQUAL_FLOAT(-prof.cool_down_rate,    curve.stage_rate[STAGE_COOL_DOWN]);
}

/* Rate lookup resolves the active stage by phase_end_ms; past end -> 0. */
void test_curve_rate_lookup(void) {
    TEST_ASSERT_TRUE(profile_curve_generate(&prof, PROFILE_START_DEFAULT_C, &curve));
    /* mid heat-up 1 */
    TEST_ASSERT_EQUAL_FLOAT(prof.first_heatup_rate,
        profile_curve_rate(&curve, curve.phase_end_ms[STAGE_HEAT_UP_1] / 2));
    /* first ms of soak (boundary: end of heat-up-1 belongs to next stage) */
    TEST_ASSERT_EQUAL_FLOAT(0.0f,
        profile_curve_rate(&curve, curve.phase_end_ms[STAGE_HEAT_UP_1]));
    /* mid heat-up 2 */
    TEST_ASSERT_EQUAL_FLOAT(prof.second_heatup_rate,
        profile_curve_rate(&curve, (curve.phase_end_ms[STAGE_SOAK]
                                  + curve.phase_end_ms[STAGE_HEAT_UP_2]) / 2));
    /* past everything -> 0 */
    TEST_ASSERT_EQUAL_FLOAT(0.0f,
        profile_curve_rate(&curve, curve.phase_end_ms[STAGE_COOL_DOWN] + 60000u));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_validate);
    RUN_TEST(test_defaults_match_config_h);
    RUN_TEST(test_reflow_below_soak_rejected);
    RUN_TEST(test_zero_rate_rejected);
    RUN_TEST(test_curve_generates_for_valid_profile);
    RUN_TEST(test_phase_end_ms_monotonic);
    RUN_TEST(test_cool_down_descends_to_cool_threshold);
    RUN_TEST(test_setpoint_at_zero_tracks_start_temp);
    RUN_TEST(test_start_temp_floored);
    RUN_TEST(test_start_temp_capped_at_soak);
    RUN_TEST(test_warmer_start_shortens_heatup1);
    RUN_TEST(test_setpoint_oob_clamps_to_last);
    RUN_TEST(test_cool_down_rate_below_min_rejected);
    RUN_TEST(test_cool_down_rate_above_max_rejected);
    RUN_TEST(test_reflow_temp_above_over_temp_headroom_rejected);
    RUN_TEST(test_stage_rates_filled);
    RUN_TEST(test_curve_rate_lookup);
    return UNITY_END();
}

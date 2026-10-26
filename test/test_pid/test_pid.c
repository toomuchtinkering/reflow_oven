#include <unity.h>
#include "control/pid.h"
#include "config.h"

static pid_state_t pid;

void setUp(void)    { pid_init(&pid, &(pid_gains_t){10.0f, 1.0f, 0.0f, 0.0f, 0.0f}); pid_reset(&pid, 25.0f); }
void tearDown(void) {}

/* After reset: integrator zero, prev_temp set to current. */
void test_reset_clears_integrator_and_sets_prev_temp(void) {
    pid_reset(&pid, 42.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, pid.integrator);
    TEST_ASSERT_EQUAL_FLOAT(42.0f, pid.prev_temp);
}

/* First step after reset produces zero d_term (prev_temp == temp). */
void test_first_step_has_zero_derivative(void) {
    /* Setpoint 50, temp 50 → error 0, p_term 0, i_term 0, d_term 0 → 0 output. */
    float out = pid_step(&pid, 50.0f, 0.0f, 50.0f, 0.25f, PID_OUTPUT_MAX);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out);
}

/* Saturation high + positive error → output clamped, integrator frozen. */
void test_saturation_high_freezes_integrator(void) {
    pid_init(&pid, &(pid_gains_t){100.0f, 50.0f, 0.0f, 0.0f, 0.0f});
    pid_reset(&pid, 25.0f);
    float before = pid.integrator;
    float out = pid_step(&pid, 200.0f, 0.0f, 25.0f, 0.25f, PID_OUTPUT_MAX);   /* big positive error */
    TEST_ASSERT_EQUAL_FLOAT(100.0f, out);
    TEST_ASSERT_EQUAL_FLOAT(before, pid.integrator);    /* not updated */
}

/* Saturation low + negative error → output clamped at 0, integrator frozen. */
void test_saturation_low_freezes_integrator(void) {
    pid_init(&pid, &(pid_gains_t){100.0f, 50.0f, 0.0f, 0.0f, 0.0f});
    pid_reset(&pid, 250.0f);
    float before = pid.integrator;
    float out = pid_step(&pid, 25.0f, 0.0f, 250.0f, 0.25f, PID_OUTPUT_MAX);   /* big negative error */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out);
    TEST_ASSERT_EQUAL_FLOAT(before, pid.integrator);
}

/* Within range: integrator accumulates. */
void test_within_range_integrator_grows(void) {
    pid_init(&pid, &(pid_gains_t){1.0f, 1.0f, 0.0f, 0.0f, 0.0f});
    pid_reset(&pid, 25.0f);
    pid_step(&pid, 30.0f, 0.0f, 25.0f, 0.25f, PID_OUTPUT_MAX);    /* error 5, accumulates */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f * 0.25f, pid.integrator);
}

/* Live gain change does not reset integrator. */
void test_set_gains_preserves_integrator(void) {
    pid_init(&pid, &(pid_gains_t){1.0f, 1.0f, 0.0f, 0.0f, 0.0f});
    pid_reset(&pid, 25.0f);
    pid_step(&pid, 30.0f, 0.0f, 25.0f, 0.25f, PID_OUTPUT_MAX);
    float before = pid.integrator;
    pid_set_gains(&pid, &(pid_gains_t){5.0f, 2.0f, 0.5f, 0.0f, 0.0f});
    TEST_ASSERT_EQUAL_FLOAT(before, pid.integrator);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, pid.g.kp);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, pid.g.ki);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, pid.g.kd);
}

/* Derivative on measurement: a setpoint step alone produces no derivative kick. */
void test_setpoint_step_does_not_cause_derivative_kick(void) {
    pid_init(&pid, &(pid_gains_t){0.0f, 0.0f, 100.0f, 0.0f, 0.0f});   /* Kp=Ki=0 isolates d_term */
    pid_reset(&pid, 50.0f);
    float out = pid_step(&pid, 200.0f, 0.0f, 50.0f, 0.25f, PID_OUTPUT_MAX);   /* temp unchanged */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out);
}

/* Derivative low-pass: a single-sample temperature spike (thermocouple noise)
 * is heavily attenuated, not passed to the output at full derivative gain.
 * Temp DROPS 4 C in one 0.25 s tick → raw d_term = -Kd·(Δtemp)/dt = +160 %,
 * which unfiltered would clamp the output to 100 %. The LPF (tau ~2 s) lets
 * only a small fraction through on the first step. */
void test_derivative_filter_attenuates_spike(void) {
    pid_init(&pid, &(pid_gains_t){0.0f, 0.0f, 10.0f, 0.0f, 0.0f});   /* Kp=Ki=0 isolates the d_term */
    pid_reset(&pid, 100.0f);
    float out = pid_step(&pid, 100.0f, 0.0f, 96.0f, 0.25f, PID_OUTPUT_MAX);   /* setpoint==prev so P/I=0 */
    TEST_ASSERT_TRUE(out > 0.0f);        /* some derivative passes */
    TEST_ASSERT_TRUE(out < 40.0f);       /* but far below the unfiltered 160→clamp 100 */
}

/* The filter must still pass a SUSTAINED derivative (real thermal rise/fall):
 * a steady cooling ramp converges the filtered d_term to the true derivative. */
void test_derivative_filter_passes_sustained_ramp(void) {
    pid_init(&pid, &(pid_gains_t){0.0f, 0.0f, 10.0f, 0.0f, 0.0f});
    pid_reset(&pid, 100.0f);
    float out = 0.0f, temp = 100.0f;
    for (int i = 0; i < 40; i++) {       /* −1 C per 0.25 s → steady raw d = +40 % */
        temp -= 1.0f;
        out = pid_step(&pid, 100.0f, 0.0f, temp, 0.25f, PID_OUTPUT_MAX);
    }
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 40.0f, out);   /* converged to the true derivative */
}

/* === Feedforward (spec 2026-07-19 §1) ==================================== */

/* ff hold term: kp=ki=kd=0 isolates it. ambient_ref=25 (reset), setpoint 125
 * -> ff = 0.3*(125-25) = 30 %. temp==prev so D=0. */
void test_ff_hold_term(void) {
    pid_init(&pid, &(pid_gains_t){0, 0, 0, 0.3f, 0});
    pid_reset(&pid, 25.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 30.0f, pid_step(&pid, 125.0f, 0.0f, 125.0f, 0.25f, PID_OUTPUT_MAX));
}

/* ff rate term: 71 %·s/°C * 0.6 °C/s = 42.6 %. setpoint==temp==ambient_ref. */
void test_ff_rate_term(void) {
    pid_init(&pid, &(pid_gains_t){0, 0, 0, 0, 71.0f});
    pid_reset(&pid, 25.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 42.6f, pid_step(&pid, 25.0f, 0.6f, 25.0f, 0.25f, PID_OUTPUT_MAX));
}

/* Negative rate (cool-down tail) must contribute nothing. */
void test_ff_negative_rate_clamped(void) {
    pid_init(&pid, &(pid_gains_t){0, 0, 0, 0, 71.0f});
    pid_reset(&pid, 25.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, pid_step(&pid, 25.0f, -1.0f, 25.0f, 0.25f, PID_OUTPUT_MAX));
}

/* Hold term never negative: setpoint below ambient_ref contributes 0, not <0. */
void test_ff_hold_term_not_negative(void) {
    pid_init(&pid, &(pid_gains_t){0, 0, 0, 0.5f, 0});
    pid_reset(&pid, 30.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, pid_step(&pid, 20.0f, 0.0f, 20.0f, 0.25f, PID_OUTPUT_MAX));
}

/* ambient_ref clamps: a hot restart (90 °C) clamps to AMBIENT_REF_MAX_C. */
void test_ambient_ref_clamped(void) {
    pid_init(&pid, &(pid_gains_t){0, 0, 0, 0.2f, 0});
    pid_reset(&pid, 90.0f);
    TEST_ASSERT_EQUAL_FLOAT(AMBIENT_REF_MAX_C, pid.ambient_ref);
    pid_reset(&pid, 5.0f);
    TEST_ASSERT_EQUAL_FLOAT(AMBIENT_REF_MIN_C, pid.ambient_ref);
}

/* Anti-windup must saturate on ff + PID, not PID alone: ff alone exceeds 100,
 * error positive -> output clamped to 100 AND integrator frozen. */
void test_antiwindup_tests_the_sum(void) {
    pid_init(&pid, &(pid_gains_t){0.1f, 1.0f, 0, 1.0f, 0});  /* small P, big I */
    pid_reset(&pid, 25.0f);
    float before = pid.integrator;
    /* setpoint 200: ff = 1.0*(200-25) = 175 > 100 on its own; error > 0 */
    float out = pid_step(&pid, 200.0f, 0.0f, 150.0f, 0.25f, PID_OUTPUT_MAX);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, out);
    TEST_ASSERT_EQUAL_FLOAT(before, pid.integrator);
}

/* The ceiling must participate in anti-windup: the integrator has to stop
 * climbing when the OUTPUT saturates, and where it stops must depend on
 * out_max. Gains are chosen so the integrator climbs over several ticks
 * (6.25/step) rather than saturating on the first call — otherwise the test
 * passes vacuously with the integrator still at 0. */
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

    /* It actually integrated — not frozen at 0 from the first call. */
    TEST_ASSERT_TRUE(i50 > 6.0f);
    /* It froze rather than running away: one more increment would have
     * pushed p_term + integrator past the ceiling. */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 43.75f, i50);
}

void test_freeze_point_tracks_the_ceiling(void) {
    /* The same input sequence must freeze the integrator LATER at a higher
     * ceiling. This is what fails if out_max is ignored by the saturation
     * test — both would freeze at the PID_OUTPUT_MAX point. */
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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reset_clears_integrator_and_sets_prev_temp);
    RUN_TEST(test_first_step_has_zero_derivative);
    RUN_TEST(test_saturation_high_freezes_integrator);
    RUN_TEST(test_saturation_low_freezes_integrator);
    RUN_TEST(test_within_range_integrator_grows);
    RUN_TEST(test_set_gains_preserves_integrator);
    RUN_TEST(test_setpoint_step_does_not_cause_derivative_kick);
    RUN_TEST(test_derivative_filter_attenuates_spike);
    RUN_TEST(test_derivative_filter_passes_sustained_ramp);
    RUN_TEST(test_ff_hold_term);
    RUN_TEST(test_ff_rate_term);
    RUN_TEST(test_ff_negative_rate_clamped);
    RUN_TEST(test_ff_hold_term_not_negative);
    RUN_TEST(test_ambient_ref_clamped);
    RUN_TEST(test_antiwindup_tests_the_sum);
    RUN_TEST(test_integrator_freezes_at_custom_ceiling);
    RUN_TEST(test_freeze_point_tracks_the_ceiling);
    RUN_TEST(test_default_ceiling_matches_legacy);
    return UNITY_END();
}

/* test/test_smoke/test_smoke.c — confirms the native build path works
 * and config.h cross-bounds are respected. */
#include <unity.h>
#include "config.h"

void setUp(void) {}
void tearDown(void) {}

void test_over_temp_limit_above_reflow_max(void) {
    TEST_ASSERT_TRUE(OVER_TEMP_LIMIT_C > REFLOW_TEMP_MAX);
}

void test_pwm_window_is_multiple_of_tick(void) {
    TEST_ASSERT_EQUAL_INT(0, SSR_PWM_WINDOW_MS % CONTROL_TICK_MS);
    TEST_ASSERT_EQUAL_INT(8, SSR_QUANTA_PER_WINDOW);
}

void test_defaults_within_bounds(void) {
    TEST_ASSERT_TRUE(SOAK_TEMP_DEFAULT >= SOAK_TEMP_MIN);
    TEST_ASSERT_TRUE(SOAK_TEMP_DEFAULT <= SOAK_TEMP_MAX);
    TEST_ASSERT_TRUE(REFLOW_TEMP_DEFAULT >= REFLOW_TEMP_MIN);
    TEST_ASSERT_TRUE(REFLOW_TEMP_DEFAULT <= REFLOW_TEMP_MAX);
    TEST_ASSERT_TRUE(FIRST_HEATUP_DEFAULT >= HEATUP_RATE_MIN);
    TEST_ASSERT_TRUE(FIRST_HEATUP_DEFAULT <= HEATUP_RATE_MAX);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_over_temp_limit_above_reflow_max);
    RUN_TEST(test_pwm_window_is_multiple_of_tick);
    RUN_TEST(test_defaults_within_bounds);
    return UNITY_END();
}

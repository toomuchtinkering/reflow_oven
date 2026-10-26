#include <unity.h>
#include "drivers/ssr_scheduler.h"
#include "config.h"

void setUp(void) {}
void tearDown(void) {}

void test_duty_zero_always_off(void) {
    for (uint32_t t = 0; t < 10000; t += CONTROL_TICK_MS) {
        TEST_ASSERT_FALSE(ssr_scheduler_should_be_on(t, 0, SSR_QUANTA_PER_WINDOW));
    }
}

void test_duty_100_always_on(void) {
    for (uint32_t t = 0; t < 10000; t += CONTROL_TICK_MS) {
        TEST_ASSERT_TRUE(ssr_scheduler_should_be_on(t, 100, SSR_QUANTA_PER_WINDOW));
    }
}

void test_duty_50_half_on(void) {
    int on = 0;
    for (uint32_t t = 0; t < SSR_PWM_WINDOW_MS; t += CONTROL_TICK_MS) {
        if (ssr_scheduler_should_be_on(t, 50, SSR_QUANTA_PER_WINDOW)) on++;
    }
    TEST_ASSERT_EQUAL_INT(SSR_QUANTA_PER_WINDOW / 2, on);
}

void test_duty_99_rounds_to_full_window(void) {
    int on = 0;
    for (uint32_t t = 0; t < SSR_PWM_WINDOW_MS; t += CONTROL_TICK_MS) {
        if (ssr_scheduler_should_be_on(t, 99, SSR_QUANTA_PER_WINDOW)) on++;
    }
    TEST_ASSERT_EQUAL_INT(SSR_QUANTA_PER_WINDOW, on);
}

void test_duty_1_rounds_to_off(void) {
    int on = 0;
    for (uint32_t t = 0; t < SSR_PWM_WINDOW_MS; t += CONTROL_TICK_MS) {
        if (ssr_scheduler_should_be_on(t, 1, SSR_QUANTA_PER_WINDOW)) on++;
    }
    TEST_ASSERT_EQUAL_INT(0, on);
}

/* The defect: at the reflow window, a 5 % duty never fires. */
void test_low_duty_is_lost_at_reflow_window(void) {
    int on = 0;
    for (uint32_t t = 0; t < SSR_PWM_WINDOW_MS; t += CONTROL_TICK_MS) {
        if (ssr_scheduler_should_be_on(t, 5, SSR_QUANTA_PER_WINDOW)) on++;
    }
    TEST_ASSERT_EQUAL_INT(0, on);
}

/* The fix: at the drying window, it fires. */
void test_low_duty_survives_at_drying_window(void) {
    int on = 0;
    const uint32_t window = (uint32_t)DRY_SSR_QUANTA * CONTROL_TICK_MS;
    for (uint32_t t = 0; t < window; t += CONTROL_TICK_MS) {
        if (ssr_scheduler_should_be_on(t, 5, DRY_SSR_QUANTA)) on++;
    }
    TEST_ASSERT_EQUAL_INT(6, on);   /* (5*120+50)/100 = 6 */
}

void test_drying_window_resolves_one_percent(void) {
    int on = 0;
    const uint32_t window = (uint32_t)DRY_SSR_QUANTA * CONTROL_TICK_MS;
    for (uint32_t t = 0; t < window; t += CONTROL_TICK_MS) {
        if (ssr_scheduler_should_be_on(t, 1, DRY_SSR_QUANTA)) on++;
    }
    TEST_ASSERT_EQUAL_INT(1, on);   /* (1*120+50)/100 = 170/100 = 1 (integer division) */
}

void test_quanta_one_is_all_or_nothing(void) {
    TEST_ASSERT_FALSE(ssr_scheduler_should_be_on(0, 49, 1));
    TEST_ASSERT_TRUE(ssr_scheduler_should_be_on(0, 51, 1));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_duty_zero_always_off);
    RUN_TEST(test_duty_100_always_on);
    RUN_TEST(test_duty_50_half_on);
    RUN_TEST(test_duty_99_rounds_to_full_window);
    RUN_TEST(test_duty_1_rounds_to_off);
    RUN_TEST(test_low_duty_is_lost_at_reflow_window);
    RUN_TEST(test_low_duty_survives_at_drying_window);
    RUN_TEST(test_drying_window_resolves_one_percent);
    RUN_TEST(test_quanta_one_is_all_or_nothing);
    return UNITY_END();
}

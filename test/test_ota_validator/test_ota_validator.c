#include "unity.h"
#include <stdbool.h>

/* Mock hooks — overridden by the test. The real impl on ESP_PLATFORM
 * uses the actual drivers. */
static bool mock_tc = true, mock_ssr = true, mock_display = true, mock_touch = true;
bool ota_validator_hook_tc_alive(void)      { return mock_tc; }
bool ota_validator_hook_ssr_alive(void)     { return mock_ssr; }
bool ota_validator_hook_display_alive(void) { return mock_display; }
bool ota_validator_hook_touch_alive(void)   { return mock_touch; }

#include "ota/ota_validator.h"

void test_all_pass(void) {
    mock_tc = mock_ssr = mock_display = mock_touch = true;
    TEST_ASSERT_TRUE(ota_validator_run_self_tests());
}
void test_tc_fail(void) {
    mock_ssr = mock_display = mock_touch = true; mock_tc = false;
    TEST_ASSERT_FALSE(ota_validator_run_self_tests());
}
void test_ssr_fail(void) {
    mock_tc = mock_display = mock_touch = true; mock_ssr = false;
    TEST_ASSERT_FALSE(ota_validator_run_self_tests());
}
void test_display_fail(void) {
    mock_tc = mock_ssr = mock_touch = true; mock_display = false;
    TEST_ASSERT_FALSE(ota_validator_run_self_tests());
}
void test_touch_fail(void) {
    mock_tc = mock_ssr = mock_display = true; mock_touch = false;
    TEST_ASSERT_FALSE(ota_validator_run_self_tests());
}

/* ── TC sample-window consensus (pure; the on-target hook retries several
 *    windows of 3 samples because boot-time readings wiggle — WiFi bursts +
 *    LCD common-mode pickup) ─────────────────────────────────────────────── */

void test_tc_window_clean_samples_pass(void) {
    float   t[3] = {22.0f, 23.0f, 22.5f};
    uint8_t f[3] = {0, 0, 0};
    TEST_ASSERT_TRUE(ota_validator_tc_window_ok(t, f));
}

void test_tc_window_fault_flag_fails(void) {
    float   t[3] = {22.0f, 23.0f, 22.5f};
    uint8_t f[3] = {0, 0x1, 0};
    TEST_ASSERT_FALSE(ota_validator_tc_window_ok(t, f));
}

void test_tc_window_out_of_range_fails(void) {
    float   t_hot[3]  = {22.0f, 400.0f, 22.5f};
    float   t_cold[3] = {-60.0f, 22.0f, 22.5f};
    uint8_t f[3] = {0, 0, 0};
    TEST_ASSERT_FALSE(ota_validator_tc_window_ok(t_hot, f));
    TEST_ASSERT_FALSE(ota_validator_tc_window_ok(t_cold, f));
}

void test_tc_window_wide_spread_fails(void) {
    /* 6 °C spread within 150 ms = sensor not usable, even with the
     * loosened 4 °C tolerance. */
    float   t[3] = {20.0f, 26.0f, 22.0f};
    uint8_t f[3] = {0, 0, 0};
    TEST_ASSERT_FALSE(ota_validator_tc_window_ok(t, f));
}

void test_tc_window_moderate_wiggle_passes(void) {
    /* 3.5 °C spread — the boot-time wiggle the 2 °C tolerance rejected
     * (2026-06-10 bench: validation was a coin flip on a healthy sensor). */
    float   t[3] = {21.0f, 24.5f, 22.0f};
    uint8_t f[3] = {0, 0, 0};
    TEST_ASSERT_TRUE(ota_validator_tc_window_ok(t, f));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_all_pass);
    RUN_TEST(test_tc_fail);
    RUN_TEST(test_ssr_fail);
    RUN_TEST(test_display_fail);
    RUN_TEST(test_touch_fail);
    RUN_TEST(test_tc_window_clean_samples_pass);
    RUN_TEST(test_tc_window_fault_flag_fails);
    RUN_TEST(test_tc_window_out_of_range_fails);
    RUN_TEST(test_tc_window_wide_spread_fails);
    RUN_TEST(test_tc_window_moderate_wiggle_passes);
    return UNITY_END();
}

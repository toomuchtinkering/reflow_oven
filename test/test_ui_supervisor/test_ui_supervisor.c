#include <unity.h>
#include "ui_supervisor.h"

void setUp(void) {}
void tearDown(void) {}

/* Before the UI has ticked once the supervisor must stay silent, however long
 * boot takes — display + touch bring-up and the OTA self-tests all run before
 * the first LVGL refresh. */
static void test_not_armed_never_stalls(void) {
    TEST_ASSERT_FALSE(ui_supervisor_stalled(false, 0, 0, 10000));
    TEST_ASSERT_FALSE(ui_supervisor_stalled(false, 999999, 0, 10000));
}

static void test_fresh_beat_is_not_a_stall(void) {
    TEST_ASSERT_FALSE(ui_supervisor_stalled(true, 1000, 1000, 10000));
    TEST_ASSERT_FALSE(ui_supervisor_stalled(true, 10999, 1000, 10000));
}

static void test_stall_at_and_beyond_the_timeout(void) {
    TEST_ASSERT_TRUE(ui_supervisor_stalled(true, 11000, 1000, 10000));
    TEST_ASSERT_TRUE(ui_supervisor_stalled(true, 90000, 1000, 10000));
}

/* An oven left powered for 49.7 days must not reboot itself when the
 * millisecond counter rolls over. The unsigned difference stays small. */
static void test_millisecond_wraparound(void) {
    const uint32_t late = 0xFFFFF000u;       /* shortly before the wrap */
    TEST_ASSERT_FALSE(ui_supervisor_stalled(true, late + 500u, late, 10000));
    TEST_ASSERT_FALSE(ui_supervisor_stalled(true, 0x00000FFFu, late, 10000));  /* ~8 s across */
    TEST_ASSERT_TRUE(ui_supervisor_stalled(true, 0x00002000u, late, 10000));   /* ~12 s across */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_not_armed_never_stalls);
    RUN_TEST(test_fresh_beat_is_not_a_stall);
    RUN_TEST(test_stall_at_and_beyond_the_timeout);
    RUN_TEST(test_millisecond_wraparound);
    return UNITY_END();
}

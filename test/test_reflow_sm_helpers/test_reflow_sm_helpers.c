#include "unity.h"
#include "control/reflow_sm.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_stage_name_covers_every_value(void) {
    TEST_ASSERT_EQUAL_STRING("Idle",      stage_name(STAGE_NONE));
    TEST_ASSERT_EQUAL_STRING("Heat-up 1", stage_name(STAGE_HEAT_UP_1));
    TEST_ASSERT_EQUAL_STRING("Soak",      stage_name(STAGE_SOAK));
    TEST_ASSERT_EQUAL_STRING("Heat-up 2", stage_name(STAGE_HEAT_UP_2));
    TEST_ASSERT_EQUAL_STRING("Reflow",    stage_name(STAGE_REFLOW));
    TEST_ASSERT_EQUAL_STRING("Cool-down", stage_name(STAGE_COOL_DOWN));
}

static void test_stage_name_never_returns_null(void) {
    /* Out-of-range value still returns a string. */
    TEST_ASSERT_NOT_NULL(stage_name((stage_t)42));
}

static void test_fault_text_none(void) {
    TEST_ASSERT_EQUAL_STRING("unknown", fault_text(0));
}

static void test_fault_text_each_flag(void) {
    TEST_ASSERT_EQUAL_STRING("thermocouple open",        fault_text(FAULT_TC_OPEN));
    TEST_ASSERT_EQUAL_STRING("thermocouple short to GND", fault_text(FAULT_TC_SCG));
    TEST_ASSERT_EQUAL_STRING("thermocouple short to VCC", fault_text(FAULT_TC_SCV));
    TEST_ASSERT_EQUAL_STRING("stale sensor",             fault_text(FAULT_TC_STALE));
    TEST_ASSERT_EQUAL_STRING("over-temperature",         fault_text(FAULT_OVER_TEMP));
    TEST_ASSERT_EQUAL_STRING("electronics too hot",      fault_text(FAULT_ELECTRONICS_HOT));
}

/* fault_text is an if-chain, so an appended flag would silently fall through
 * to "unknown" — the reason the test above once stopped at FAULT_OVER_TEMP
 * despite its name. This walks every bit up to the sentinel instead, so the
 * next flag fails here until fault_text learns it. */
static void test_fault_text_knows_every_defined_flag(void) {
    for (unsigned b = 1; b <= FAULT_FLAG_LAST_; b <<= 1) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("unknown", fault_text((uint8_t)b)));
    }
}

static void test_fault_text_priority_when_multiple_set(void) {
    /* TC_OPEN has highest priority. */
    uint8_t both = FAULT_TC_OPEN | FAULT_OVER_TEMP;
    TEST_ASSERT_EQUAL_STRING("thermocouple open", fault_text(both));
    /* An over-temperature is the more acute hazard, so it must keep winning
     * the chain when a long dry has tripped both. */
    TEST_ASSERT_EQUAL_STRING("over-temperature",
                             fault_text(FAULT_OVER_TEMP | FAULT_ELECTRONICS_HOT));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_stage_name_covers_every_value);
    RUN_TEST(test_stage_name_never_returns_null);
    RUN_TEST(test_fault_text_none);
    RUN_TEST(test_fault_text_each_flag);
    RUN_TEST(test_fault_text_knows_every_defined_flag);
    RUN_TEST(test_fault_text_priority_when_multiple_set);
    return UNITY_END();
}

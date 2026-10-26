/* test/test_wifi_state.c — host-side, [env:native] */
#include "unity.h"
/* Use the real driver header (the FSM types + wifi_fsm_next are host-safe)
 * instead of a local enum copy, so a future reorder of the driver enums can't
 * silently diverge from what these tests assert (finding #50). */
#include "drivers/wifi.h"

void test_uninit_to_idle(void)                  { TEST_ASSERT_EQUAL(WIFI_STATE_IDLE,       wifi_fsm_next(WIFI_STATE_UNINIT, WIFI_EV_INIT_DONE)); }
void test_idle_to_scanning(void)                { TEST_ASSERT_EQUAL(WIFI_STATE_SCANNING,   wifi_fsm_next(WIFI_STATE_IDLE,    WIFI_EV_SCAN_START)); }
void test_scanning_back_to_idle(void)           { TEST_ASSERT_EQUAL(WIFI_STATE_IDLE,       wifi_fsm_next(WIFI_STATE_SCANNING,WIFI_EV_SCAN_DONE)); }
void test_idle_to_connecting(void)              { TEST_ASSERT_EQUAL(WIFI_STATE_CONNECTING, wifi_fsm_next(WIFI_STATE_IDLE,    WIFI_EV_CONNECT_START)); }
void test_connecting_to_connected(void)         { TEST_ASSERT_EQUAL(WIFI_STATE_CONNECTED,  wifi_fsm_next(WIFI_STATE_CONNECTING,WIFI_EV_GOT_IP)); }
void test_connecting_self_loop_on_disconnect(void) {
    /* During CONNECTING, a disconnect event keeps us in CONNECTING (retry). */
    TEST_ASSERT_EQUAL(WIFI_STATE_CONNECTING, wifi_fsm_next(WIFI_STATE_CONNECTING, WIFI_EV_DISCONNECTED));
}
void test_connected_to_connecting_on_disconnect(void) { TEST_ASSERT_EQUAL(WIFI_STATE_CONNECTING, wifi_fsm_next(WIFI_STATE_CONNECTED, WIFI_EV_DISCONNECTED)); }
void test_forget_from_connected(void)           { TEST_ASSERT_EQUAL(WIFI_STATE_IDLE,       wifi_fsm_next(WIFI_STATE_CONNECTED, WIFI_EV_FORGET)); }
void test_forget_from_connecting(void)          { TEST_ASSERT_EQUAL(WIFI_STATE_IDLE,       wifi_fsm_next(WIFI_STATE_CONNECTING,WIFI_EV_FORGET)); }
void test_unknown_event_keeps_state(void)       { TEST_ASSERT_EQUAL(WIFI_STATE_IDLE,       wifi_fsm_next(WIFI_STATE_IDLE,    WIFI_EV_GOT_IP)); }

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_uninit_to_idle);
    RUN_TEST(test_idle_to_scanning);
    RUN_TEST(test_scanning_back_to_idle);
    RUN_TEST(test_idle_to_connecting);
    RUN_TEST(test_connecting_to_connected);
    RUN_TEST(test_connecting_self_loop_on_disconnect);
    RUN_TEST(test_connected_to_connecting_on_disconnect);
    RUN_TEST(test_forget_from_connected);
    RUN_TEST(test_forget_from_connecting);
    RUN_TEST(test_unknown_event_keeps_state);
    return UNITY_END();
}

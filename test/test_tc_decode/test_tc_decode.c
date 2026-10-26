/* test/test_tc_decode/test_tc_decode.c
 * Pure decode of the MAX31855 32-bit frame: thermocouple temp (bits 31:18,
 * 0.25 °C/LSB), cold-junction temp (bits 15:4, 0.0625 °C/LSB), fault flags
 * (bits 2:0 + the bit-16 summary). Frames are built by hand from the
 * datasheet layout so the tests don't depend on the decoder under test.
 */
#include <unity.h>
#include "drivers/tc_decode.h"
#include "control/reflow_sm.h"

void setUp(void)    {}
void tearDown(void) {}

/* Compose a frame from signed raw counts + fault bits. */
static uint32_t frame(int16_t tc_raw14, int16_t cj_raw12, uint8_t fault_bits, bool fault_summary) {
    uint32_t w = 0;
    w |= ((uint32_t)tc_raw14 & 0x3FFF) << 18;
    w |= ((uint32_t)cj_raw12 & 0xFFF) << 4;
    w |= fault_bits & 0x7;
    if (fault_summary) w |= 1u << 16;
    return w;
}

void test_positive_temps(void) {
    /* TC +100.00 °C = 400 LSB; CJ +25.0 °C = 400 LSB */
    tc_frame_t f;
    tc_decode(frame(400, 400, 0, false), &f);
    TEST_ASSERT_EQUAL_UINT8(0, f.fault_flags);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, f.tc_c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, f.cj_c);
}

void test_negative_tc_temp_sign_extends(void) {
    /* TC -25.00 °C = -100 LSB (14-bit two's complement) */
    tc_frame_t f;
    tc_decode(frame(-100, 0, 0, false), &f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -25.0f, f.tc_c);
}

void test_negative_cj_temp_sign_extends(void) {
    /* CJ -10.0 °C = -160 LSB (12-bit two's complement) */
    tc_frame_t f;
    tc_decode(frame(0, -160, 0, false), &f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -10.0f, f.cj_c);
}

void test_fault_bits_map_to_flags(void) {
    tc_frame_t f;
    tc_decode(frame(0, 0, 0x1, true), &f);
    TEST_ASSERT_EQUAL_UINT8(FAULT_TC_OPEN, f.fault_flags);
    tc_decode(frame(0, 0, 0x2, true), &f);
    TEST_ASSERT_EQUAL_UINT8(FAULT_TC_SCG, f.fault_flags);
    tc_decode(frame(0, 0, 0x4, true), &f);
    TEST_ASSERT_EQUAL_UINT8(FAULT_TC_SCV, f.fault_flags);
}

void test_summary_bit_without_detail_falls_back_to_open(void) {
    /* Bit 16 set but D2:0 clear — keep the existing "treat as open" fallback. */
    tc_frame_t f;
    tc_decode(frame(0, 0, 0, true), &f);
    TEST_ASSERT_EQUAL_UINT8(FAULT_TC_OPEN, f.fault_flags);
}

void test_all_zero_frame_is_open_fault(void) {
    /* A MISO line stuck low (broken wire, unpowered chip, solder bridge)
     * yields a frame of 32 zero bits, which would otherwise decode as a
     * plausible "0.00 °C, no fault" and let the PID heat forever. A real
     * frame can't legitimately be all-zero next to a powered ESP32 (the
     * cold junction would have to read exactly 0.0000 °C). */
    tc_frame_t f;
    tc_decode(0x00000000u, &f);
    TEST_ASSERT_EQUAL_UINT8(FAULT_TC_OPEN, f.fault_flags);
}

void test_cold_junction_still_valid_on_faulted_frame(void) {
    /* Probe unplugged: OC fault, but the chip keeps reporting its own temp.
     * CJ +41.25 °C = 660 LSB. */
    tc_frame_t f;
    tc_decode(frame(0, 660, 0x1, true), &f);
    TEST_ASSERT_EQUAL_UINT8(FAULT_TC_OPEN, f.fault_flags);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 41.25f, f.cj_c);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_positive_temps);
    RUN_TEST(test_negative_tc_temp_sign_extends);
    RUN_TEST(test_negative_cj_temp_sign_extends);
    RUN_TEST(test_fault_bits_map_to_flags);
    RUN_TEST(test_summary_bit_without_detail_falls_back_to_open);
    RUN_TEST(test_all_zero_frame_is_open_fault);
    RUN_TEST(test_cold_junction_still_valid_on_faulted_frame);
    UNITY_END();
    return 0;
}

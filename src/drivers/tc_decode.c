#include "drivers/tc_decode.h"
#include "control/reflow_sm.h"   /* FAULT_TC_* */

void tc_decode(uint32_t frame, tc_frame_t *out) {
    uint8_t flags = 0;
    /* All-zero frame = MISO stuck low (broken wire, unpowered chip). It would
     * otherwise decode as a valid 0.00 °C and defeat every thermal gate while
     * the heater runs; a genuine frame can't be all-zero (the cold junction
     * sits next to a powered ESP32). Stuck-high (all-ones) already trips the
     * OC fault bit below. */
    if (frame == 0) flags |= FAULT_TC_OPEN;
    if (frame & 0x1) flags |= FAULT_TC_OPEN;
    if (frame & 0x2) flags |= FAULT_TC_SCG;
    if (frame & 0x4) flags |= FAULT_TC_SCV;
    if ((frame & (1u << 16)) && !flags) flags = FAULT_TC_OPEN;
    out->fault_flags = flags;

    int16_t tc_raw = (int16_t)((frame >> 18) & 0x3FFF);
    if (tc_raw & 0x2000) tc_raw |= 0xC000;
    out->tc_c = tc_raw * 0.25f;

    int16_t cj_raw = (int16_t)((frame >> 4) & 0xFFF);
    if (cj_raw & 0x800) cj_raw |= 0xF000;
    out->cj_c = cj_raw * 0.0625f;
}

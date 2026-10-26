/* Pure MAX31855 frame decode — no IDF dependencies so the native test env
 * can compile it (drivers/thermocouple.c, which owns the SPI bus, is
 * excluded there). See spec 2026-06-10 §1. */
#pragma once
#include <stdint.h>

typedef struct {
    float   tc_c;        /* thermocouple temp, 0.25 °C/LSB (bits 31:18) */
    float   cj_c;        /* cold-junction temp, 0.0625 °C/LSB (bits 15:4) */
    uint8_t fault_flags; /* FAULT_TC_OPEN / FAULT_TC_SCG / FAULT_TC_SCV */
} tc_frame_t;

/* Decode a raw 32-bit MAX31855 frame. cj_c is valid even on faulted frames
 * (the chip keeps reporting its own junction temp with the probe open). */
void tc_decode(uint32_t frame, tc_frame_t *out);

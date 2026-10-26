/* MAX31855 K-type thermocouple driver. SPI3 (VSPI), see config.h. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Initialise bus + device. Call once at boot. */
void thermocouple_init(void);

/* Blocking read (~50 us SPI). Populates *temp_c and *fault_flags (FAULT_TC_*
 * from reflow_sm.h, OR-combined; 0 = no fault). *age_ms is 0 immediately
 * after a successful read; if the call returns false, *age_ms is left
 * untouched and the caller should treat the previous reading as stale.
 * *cj_c (optional, may be NULL) receives the chip's cold-junction
 * temperature; it is written whenever the SPI transfer succeeds — including
 * thermocouple-fault frames (open probe etc.) — and left untouched only on
 * SPI-transport failure.
 * Returns true on a successful read with no fault.
 */
bool thermocouple_read(float *temp_c, float *cj_c, uint8_t *fault_flags, uint32_t *age_ms);

/* Analytic drying setpoint — the whole reason drying needs no curve array.
 *
 *   setpoint(t) = min(start_temp_c + DRY_RAMP_RATE_C_S * t, target_c)
 *
 * A dry of the configured maximum (DRY_DURATION_MAX_MS, 16 h) sampled at
 * PROFILE_CURVE_STEP_MS = 1 s would need 57,600 float samples = 225 KiB
 * against 37.4 KiB of free static DRAM; this is two floats and a multiply.
 * Pure logic, no state. See drying spec 1.1.
 */
#pragma once

#include <stdint.h>

/* Setpoint in °C at `elapsed_ms` into the run. If start_temp_c >= target_c
 * the result is target_c for all t — the setpoint never descends. */
float dry_setpoint(float start_temp_c, float target_c, uint32_t elapsed_ms);

/* Commanded setpoint slope (°C/s) at `elapsed_ms`, for the feedforward
 * k_rate term: DRY_RAMP_RATE_C_S while ramping, 0 once at target. */
float dry_setpoint_rate(float start_temp_c, float target_c, uint32_t elapsed_ms);

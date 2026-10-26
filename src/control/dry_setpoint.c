#include "control/dry_setpoint.h"
#include "config.h"

float dry_setpoint(float start_temp_c, float target_c, uint32_t elapsed_ms) {
    if (start_temp_c >= target_c) return target_c;
    float sp = start_temp_c + DRY_RAMP_RATE_C_S * ((float)elapsed_ms / 1000.0f);
    return (sp > target_c) ? target_c : sp;
}

float dry_setpoint_rate(float start_temp_c, float target_c, uint32_t elapsed_ms) {
    /* Single source of truth for the ramp formula: the rate is just the
     * derivative of dry_setpoint() — nonzero only while it hasn't reached
     * target_c yet (this also covers start_temp_c >= target_c, where
     * dry_setpoint() returns target_c immediately). */
    float sp = dry_setpoint(start_temp_c, target_c, elapsed_ms);
    return (sp < target_c) ? DRY_RAMP_RATE_C_S : 0.0f;
}

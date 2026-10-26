/* Pre-computes a setpoint-vs-time curve for one reflow profile.
 * Curve is sampled at PROFILE_CURVE_STEP_MS intervals.
 * See spec §4.3 and §5.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    float    first_heatup_rate;   /* °C/s */
    uint16_t soak_temp;           /* °C */
    uint16_t soak_time;           /* s */
    float    second_heatup_rate;
    uint16_t reflow_temp;
    uint16_t reflow_time;
    float    cool_down_rate;      /* °C/s; passive cool-down rate used for the planned curve tail */
} profile_t;

typedef enum {
    STAGE_NONE,
    STAGE_HEAT_UP_1,
    STAGE_SOAK,
    STAGE_HEAT_UP_2,
    STAGE_REFLOW,
    STAGE_COOL_DOWN,
    STAGE_COUNT_
} stage_t;

/* Worst-case ACTIVE profile (heat/soak/reflow) under the bounds in config.h.
 * Heat-up 1 starts at the run-start temperature, floored at PROFILE_START_MIN_C
 * (a colder start = a longer ramp 1), so the worst case uses that floor:
 *   Heat-up 1: (SOAK_TEMP_MAX - PROFILE_START_MIN_C)/ HEATUP_RATE_MIN = 900 s
 *   Soak:                                        SOAK_TIME_MAX   = 300 s
 *   Heat-up 2: (REFLOW_TEMP_MAX - SOAK_TEMP_MAX)/ HEATUP_RATE_MIN = 250 s
 *   Reflow:                                      REFLOW_TIME_MAX = 120 s
 *   Total = 1570 s. At PROFILE_CURVE_STEP_MS = 1000 ms that's 1570
 * samples; we keep 1600 for a small safety margin. The cap MUST
 * cover the worst-case ACTIVE region — truncating it would corrupt
 * phase_end_ms and the setpoint lookup; only the cool-down tail may
 * truncate (graph-only, the SM exits cool-down on temp/timeout). Array
 * sizes scale with this: s_plan_pts + s_live_pts + s_curve_copy + the
 * heap-allocated profile_curve_t. */
#define PROFILE_CURVE_MAX_SAMPLES 1600

typedef struct {
    float    samples[PROFILE_CURVE_MAX_SAMPLES];
    uint16_t length;
    uint32_t phase_end_ms[STAGE_COUNT_];
    /* Signed setpoint slope (°C/s) per stage; filled by generate.
     * Consumers wanting heat-only ff must clamp negatives themselves. */
    float    stage_rate[STAGE_COUNT_];
} profile_curve_t;

bool profile_validate(const profile_t *p);
/* Generate the setpoint curve. Heat-up 1 starts at `start_temp_c` (the measured
 * oven temperature at run start), clamped to [PROFILE_START_MIN_C, soak_temp] so
 * a cold room can't overrun the sample buffer and a warm start can't invert the
 * first ramp. The cool-down tail descends to COOL_DOWN_THRESHOLD_C (where the
 * run completes), not to ambient. */
bool profile_curve_generate(const profile_t *p, float start_temp_c, profile_curve_t *out);
float profile_curve_setpoint(const profile_curve_t *c, uint32_t elapsed_ms);
/* Setpoint slope of the stage active at elapsed_ms; 0 past the end. */
float profile_curve_rate(const profile_curve_t *c, uint32_t elapsed_ms);
void profile_defaults(profile_t *out);

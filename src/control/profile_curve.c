#include "control/profile_curve.h"
#include "config.h"
#include <string.h>

void profile_defaults(profile_t *out) {
    out->first_heatup_rate  = FIRST_HEATUP_DEFAULT;
    out->soak_temp          = SOAK_TEMP_DEFAULT;
    out->soak_time          = SOAK_TIME_DEFAULT;
    out->second_heatup_rate = SECOND_HEATUP_DEFAULT;
    out->reflow_temp        = REFLOW_TEMP_DEFAULT;
    out->reflow_time        = REFLOW_TIME_DEFAULT;
    out->cool_down_rate     = COOL_DOWN_RATE_DEFAULT;
}

bool profile_validate(const profile_t *p) {
    if (p->first_heatup_rate  < HEATUP_RATE_MIN  || p->first_heatup_rate  > HEATUP_RATE_MAX) return false;
    if (p->second_heatup_rate < HEATUP_RATE_MIN  || p->second_heatup_rate > HEATUP_RATE_MAX) return false;
    if (p->cool_down_rate     < COOL_DOWN_RATE_MIN || p->cool_down_rate   > COOL_DOWN_RATE_MAX) return false;
    if (p->soak_temp   < SOAK_TEMP_MIN   || p->soak_temp   > SOAK_TEMP_MAX)   return false;
    if (p->reflow_temp < REFLOW_TEMP_MIN || p->reflow_temp > REFLOW_TEMP_MAX) return false;
    /* soak_time and reflow_time are uint16_t and their MINs are 0, so the
     * lower-bound check is trivially true. ESP-IDF's -Werror=type-limits
     * rejects the redundant compare; the upper-bound check is the meaningful
     * one. If the MINs ever change away from 0, restore the full check.
     */
    if (p->soak_time   > SOAK_TIME_MAX)   return false;
    if (p->reflow_time > REFLOW_TIME_MAX) return false;
    if (p->reflow_temp <= p->soak_temp) return false;
    /* Spec §9 cross-bound: reflow target must leave >=10 C headroom below
     * the over-temp limit. Today this is also enforced transitively via
     * REFLOW_TEMP_MAX + the static_assert in config.h, but checking the
     * runtime value directly is defence-in-depth against future bound
     * edits that drift the transitive guarantee. */
    if ((float)p->reflow_temp > OVER_TEMP_LIMIT_C - 10.0f) return false;
    return true;
}

static uint32_t append_ramp(profile_curve_t *c, float from, float to, float rate) {
    const float step_s = PROFILE_CURVE_STEP_MS / 1000.0f;
    float v = from;
    uint32_t added = 0;
    while (v < to && c->length < PROFILE_CURVE_MAX_SAMPLES) {
        c->samples[c->length++] = v;
        v += rate * step_s;
        added += PROFILE_CURVE_STEP_MS;
    }
    /* Emit the target value as the final ramp sample so the curve is
     * continuous across phase boundaries (the next phase starts at `to`). */
    if (c->length < PROFILE_CURVE_MAX_SAMPLES) {
        c->samples[c->length++] = to;
        added += PROFILE_CURVE_STEP_MS;
    }
    return added;
}

static uint32_t append_hold(profile_curve_t *c, float value, uint32_t duration_ms) {
    uint32_t added = 0;
    while (added < duration_ms && c->length < PROFILE_CURVE_MAX_SAMPLES) {
        c->samples[c->length++] = value;
        added += PROFILE_CURVE_STEP_MS;
    }
    return added;
}

/* Mirror of append_ramp for a descending ramp (used for the cool-down
 * tail). `rate` is the magnitude in °C/s. */
static uint32_t append_ramp_down(profile_curve_t *c, float from, float to, float rate) {
    const float step_s = PROFILE_CURVE_STEP_MS / 1000.0f;
    float v = from;
    uint32_t added = 0;
    while (v > to && c->length < PROFILE_CURVE_MAX_SAMPLES) {
        c->samples[c->length++] = v;
        v -= rate * step_s;
        added += PROFILE_CURVE_STEP_MS;
    }
    /* Emit the target value as the final cool-down sample. */
    if (c->length < PROFILE_CURVE_MAX_SAMPLES) {
        c->samples[c->length++] = to;
        added += PROFILE_CURVE_STEP_MS;
    }
    return added;
}

bool profile_curve_generate(const profile_t *p, float start_temp_c, profile_curve_t *out) {
    memset(out, 0, sizeof(*out));
    if (!profile_validate(p)) return false;

    /* Clamp the ramp-1 start: the floor bounds ramp 1 within the sample buffer
     * (see PROFILE_CURVE_MAX_SAMPLES); capping at soak_temp keeps ramp 1 from
     * inverting if the oven starts at/above the soak target. */
    float start = start_temp_c;
    if (start < PROFILE_START_MIN_C)     start = PROFILE_START_MIN_C;
    if (start > (float)p->soak_temp)     start = (float)p->soak_temp;

    uint32_t t = 0;

    t += append_ramp(out, start, (float)p->soak_temp, p->first_heatup_rate);
    out->phase_end_ms[STAGE_HEAT_UP_1] = t;

    t += append_hold(out, (float)p->soak_temp, p->soak_time * 1000u);
    out->phase_end_ms[STAGE_SOAK] = t;

    t += append_ramp(out, (float)p->soak_temp, (float)p->reflow_temp, p->second_heatup_rate);
    out->phase_end_ms[STAGE_HEAT_UP_2] = t;

    t += append_hold(out, (float)p->reflow_temp, p->reflow_time * 1000u);
    out->phase_end_ms[STAGE_REFLOW] = t;

    /* Cool-down: ramp from reflow_temp down to COOL_DOWN_THRESHOLD_C — the
     * temperature where the run completes — at the typical convection rate.
     * Visual + setpoint completeness; the state machine still exits cool-down
     * on temp/timeout. May truncate at PROFILE_CURVE_MAX_SAMPLES for extreme
     * worst-case profiles — that's acceptable, the curve just falls short. */
    t += append_ramp_down(out, (float)p->reflow_temp, COOL_DOWN_THRESHOLD_C,
                          p->cool_down_rate);
    out->phase_end_ms[STAGE_COOL_DOWN] = t;

    /* Per-stage setpoint slopes for feedforward (spec 2026-07-19 §2). The
     * memset above zeroed NONE/SOAK/REFLOW. Cool-down is negative on
     * purpose (truthful sign; ff consumers clamp at 0). */
    out->stage_rate[STAGE_HEAT_UP_1] = p->first_heatup_rate;
    out->stage_rate[STAGE_HEAT_UP_2] = p->second_heatup_rate;
    out->stage_rate[STAGE_COOL_DOWN] = -p->cool_down_rate;

    return true;
}

float profile_curve_setpoint(const profile_curve_t *c, uint32_t elapsed_ms) {
    if (c->length == 0) return PROFILE_START_DEFAULT_C;
    size_t idx = elapsed_ms / PROFILE_CURVE_STEP_MS;
    if (idx >= c->length) idx = c->length - 1;
    return c->samples[idx];
}

float profile_curve_rate(const profile_curve_t *c, uint32_t elapsed_ms) {
    for (int s = STAGE_HEAT_UP_1; s < STAGE_COUNT_; s++) {
        if (elapsed_ms < c->phase_end_ms[s]) return c->stage_rate[s];
    }
    return 0.0f;
}

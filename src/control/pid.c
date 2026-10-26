#include "control/pid.h"
#include "config.h"     /* PID_D_FILTER_TAU_S, AMBIENT_REF_*_C */
#include <assert.h>

static float clamp(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

void pid_init(pid_state_t *s, const pid_gains_t *g) {
    s->g = *g;
    s->integrator  = 0.0f;
    s->prev_temp   = 0.0f;
    s->d_filt      = 0.0f;
    s->ambient_ref = AMBIENT_REF_MIN_C;
}

void pid_set_gains(pid_state_t *s, const pid_gains_t *g) {
    s->g = *g;
}

void pid_reset(pid_state_t *s, float current_temp) {
    s->integrator  = 0.0f;
    s->prev_temp   = current_temp;
    s->d_filt      = 0.0f;
    /* Clamped so a still-warm oven doesn't zero out the hold model —
     * losses scale with temp above ROOM, not above start temp (spec §1). */
    s->ambient_ref = clamp(current_temp, AMBIENT_REF_MIN_C, AMBIENT_REF_MAX_C);
}

float pid_step(pid_state_t *s, float setpoint, float rate, float temp,
               float dt, float out_max) {
    assert(dt > 0.0f);
    /* An out-of-range ceiling is a caller bug with a nasty tail: clamp() would
     * return out_max itself for any raw above it, so a zero or negative ceiling
     * yields a negative duty, and control_task's (uint8_t)out conversion of a
     * negative float is undefined behaviour rather than a benign wrap. Catch it
     * here, at the point of misuse. Note this does NOT catch a transposed
     * dt/out_max pair -- both would still pass their own checks. */
    assert(out_max > PID_OUTPUT_MIN && out_max <= PID_OUTPUT_MAX);
    const float error       = setpoint - temp;
    const float p_term      = s->g.kp * error;
    /* Derivative on measurement, low-pass filtered. The raw sample-to-sample
     * slope carries the full thermocouple noise straight to the SSR; a
     * first-order LPF (tau = PID_D_FILTER_TAU_S) attenuates the per-tick noise
     * while passing the slow thermal derivative. tau = 0 degenerates to the
     * unfiltered derivative (alpha = 1). */
    const float d_raw       = -s->g.kd * (temp - s->prev_temp) / dt;
    const float alpha       = dt / (PID_D_FILTER_TAU_S + dt);
    s->d_filt              += alpha * (d_raw - s->d_filt);
    const float d_term      = s->d_filt;
    /* Feedforward (spec §1): planned-trajectory duty from the plant model.
     * Both terms clamp at 0 — ff may never subtract duty (cool-down rate is
     * negative; a setpoint below ambient_ref is outside the loss model). */
    float ff_hold = s->g.k_hold * (setpoint - s->ambient_ref);
    if (ff_hold < 0.0f) ff_hold = 0.0f;
    const float ff          = ff_hold + s->g.k_rate * (rate > 0.0f ? rate : 0.0f);
    const float i_tentative = s->integrator + s->g.ki * error * dt;
    const float raw         = ff + p_term + i_tentative + d_term;

    float output;
    if (raw > out_max && error > 0.0f) {
        output = out_max;               /* freeze integrator */
    } else if (raw < PID_OUTPUT_MIN && error < 0.0f) {
        output = PID_OUTPUT_MIN;        /* freeze integrator */
    } else {
        s->integrator = i_tentative;
        output = clamp(raw, PID_OUTPUT_MIN, out_max);
    }

    s->prev_temp = temp;
    return output;
}

/* Standard-form PID with conditional-integration anti-windup, plus
 * model-based feedforward (spec 2026-07-19 §1):
 *     ff = k_hold·max(setpoint − ambient_ref, 0) + k_rate·max(rate, 0)
 * ff is summed INSIDE pid_step so the anti-windup saturation test sees the
 * true output. Derivative on measurement (not error) to avoid kick on
 * setpoint steps. See spec §4.1.
 */
#pragma once

/* Gains + feedforward coefficients. Serialised to NVS by storage/nvs_schema
 * (blob v2) and carried end-to-end: /suggest-gains → confirm → apply. */
typedef struct {
    float kp;       /* %/°C   */
    float ki;       /* 1/s    (Ki = Kp/Ti) */
    float kd;       /* s      (Kd = Kp·Td) */
    float k_hold;   /* %/°C   — holding duty per °C above ambient_ref */
    float k_rate;   /* %·s/°C — duty per °C/s of commanded ramp (1/R) */
} pid_gains_t;

typedef struct {
    pid_gains_t g;
    float integrator;
    float prev_temp;
    float d_filt;       /* low-pass-filtered derivative term (see PID_D_FILTER_TAU_S) */
    float ambient_ref;  /* captured by pid_reset, clamped to AMBIENT_REF_*_C */
} pid_state_t;

/* Output range is [0, 100] (percent duty). */
#define PID_OUTPUT_MIN  0.0f
#define PID_OUTPUT_MAX  100.0f

void  pid_init(pid_state_t *s, const pid_gains_t *g);
void  pid_set_gains(pid_state_t *s, const pid_gains_t *g);

/* IDLE→RUNNING transition. MUST be called between pid_init and the first
 * pid_step — pid_init leaves prev_temp = 0, so a stray pid_step without a
 * preceding pid_reset would compute a huge spurious derivative spike.
 * Also captures ambient_ref for the ff hold term. */
void  pid_reset(pid_state_t *s, float current_temp);

/* rate: commanded setpoint slope (°C/s), from profile_curve_rate() or the
 * drying ramp rate; negative values (cool-down) are treated as 0. dt must be
 * > 0 (asserted). out_max is the duty ceiling in percent — reflow passes
 * PID_OUTPUT_MAX, drying passes DRY_DUTY_MAX_PERCENT. It participates in the
 * anti-windup saturation test, so a cap applied by the caller instead would
 * wind the integrator up invisibly. */
float pid_step(pid_state_t *s, float setpoint, float rate, float temp,
               float dt, float out_max);

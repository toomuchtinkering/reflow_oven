/* src/drivers/touch_cal_math.h — 5-point affine touch calibration math.
 * Pure C, no ESP-IDF or LVGL dependencies. Tested under [env:native].
 *
 * Model: display = M * raw + offset, solved independently per axis:
 *     x_disp = ax*x_raw + bx*y_raw + cx
 *     y_disp = ay*x_raw + by*y_raw + cy
 *
 * Inputs are 5 (raw, display) pairs; outputs are the 6 coefficients
 * plus a fit-quality summary (max + RMS residual in display pixels).
 */
#pragma once

#include <stdbool.h>

#define TOUCH_CAL_N_POINTS 5

typedef struct {
    float ax, bx, cx;   /* x_disp = ax*x_raw + bx*y_raw + cx */
    float ay, by, cy;   /* y_disp = ay*x_raw + by*y_raw + cy */
} touch_cal_matrix_t;

typedef struct {
    float max_residual_px;
    float rms_residual_px;
} touch_cal_residual_t;

/* Solve the over-determined system by least squares (3x3 normal equations,
 * Gaussian elimination with partial pivoting, float precision).
 * Returns false only if the system is singular (collinear points) —
 * never expected with the spec's 5 corner+center targets.
 *
 *   raw_x[i], raw_y[i] : 5 raw ADC samples (per axis)
 *   disp_x[i], disp_y[i] : 5 known display-pixel targets
 *   out_m : fitted coefficients
 */
bool touch_cal_solve(const float raw_x[TOUCH_CAL_N_POINTS],
                     const float raw_y[TOUCH_CAL_N_POINTS],
                     const float disp_x[TOUCH_CAL_N_POINTS],
                     const float disp_y[TOUCH_CAL_N_POINTS],
                     touch_cal_matrix_t *out_m);

/* Evaluate the fitted matrix at each of the 5 raw points and report the
 * max / RMS Euclidean error (in display pixels) vs. the known targets. */
void touch_cal_residual(const float raw_x[TOUCH_CAL_N_POINTS],
                        const float raw_y[TOUCH_CAL_N_POINTS],
                        const float disp_x[TOUCH_CAL_N_POINTS],
                        const float disp_y[TOUCH_CAL_N_POINTS],
                        const touch_cal_matrix_t *m,
                        touch_cal_residual_t *out_r);

/* Apply the matrix to a single raw point. Used both by the residual
 * computation (above) and by the main firmware's touch driver at runtime.
 * No clamping — the caller decides what to do with out-of-display outputs. */
void touch_cal_apply(const touch_cal_matrix_t *m,
                     float raw_x, float raw_y,
                     float *out_disp_x, float *out_disp_y);

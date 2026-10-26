/* src/drivers/touch_cal_math.c — 5-point affine touch calibration math.
 * Pure C; no ESP-IDF / LVGL deps. See header for model.
 *
 * The X-axis fit and Y-axis fit decouple, so we run the same 3x3 solver
 * twice on the same accumulators, swapping only the RHS vector.
 */
#include "drivers/touch_cal_math.h"

#include <math.h>
#include <string.h>

/* Solve A * x = b for a 3-vector x. A is row-major 3x3, modified in place.
 * Gaussian elimination with partial pivoting, float precision.
 * Returns false if A is singular — detected via |det(A)| vs. a relative
 * scale derived from the matrix's max entry. (A pure pivot-magnitude
 * check doesn't separate well-conditioned from collinear inputs reliably
 * in float arithmetic on this matrix family.) */
static bool solve3x3(float A[3][3], float b[3], float out[3])
{
    float max_abs = 0.0f;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            float v = fabsf(A[i][j]);
            if (v > max_abs) max_abs = v;
        }
    }
    if (max_abs == 0.0f) return false;

    /* det(A) for a 3x3 (computed before in-place destruction). */
    float det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1])
              - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0])
              + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
    /* Empirically: for the spec's 5 corner+center targets, |det|/max_abs^3
     * is on the order of 1e-8 (well-conditioned) or exactly 0 (collinear).
     * 1e-10 leaves two orders of magnitude of safety margin against any
     * plausible well-conditioned tap, and still catches true singularity. */
    float scale = max_abs * max_abs * max_abs;
    if (fabsf(det) < scale * 1e-10f) return false;

    for (int col = 0; col < 3; ++col) {
        /* partial pivot: find the row with the largest |A[row][col]|.
         * Singularity is already ruled out by the det check above, so we
         * only need pivoting here for numerical stability. */
        int pivot_row = col;
        float pivot_abs = fabsf(A[col][col]);
        for (int row = col + 1; row < 3; ++row) {
            float v = fabsf(A[row][col]);
            if (v > pivot_abs) { pivot_abs = v; pivot_row = row; }
        }

        if (pivot_row != col) {
            for (int k = 0; k < 3; ++k) {
                float tmp = A[col][k]; A[col][k] = A[pivot_row][k]; A[pivot_row][k] = tmp;
            }
            float tmp = b[col]; b[col] = b[pivot_row]; b[pivot_row] = tmp;
        }

        /* eliminate below */
        for (int row = col + 1; row < 3; ++row) {
            float factor = A[row][col] / A[col][col];
            for (int k = col; k < 3; ++k) {
                A[row][k] -= factor * A[col][k];
            }
            b[row] -= factor * b[col];
        }
    }

    /* back-substitution */
    for (int row = 2; row >= 0; --row) {
        float s = b[row];
        for (int k = row + 1; k < 3; ++k) s -= A[row][k] * out[k];
        out[row] = s / A[row][row];
    }
    return true;
}

/* Build the normal-equation accumulators that are shared between the two
 * axis fits, then call solve3x3 twice with different RHS vectors. */
bool touch_cal_solve(const float raw_x[TOUCH_CAL_N_POINTS],
                     const float raw_y[TOUCH_CAL_N_POINTS],
                     const float disp_x[TOUCH_CAL_N_POINTS],
                     const float disp_y[TOUCH_CAL_N_POINTS],
                     touch_cal_matrix_t *out_m)
{
    float sum_xx = 0, sum_xy = 0, sum_yy = 0, sum_x = 0, sum_y = 0;
    float sum_x_Xd = 0, sum_y_Xd = 0, sum_Xd = 0;
    float sum_x_Yd = 0, sum_y_Yd = 0, sum_Yd = 0;

    for (int i = 0; i < TOUCH_CAL_N_POINTS; ++i) {
        const float x = raw_x[i];
        const float y = raw_y[i];
        sum_xx += x * x;
        sum_xy += x * y;
        sum_yy += y * y;
        sum_x  += x;
        sum_y  += y;
        sum_x_Xd += x * disp_x[i];
        sum_y_Xd += y * disp_x[i];
        sum_Xd   += disp_x[i];
        sum_x_Yd += x * disp_y[i];
        sum_y_Yd += y * disp_y[i];
        sum_Yd   += disp_y[i];
    }

    /* X-axis fit. */
    float A[3][3] = {
        { sum_xx, sum_xy, sum_x },
        { sum_xy, sum_yy, sum_y },
        { sum_x,  sum_y,  (float)TOUCH_CAL_N_POINTS },
    };
    float bx_rhs[3] = { sum_x_Xd, sum_y_Xd, sum_Xd };
    float coeffs[3];
    if (!solve3x3(A, bx_rhs, coeffs)) return false;
    out_m->ax = coeffs[0];
    out_m->bx = coeffs[1];
    out_m->cx = coeffs[2];

    /* Rebuild A — solve3x3 destroyed it. */
    float A2[3][3] = {
        { sum_xx, sum_xy, sum_x },
        { sum_xy, sum_yy, sum_y },
        { sum_x,  sum_y,  (float)TOUCH_CAL_N_POINTS },
    };
    float by_rhs[3] = { sum_x_Yd, sum_y_Yd, sum_Yd };
    if (!solve3x3(A2, by_rhs, coeffs)) return false;
    out_m->ay = coeffs[0];
    out_m->by = coeffs[1];
    out_m->cy = coeffs[2];
    return true;
}

void touch_cal_apply(const touch_cal_matrix_t *m,
                     float raw_x, float raw_y,
                     float *out_disp_x, float *out_disp_y)
{
    *out_disp_x = m->ax * raw_x + m->bx * raw_y + m->cx;
    *out_disp_y = m->ay * raw_x + m->by * raw_y + m->cy;
}

void touch_cal_residual(const float raw_x[TOUCH_CAL_N_POINTS],
                        const float raw_y[TOUCH_CAL_N_POINTS],
                        const float disp_x[TOUCH_CAL_N_POINTS],
                        const float disp_y[TOUCH_CAL_N_POINTS],
                        const touch_cal_matrix_t *m,
                        touch_cal_residual_t *out_r)
{
    float max_sq = 0.0f;
    float sum_sq = 0.0f;
    for (int i = 0; i < TOUCH_CAL_N_POINTS; ++i) {
        float px, py;
        touch_cal_apply(m, raw_x[i], raw_y[i], &px, &py);
        float dx = px - disp_x[i];
        float dy = py - disp_y[i];
        float err_sq = dx * dx + dy * dy;
        sum_sq += err_sq;
        if (err_sq > max_sq) max_sq = err_sq;
    }
    out_r->max_residual_px = sqrtf(max_sq);
    out_r->rms_residual_px = sqrtf(sum_sq / (float)TOUCH_CAL_N_POINTS);
}

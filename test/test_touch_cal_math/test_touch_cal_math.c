/* test/test_touch_cal_math/test_touch_cal_math.c
 * Drives the affine solver with synthetic ground truth: invent a matrix,
 * generate the raw points that the 5 known display targets would map to,
 * feed those raws back into the solver, expect to recover the matrix.
 */
#include <unity.h>
#include <math.h>
#include "drivers/touch_cal_math.h"

void setUp(void)    {}
void tearDown(void) {}

/* Known display targets (spec §5.2). */
static const float DISP_X[5] = {  32.0f, 288.0f,  32.0f, 288.0f, 160.0f };
static const float DISP_Y[5] = {  48.0f,  48.0f, 432.0f, 432.0f, 240.0f };

/* Ground-truth matrix:
 *   ax=0.075, bx=0,    cx=10
 *   ay=0,     by=-0.125, cy=510
 * This is invertible (det = ax*by - bx*ay = -0.009375), so we can compute
 * the unique raw_x, raw_y that would map to each (disp_x, disp_y). */
static void invent_raw_points(float raw_x[5], float raw_y[5]) {
    const float ax = 0.075f, bx = 0.0f, cx = 10.0f;
    const float ay = 0.0f,   by = -0.125f, cy = 510.0f;
    const float det = ax * by - bx * ay;
    for (int i = 0; i < 5; ++i) {
        /* Solve a 2x2 system per point: raw = M^-1 * (disp - offset). */
        float dx = DISP_X[i] - cx;
        float dy = DISP_Y[i] - cy;
        raw_x[i] = ( by * dx - bx * dy) / det;
        raw_y[i] = (-ay * dx + ax * dy) / det;
    }
}

void test_solve_recovers_ground_truth_matrix(void) {
    float raw_x[5], raw_y[5];
    invent_raw_points(raw_x, raw_y);

    touch_cal_matrix_t m = {0};
    bool ok = touch_cal_solve(raw_x, raw_y, DISP_X, DISP_Y, &m);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f,  0.075f, m.ax);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f,  0.0f,   m.bx);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f,  10.0f,  m.cx);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f,  0.0f,   m.ay);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -0.125f, m.by);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f,  510.0f, m.cy);
}

void test_residuals_near_zero_for_exact_fit(void) {
    float raw_x[5], raw_y[5];
    invent_raw_points(raw_x, raw_y);

    touch_cal_matrix_t m;
    TEST_ASSERT_TRUE(touch_cal_solve(raw_x, raw_y, DISP_X, DISP_Y, &m));

    touch_cal_residual_t r;
    touch_cal_residual(raw_x, raw_y, DISP_X, DISP_Y, &m, &r);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, r.max_residual_px);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, r.rms_residual_px);
}

void test_solve_returns_false_on_singular_system(void) {
    /* All 5 raw points collinear (same y, varying x) -> the y-column of the
     * normal-equations matrix is degenerate, so the solver must return false
     * instead of silently producing nonsense coefficients. */
    float raw_x[5] = { 100, 200, 300, 400, 500 };
    float raw_y[5] = { 250, 250, 250, 250, 250 };

    touch_cal_matrix_t m = {0};
    bool ok = touch_cal_solve(raw_x, raw_y, DISP_X, DISP_Y, &m);
    TEST_ASSERT_FALSE(ok);
}

void test_residuals_detect_one_bad_point(void) {
    /* Perturb the last raw point by an amount the matrix can't absorb;
     * the solver minimises total squared error, so residuals stay above 0. */
    float raw_x[5], raw_y[5];
    invent_raw_points(raw_x, raw_y);
    raw_x[4] += 50.0f;   /* shift the center point's raw x by 50 ADC counts */

    touch_cal_matrix_t m;
    TEST_ASSERT_TRUE(touch_cal_solve(raw_x, raw_y, DISP_X, DISP_Y, &m));

    touch_cal_residual_t r;
    touch_cal_residual(raw_x, raw_y, DISP_X, DISP_Y, &m, &r);

    TEST_ASSERT_GREATER_THAN_FLOAT(0.5f, r.max_residual_px);
    TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, r.rms_residual_px);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_solve_recovers_ground_truth_matrix);
    RUN_TEST(test_residuals_near_zero_for_exact_fit);
    RUN_TEST(test_solve_returns_false_on_singular_system);
    RUN_TEST(test_residuals_detect_one_bad_point);
    return UNITY_END();
}

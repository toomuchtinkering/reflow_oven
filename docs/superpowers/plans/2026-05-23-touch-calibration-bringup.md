# 08_touch_cal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a standalone bring-up tool (`tools/bringup/08_touch_cal`) that runs a 5-point XPT2046 → display calibration sequence on the LCDwiki E32 board, computes an affine matrix by least-squares, validates it against a residual threshold, and prints config.h-pasteable constants to UART.

**Architecture:** Single ESP-IDF / PlatformIO project under `tools/bringup/08_touch_cal/`, modelled on the existing `04_lvgl_hello`. The pure-C math layer (Gaussian elimination on the 3×3 normal equations, residual computation) lives in `src/drivers/touch_cal_math.{c,h}` so it can be exercised by Unity under the existing `[env:native]` PlatformIO env. The bring-up's `main.c` cross-references that file via an extra `INCLUDE_DIRS` / `SRCS` entry — same source, two build targets. Everything else (SPI bus, ST7796 panel, XPT2046 chip, LVGL via `esp_lvgl_port`, crosshair drawing, state machine) is local to the bring-up's `main.c`.

**Tech Stack:** ESP-IDF 5.x, PlatformIO, LVGL 9 via `esp_lvgl_port`, `esp_lcd_st7796`, `atanisoft/esp_lcd_touch_xpt2046`, Unity (host-side tests in `[env:native]`), pure C.

---

## File Structure

**New files:**
- `src/drivers/touch_cal_math.h` — pure-C math API (5-point affine fit, residual)
- `src/drivers/touch_cal_math.c` — implementation
- `test/test_touch_cal_math/test_touch_cal_math.c` — Unity test for the math
- `tools/bringup/08_touch_cal/CMakeLists.txt` — IDF project root
- `tools/bringup/08_touch_cal/platformio.ini` — PlatformIO env
- `tools/bringup/08_touch_cal/sdkconfig.esp32dev` — copied from `04_lvgl_hello`, unchanged
- `tools/bringup/08_touch_cal/src/CMakeLists.txt` — component registration (pulls in the shared math file)
- `tools/bringup/08_touch_cal/src/idf_component.yml` — LVGL / esp_lvgl_port / panel / touch components
- `tools/bringup/08_touch_cal/src/main.c` — init, state machine, UI

**Modified files:** none. `src/drivers/touch.c` is unchanged in this plan; the spec's §7 follow-up will modify it later.

**Spec reference:** [docs/superpowers/specs/2026-05-23-touch-calibration-bringup-design.md](../specs/2026-05-23-touch-calibration-bringup-design.md)

---

## Task 1: Math API header

**Files:**
- Create: `src/drivers/touch_cal_math.h`

- [ ] **Step 1: Write the header**

Create `src/drivers/touch_cal_math.h`:

```c
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
```

- [ ] **Step 2: Commit**

```bash
git add src/drivers/touch_cal_math.h
git commit -m "touch_cal_math: declare pure-C affine-fit API"
```

---

## Task 2: Unity test for the math (failing)

**Files:**
- Create: `test/test_touch_cal_math/test_touch_cal_math.c`

- [ ] **Step 1: Write the failing test**

Create `test/test_touch_cal_math/test_touch_cal_math.c`:

```c
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
    RUN_TEST(test_residuals_detect_one_bad_point);
    return UNITY_END();
}
```

- [ ] **Step 2: Run the test — expect failure**

```bash
cd ~/reflow_oven
pio test -e native -f test_touch_cal_math
```

Expected: a linker error along the lines of
`undefined reference to 'touch_cal_solve'` / `'touch_cal_residual'`. (No implementation exists yet.)

- [ ] **Step 3: Commit the failing test**

```bash
git add test/test_touch_cal_math/test_touch_cal_math.c
git commit -m "test: failing unit test for 5-point affine touch calibration"
```

---

## Task 3: Implement the math, make the test pass

**Files:**
- Create: `src/drivers/touch_cal_math.c`

- [ ] **Step 1: Write the implementation**

Create `src/drivers/touch_cal_math.c`:

```c
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
 * Returns false if A is singular (max pivot <= eps). */
static bool solve3x3(float A[3][3], float b[3], float out[3])
{
    const float eps = 1e-9f;
    for (int col = 0; col < 3; ++col) {
        /* partial pivot: find the row with the largest |A[row][col]| */
        int pivot_row = col;
        float pivot_abs = fabsf(A[col][col]);
        for (int row = col + 1; row < 3; ++row) {
            float v = fabsf(A[row][col]);
            if (v > pivot_abs) { pivot_abs = v; pivot_row = row; }
        }
        if (pivot_abs < eps) return false;

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
        float px = m->ax * raw_x[i] + m->bx * raw_y[i] + m->cx;
        float py = m->ay * raw_x[i] + m->by * raw_y[i] + m->cy;
        float dx = px - disp_x[i];
        float dy = py - disp_y[i];
        float err_sq = dx * dx + dy * dy;
        sum_sq += err_sq;
        if (err_sq > max_sq) max_sq = err_sq;
    }
    out_r->max_residual_px = sqrtf(max_sq);
    out_r->rms_residual_px = sqrtf(sum_sq / (float)TOUCH_CAL_N_POINTS);
}
```

- [ ] **Step 2: Run the test — expect pass**

```bash
pio test -e native -f test_touch_cal_math
```

Expected output ends with:
```
3 Tests 0 Failures 0 Ignored
OK
```

- [ ] **Step 3: Commit**

```bash
git add src/drivers/touch_cal_math.c
git commit -m "touch_cal_math: implement 5-point affine fit via normal equations"
```

---

## Task 4: Bring-up project scaffold (compiles, init only)

**Files:**
- Create: `tools/bringup/08_touch_cal/CMakeLists.txt`
- Create: `tools/bringup/08_touch_cal/platformio.ini`
- Create: `tools/bringup/08_touch_cal/sdkconfig.esp32dev`
- Create: `tools/bringup/08_touch_cal/src/CMakeLists.txt`
- Create: `tools/bringup/08_touch_cal/src/idf_component.yml`
- Create: `tools/bringup/08_touch_cal/src/main.c`

- [ ] **Step 1: Copy the sdkconfig from 04_lvgl_hello (unchanged)**

```bash
cd ~/reflow_oven
mkdir -p tools/bringup/08_touch_cal/src
cp tools/bringup/04_lvgl_hello/sdkconfig.esp32dev tools/bringup/08_touch_cal/sdkconfig.esp32dev
```

- [ ] **Step 2: Write the IDF project root**

Create `tools/bringup/08_touch_cal/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(08_touch_cal)
```

- [ ] **Step 3: Write the PlatformIO env**

Create `tools/bringup/08_touch_cal/platformio.ini`:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = espidf
monitor_speed = 115200
build_flags =
    -I../../../include            ; reach the main config.h
    -I../../../src                ; reach drivers/touch_cal_math.h
```

- [ ] **Step 4: Write the component registration (with cross-dir math source)**

Create `tools/bringup/08_touch_cal/src/CMakeLists.txt`:

```cmake
# Local sources in this bring-up + the shared pure-C calibration math from
# the main src tree. Pulling the math from src/drivers keeps it in sync
# with the [env:native] unit test (test/test_touch_cal_math/) — same file,
# two build targets.
FILE(GLOB_RECURSE local_sources ${CMAKE_SOURCE_DIR}/src/*.c)
set(shared_sources "${CMAKE_SOURCE_DIR}/../../../src/drivers/touch_cal_math.c")

idf_component_register(
    SRCS ${local_sources} ${shared_sources}
    INCLUDE_DIRS "." "../../../src" "../../../include"
)
```

- [ ] **Step 5: Write the component manifest**

Create `tools/bringup/08_touch_cal/src/idf_component.yml`:

```yaml
dependencies:
  idf: ">=5.0"
  lvgl/lvgl: "^9"
  espressif/esp_lvgl_port: "^2"
  espressif/esp_lcd_st7796: "^1"
  atanisoft/esp_lcd_touch_xpt2046: "^1"   # not in the espressif/ namespace
```

- [ ] **Step 6: Write the minimal main.c (init only — full flow added in Task 5)**

Create `tools/bringup/08_touch_cal/src/main.c`:

```c
/* tools/bringup/08_touch_cal/src/main.c
 * 5-point XPT2046 touch-screen calibration bring-up.
 *
 * Pipeline:
 *   SPI bus -> ST7796 panel -> XPT2046 touch -> LVGL via esp_lvgl_port
 *   then a FreeRTOS task drives the state machine that walks 5 crosshair
 *   targets, captures medians of the raw ADC, fits an affine matrix, and
 *   logs config.h-pasteable constants to UART on success.
 *
 * Spec: docs/superpowers/specs/2026-05-23-touch-calibration-bringup-design.md
 *
 * NOTE on touch flags: unlike src/drivers/touch.c (mirror_y = 1), this
 * bring-up sets ALL mirror/swap flags to 0 and x_max=y_max=4095 so the
 * fitted matrix maps raw 12-bit ADC counts directly to display pixels.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_xpt2046.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "lvgl.h"

#include "config.h"
#include "drivers/touch_cal_math.h"

static const char *TAG = "touch_cal";

/* Set up SPI + ST7796 + XPT2046 + LVGL. Returns the touch handle so the
 * calibration task can read raw samples directly (bypassing the LVGL indev,
 * which would force the data through display-pixel normalisation). */
static esp_lcd_touch_handle_t bringup_init(lv_display_t **out_disp)
{
    /* === LCD SPI bus, shared with touch === */
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 320 * 48 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* === LCD panel IO + driver === */
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS, .dc_gpio_num = PIN_LCD_DC, .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000, .trans_queue_depth = 10,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    gpio_config_t bl = {.pin_bit_mask = 1ULL << PIN_LCD_BL, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&bl);
    gpio_set_level(PIN_LCD_BL, 1);

    /* === Touch — ALL flags off, raw ADC range. Spec §5.1. === */
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(PIN_TOUCH_CS);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &tp_io_cfg, &tp_io));
    esp_lcd_touch_handle_t tp = NULL;
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = 4095, .y_max = 4095,
        .rst_gpio_num = -1, .int_gpio_num = -1,  /* GPIO 36, no pull-up */
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &tp));

    /* === LVGL via esp_lvgl_port === */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = 320 * 48,
        .double_buffer = true,
        .hres = 320, .vres = 480,
        .monochrome = false,
        .rotation = {.swap_xy = false, .mirror_x = true, .mirror_y = false},
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    *out_disp = lvgl_port_add_disp(&disp_cfg);

    /* IMPORTANT: do NOT lvgl_port_add_touch() here. We don't want LVGL
     * pumping the indev — we read the raw chip directly from cal_task.
     * (Adding a touch indev would also force the data through display-
     * pixel scaling, which is precisely what we're trying to calibrate.) */

    return tp;
}

void app_main(void)
{
    lv_display_t *disp = NULL;
    esp_lcd_touch_handle_t tp = bringup_init(&disp);
    (void)tp; (void)disp;
    ESP_LOGI(TAG, "init OK — calibration flow added in next task");
}
```

- [ ] **Step 7: Build — expect success**

```bash
cd ~/reflow_oven/tools/bringup/08_touch_cal
pio run -e esp32dev
```

Expected: compiles cleanly, ends with `SUCCESS`. (Flashing is optional at this stage — no UI yet to observe.)

- [ ] **Step 8: Commit**

```bash
cd ~/reflow_oven
git add tools/bringup/08_touch_cal/
git commit -m "08_touch_cal: scaffold bring-up project (init only, no UI yet)"
```

---

## Task 5: State machine, crosshair drawing, capture, fit, UART output

This task makes the bring-up actually do calibration. It's a single chunky task because the state machine, the UI, the capture loop, and the result rendering are tightly coupled — splitting them would leave intermediate states that the engineer can't usefully verify on hardware.

**Files:**
- Modify: `tools/bringup/08_touch_cal/src/main.c`

- [ ] **Step 1: Replace main.c with the full implementation**

Open `tools/bringup/08_touch_cal/src/main.c` and replace it entirely with:

```c
/* tools/bringup/08_touch_cal/src/main.c
 * 5-point XPT2046 touch-screen calibration bring-up.
 *
 * Pipeline:
 *   SPI bus -> ST7796 panel -> XPT2046 touch -> LVGL via esp_lvgl_port
 *   then a FreeRTOS task drives the state machine that walks 5 crosshair
 *   targets, captures medians of the raw ADC, fits an affine matrix, and
 *   logs config.h-pasteable constants to UART on success.
 *
 * Spec: docs/superpowers/specs/2026-05-23-touch-calibration-bringup-design.md
 *
 * NOTE on touch flags: unlike src/drivers/touch.c (mirror_y = 1), this
 * bring-up sets ALL mirror/swap flags to 0 and x_max=y_max=4095 so the
 * fitted matrix maps raw 12-bit ADC counts directly to display pixels.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_xpt2046.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>

#include "config.h"
#include "drivers/touch_cal_math.h"

static const char *TAG = "touch_cal";

/* ---------- Target layout (spec §5.2). ---------- */
static const float TARGET_X[TOUCH_CAL_N_POINTS] = {  32.0f, 288.0f,  32.0f, 288.0f, 160.0f };
static const float TARGET_Y[TOUCH_CAL_N_POINTS] = {  48.0f,  48.0f, 432.0f, 432.0f, 240.0f };

/* ---------- Pass thresholds (spec §5.5). ---------- */
#define PASS_MAX_RESIDUAL_PX   5.0f
#define PASS_RMS_RESIDUAL_PX   3.0f

/* ---------- Sampling parameters (spec §5.3). ---------- */
#define SAMPLE_SETTLE_MS         50
#define SAMPLE_COUNT             16
#define SAMPLE_INTERVAL_MS       10
#define SAMPLE_MAX_RANGE_ADC    200   /* per-axis (max-min) acceptance window */

/* ---------- Colours. ---------- */
#define COLOR_BG       0x0f1115
#define COLOR_FG       0xE6E6E6
#define COLOR_DIM      0x808080
#define COLOR_TARGET   0xFF8C42
#define COLOR_ERROR    0xFF4040

/* ---------- LVGL handles, kept around so we can wipe + redraw. ---------- */
static esp_lcd_touch_handle_t s_touch = NULL;
static lv_obj_t              *s_screen = NULL;

/* ---------- UI helpers ---------- */

static void clear_screen(void)
{
    lvgl_port_lock(0);
    lv_obj_clean(s_screen);
    lvgl_port_unlock();
}

static lv_obj_t *add_label(lv_align_t align, lv_coord_t y_off, uint32_t color, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    lv_obj_t *lbl = lv_label_create(s_screen);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_align(lbl, align, 0, y_off);
    return lbl;
}

static void show_instructions(void)
{
    clear_screen();
    lvgl_port_lock(0);
    add_label(LV_ALIGN_TOP_MID,    60,  COLOR_FG,  "Touch Calibration");
    add_label(LV_ALIGN_TOP_MID,   140,  COLOR_FG,  "Tap each crosshair as");
    add_label(LV_ALIGN_TOP_MID,   170,  COLOR_FG,  "precisely as you can.");
    add_label(LV_ALIGN_TOP_MID,   200,  COLOR_FG,  "Hold still until it");
    add_label(LV_ALIGN_TOP_MID,   230,  COLOR_FG,  "disappears, then move");
    add_label(LV_ALIGN_TOP_MID,   260,  COLOR_FG,  "to the next one.");
    add_label(LV_ALIGN_BOTTOM_MID, -80, COLOR_DIM, "Tap anywhere to start");
    lvgl_port_unlock();
}

/* Draw a single crosshair (two 20 px orange lines crossing at (cx,cy))
 * plus the "i+1 / 5" progress label in the top-center. */
static void show_crosshair(int idx, int cx, int cy)
{
    clear_screen();
    lvgl_port_lock(0);

    static lv_point_precise_t h_pts[2], v_pts[2];
    h_pts[0].x = cx - 10; h_pts[0].y = cy;
    h_pts[1].x = cx + 10; h_pts[1].y = cy;
    v_pts[0].x = cx;      v_pts[0].y = cy - 10;
    v_pts[1].x = cx;      v_pts[1].y = cy + 10;

    lv_obj_t *h = lv_line_create(s_screen);
    lv_line_set_points(h, h_pts, 2);
    lv_obj_set_style_line_color(h, lv_color_hex(COLOR_TARGET), 0);
    lv_obj_set_style_line_width(h, 2, 0);

    lv_obj_t *v = lv_line_create(s_screen);
    lv_line_set_points(v, v_pts, 2);
    lv_obj_set_style_line_color(v, lv_color_hex(COLOR_TARGET), 0);
    lv_obj_set_style_line_width(v, 2, 0);

    add_label(LV_ALIGN_TOP_MID, 16, COLOR_DIM, "%d / %d", idx + 1, TOUCH_CAL_N_POINTS);
    lvgl_port_unlock();
}

static void show_result(bool pass,
                        const touch_cal_residual_t *r,
                        const touch_cal_matrix_t *m)
{
    clear_screen();
    lvgl_port_lock(0);
    if (pass) {
        add_label(LV_ALIGN_TOP_MID,    80,  COLOR_FG,    "Calibration OK");
        add_label(LV_ALIGN_TOP_MID,   160,  COLOR_FG,    "max residual: %.2f px", r->max_residual_px);
        add_label(LV_ALIGN_TOP_MID,   190,  COLOR_FG,    "RMS residual: %.2f px", r->rms_residual_px);
        add_label(LV_ALIGN_TOP_MID,   250,  COLOR_DIM,   "Constants logged to UART.");
    } else {
        add_label(LV_ALIGN_TOP_MID,    80,  COLOR_ERROR, "Fit poor - please retry");
        add_label(LV_ALIGN_TOP_MID,   160,  COLOR_FG,    "max residual: %.2f px", r->max_residual_px);
        add_label(LV_ALIGN_TOP_MID,   190,  COLOR_FG,    "RMS residual: %.2f px", r->rms_residual_px);
        add_label(LV_ALIGN_TOP_MID,   250,  COLOR_DIM,   "(try to hold still on");
        add_label(LV_ALIGN_TOP_MID,   280,  COLOR_DIM,   "each crosshair)");
    }
    add_label(LV_ALIGN_BOTTOM_MID, -80, COLOR_DIM,
              pass ? "Tap to re-run" : "Tap to retry");
    lvgl_port_unlock();
    (void)m;  /* matrix is logged to UART, not displayed */
}

/* ---------- Touch I/O ---------- */

/* Read one raw ADC sample. Returns true if a press is currently detected. */
static bool read_raw_sample(uint16_t *out_x, uint16_t *out_y)
{
    uint16_t xs[1], ys[1], ss[1];
    uint8_t cnt = 0;
    esp_lcd_touch_read_data(s_touch);
    bool pressed = esp_lcd_touch_get_coordinates(s_touch, xs, ys, ss, &cnt, 1);
    if (pressed && cnt > 0) {
        *out_x = xs[0];
        *out_y = ys[0];
        return true;
    }
    return false;
}

static void wait_for_release(void)
{
    uint16_t x, y;
    while (read_raw_sample(&x, &y)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* Block until a press is sustained for >= SAMPLE_SETTLE_MS, then collect
 * SAMPLE_COUNT samples at SAMPLE_INTERVAL_MS spacing and median-filter per
 * axis. Returns false if the captured range is too wide on either axis
 * (jittery tap — caller should retry the same target). */
static int cmp_u16(const void *a, const void *b)
{
    uint16_t va = *(const uint16_t *)a, vb = *(const uint16_t *)b;
    return (va > vb) - (va < vb);
}

static bool capture_target(float *out_x, float *out_y)
{
    uint16_t x, y;

    /* Wait for fresh press (caller must wait_for_release() between targets). */
    while (!read_raw_sample(&x, &y)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* Settle. */
    vTaskDelay(pdMS_TO_TICKS(SAMPLE_SETTLE_MS));

    uint16_t xs[SAMPLE_COUNT], ys[SAMPLE_COUNT];
    int n = 0;
    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        if (read_raw_sample(&x, &y)) {
            xs[n] = x;
            ys[n] = y;
            ++n;
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
    }

    if (n < SAMPLE_COUNT / 2) {
        /* User lifted mid-capture. Treat as jittery. */
        ESP_LOGW(TAG, "captured only %d/%d samples — rejected", n, SAMPLE_COUNT);
        return false;
    }

    qsort(xs, n, sizeof(uint16_t), cmp_u16);
    qsort(ys, n, sizeof(uint16_t), cmp_u16);
    uint16_t x_range = xs[n - 1] - xs[0];
    uint16_t y_range = ys[n - 1] - ys[0];

    if (x_range > SAMPLE_MAX_RANGE_ADC || y_range > SAMPLE_MAX_RANGE_ADC) {
        ESP_LOGW(TAG, "tap too jittery (x range=%u, y range=%u) — rejected",
                 x_range, y_range);
        return false;
    }

    *out_x = (float)xs[n / 2];
    *out_y = (float)ys[n / 2];
    return true;
}

/* ---------- UART output (spec §6) ---------- */

static void log_block(const float raw_x[TOUCH_CAL_N_POINTS],
                      const float raw_y[TOUCH_CAL_N_POINTS],
                      const touch_cal_residual_t *r,
                      const touch_cal_matrix_t *m,
                      bool pass)
{
    ESP_LOGI(TAG, "---------- calibration result ----------");
    ESP_LOGI(TAG, "targets (display px):   raw (ADC counts):");
    for (int i = 0; i < TOUCH_CAL_N_POINTS; ++i) {
        ESP_LOGI(TAG, "  (%3d, %3d)              (%4d, %4d)",
                 (int)TARGET_X[i], (int)TARGET_Y[i],
                 (int)raw_x[i], (int)raw_y[i]);
    }
    ESP_LOGI(TAG, "residuals: max=%.2f px, rms=%.2f px  -> %s",
             r->max_residual_px, r->rms_residual_px,
             pass ? "PASS" : "FAIL (max>5px or rms>3px)");
    if (pass) {
        ESP_LOGI(TAG, " ");
        ESP_LOGI(TAG, "/* paste into include/config.h */");
        ESP_LOGI(TAG, "#define TOUCH_CAL_AX  %12.6ff", m->ax);
        ESP_LOGI(TAG, "#define TOUCH_CAL_BX  %12.6ff", m->bx);
        ESP_LOGI(TAG, "#define TOUCH_CAL_CX  %12.3ff", m->cx);
        ESP_LOGI(TAG, "#define TOUCH_CAL_AY  %12.6ff", m->ay);
        ESP_LOGI(TAG, "#define TOUCH_CAL_BY  %12.6ff", m->by);
        ESP_LOGI(TAG, "#define TOUCH_CAL_CY  %12.3ff", m->cy);
    }
    ESP_LOGI(TAG, "----------------------------------------");
}

/* ---------- State machine ---------- */

static void run_one_pass(void)
{
    float raw_x[TOUCH_CAL_N_POINTS] = {0};
    float raw_y[TOUCH_CAL_N_POINTS] = {0};

    show_instructions();
    /* Wait for any tap to start. */
    wait_for_release();
    uint16_t dummy_x, dummy_y;
    while (!read_raw_sample(&dummy_x, &dummy_y)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    wait_for_release();

    /* Walk the 5 targets. */
    for (int i = 0; i < TOUCH_CAL_N_POINTS; ++i) {
        for (;;) {
            show_crosshair(i, (int)TARGET_X[i], (int)TARGET_Y[i]);
            if (capture_target(&raw_x[i], &raw_y[i])) break;
            /* On rejection, wait for release so the user can re-tap cleanly. */
            wait_for_release();
        }
        /* Brief confirmation pause so the user sees the crosshair disappear. */
        clear_screen();
        vTaskDelay(pdMS_TO_TICKS(150));
        wait_for_release();
    }

    /* Compute. */
    clear_screen();
    lvgl_port_lock(0);
    add_label(LV_ALIGN_CENTER, 0, COLOR_DIM, "Computing fit...");
    lvgl_port_unlock();

    touch_cal_matrix_t m = {0};
    bool solved = touch_cal_solve(raw_x, raw_y, TARGET_X, TARGET_Y, &m);
    touch_cal_residual_t r = { .max_residual_px = 1e9f, .rms_residual_px = 1e9f };
    if (solved) {
        touch_cal_residual(raw_x, raw_y, TARGET_X, TARGET_Y, &m, &r);
    } else {
        ESP_LOGE(TAG, "solver reported singular system — points collinear?");
    }

    bool pass = solved &&
                r.max_residual_px <= PASS_MAX_RESIDUAL_PX &&
                r.rms_residual_px <= PASS_RMS_RESIDUAL_PX;

    log_block(raw_x, raw_y, &r, &m, pass);
    show_result(pass, &r, &m);

    /* Wait for tap to restart. */
    wait_for_release();
    while (!read_raw_sample(&dummy_x, &dummy_y)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    wait_for_release();
}

static void cal_task(void *arg)
{
    (void)arg;
    /* Build the screen once; run_one_pass clears + repopulates it each cycle. */
    lvgl_port_lock(0);
    s_screen = lv_screen_active();
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lvgl_port_unlock();

    for (;;) {
        run_one_pass();
    }
}

/* ---------- Init ---------- */

static esp_lcd_touch_handle_t bringup_init(lv_display_t **out_disp)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 320 * 48 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS, .dc_gpio_num = PIN_LCD_DC, .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000, .trans_queue_depth = 10,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    gpio_config_t bl = {.pin_bit_mask = 1ULL << PIN_LCD_BL, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&bl);
    gpio_set_level(PIN_LCD_BL, 1);

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(PIN_TOUCH_CS);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &tp_io_cfg, &tp_io));
    esp_lcd_touch_handle_t tp = NULL;
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = 4095, .y_max = 4095,
        .rst_gpio_num = -1, .int_gpio_num = -1,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &tp));

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = 320 * 48,
        .double_buffer = true,
        .hres = 320, .vres = 480,
        .monochrome = false,
        .rotation = {.swap_xy = false, .mirror_x = true, .mirror_y = false},
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    *out_disp = lvgl_port_add_disp(&disp_cfg);

    /* No lvgl_port_add_touch() — we read the chip directly from cal_task. */
    return tp;
}

void app_main(void)
{
    lv_display_t *disp = NULL;
    s_touch = bringup_init(&disp);

    /* 8 KB stack is comfortable for LVGL operations + vsnprintf in add_label. */
    xTaskCreate(cal_task, "cal_task", 8 * 1024, NULL, 5, NULL);
    ESP_LOGI(TAG, "calibration bring-up running");
}
```

- [ ] **Step 2: Build — expect success**

```bash
cd ~/reflow_oven/tools/bringup/08_touch_cal
pio run -e esp32dev
```

Expected: ends with `SUCCESS`. If compilation fails on `lv_point_precise_t`, the installed LVGL minor version is older than 9.1 — substitute `lv_point_t` for `lv_point_precise_t` and rebuild.

- [ ] **Step 3: Commit**

```bash
cd ~/reflow_oven
git add tools/bringup/08_touch_cal/src/main.c
git commit -m "08_touch_cal: 5-point state machine, capture, fit, UART output"
```

---

## Task 6: On-device manual verification

This task is the device test — there's no automation. Run through the spec §8.1 checklist and confirm each item.

**Files:** none modified.

- [ ] **Step 1: Flash and open serial monitor**

```bash
cd ~/reflow_oven/tools/bringup/08_touch_cal
pio run -e esp32dev -t upload && pio device monitor
```

Expected first log lines (after boot):
```
I (xxx) touch_cal: calibration bring-up running
```

- [ ] **Step 2: Confirm intro screen renders**

On the display you should see:
- Dark background (`#0f1115`).
- Title "Touch Calibration" near the top.
- Five-line instruction block in the middle.
- "Tap anywhere to start" near the bottom.

If colours are scrambled or the text is upside-down, the LVGL display config in `bringup_init` doesn't match the panel — diff against `04_lvgl_hello/src/main.c` and align (this build copies its proven settings).

- [ ] **Step 3: Walk through all 5 targets in order**

Tap the intro screen. The first crosshair appears at (32, 48) — top-left. Tap it as precisely as you can and hold still for ~250 ms. The crosshair should disappear within ~200 ms of touch-down. Repeat for targets 2 (top-right), 3 (bottom-left), 4 (bottom-right), 5 (center).

The progress label `"1 / 5"`, `"2 / 5"`, … should update at the top of the screen for each target.

- [ ] **Step 4: Confirm result screen + UART block match**

After the 5th target, the screen briefly shows `"Computing fit..."` then transitions to either `"Calibration OK"` or `"Fit poor - please retry"`. The on-screen `max residual` and `RMS residual` numbers should equal the values logged to UART. On PASS, the UART scrollback contains the six `#define TOUCH_CAL_*` lines.

- [ ] **Step 5: Sanity check — bad tap on one target**

Re-run (tap the result screen). On target 3, tap somewhere clearly off the crosshair (~30 px away). Verify that the resulting max residual is noticeably larger than your previous PASS run — this proves the math is actually using the captured points and not e.g. always reporting zero.

- [ ] **Step 6: Sanity check — jittery tap rejection**

Re-run. On any target, drag your finger noticeably during the capture window. The serial log should show `tap too jittery (x range=…, y range=…) — rejected` and the crosshair should stay on screen waiting for a clean tap. Lift, then tap cleanly — flow should proceed normally.

- [ ] **Step 7: Sanity check — FAIL path emits no coefficients**

Re-run. Tap deliberately badly on at least two targets (~30 px off each). On the result screen, confirm `Fit poor - please retry`, and in the UART log confirm the block ends with `-> FAIL (max>5px or rms>3px)` and is **not** followed by any `#define TOUCH_CAL_*` lines.

- [ ] **Step 8: Save a known-good UART log**

When you do get a clean PASS run, copy the entire `---------- calibration result ----------` … `----------------------------------------` block from the serial monitor and paste it into the PR description (or save it to a scratch file). The §7 follow-up task will pick those numbers up from there.

- [ ] **Step 9: No code changes — nothing to commit**

Manual verification only. If any step fails, fix the underlying issue and amend Task 5's commit (or add a follow-up commit, your choice).

---

## Self-Review (post-write)

**Spec coverage check:**

| Spec section | Task(s) |
|---|---|
| §2 Deliverable — project layout | Task 4 |
| §3 Architecture — math layer in src/ for testability | Tasks 1–3 (math file lives in `src/drivers/`) |
| §3 Architecture — input/UI layer in bring-up | Task 5 |
| §4 State machine | Task 5 (`run_one_pass`) |
| §5.1 Raw input (all flags off, x_max=y_max=4095) | Task 4 step 6 + Task 5 (`bringup_init` touch cfg) |
| §5.2 Target layout (5 inset points) | Task 5 (`TARGET_X/Y`) |
| §5.3 Sampling (median, settle, jitter reject) | Task 5 (`capture_target`) |
| §5.4 Fit (3×3 normal equations) | Task 3 |
| §5.5 Pass criteria (max ≤ 5, rms ≤ 3) | Task 5 (`PASS_MAX_RESIDUAL_PX`, `PASS_RMS_RESIDUAL_PX`) |
| §6 UART format | Task 5 (`log_block`) |
| §7 Follow-up | explicitly out of scope, noted in plan header |
| §8.1 On-device manual verification | Task 6 |
| §8.2 Host-side test | Task 2 |

All requirements covered.

**Placeholder scan:** No TBDs, no "implement later", no "add error handling". Every code step shows full code.

**Type/name consistency:** `touch_cal_matrix_t`, `touch_cal_residual_t`, `touch_cal_solve`, `touch_cal_residual`, `TOUCH_CAL_N_POINTS` are introduced in Task 1's header and used identically in Tasks 2, 3, and 5. `TARGET_X/Y`, `PASS_*`, `SAMPLE_*` are introduced and used only in Task 5. No drift.

**One spec ambiguity, resolved up front:** spec §3 sketched the math as a section of `main.c`, but §8.2 wants it tested under `[env:native]`. The plan resolves this by putting the math in `src/drivers/touch_cal_math.{c,h}` from the start and `#include`ing it cross-dir from the bring-up — same source, two build targets. This also shrinks the §7 follow-up work (the math file will already be in place).

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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int cmp_u16(const void *a, const void *b)
{
    uint16_t va = *(const uint16_t *)a, vb = *(const uint16_t *)b;
    return (va > vb) - (va < vb);
}

/* Block until a press is detected, then settle and collect SAMPLE_COUNT
 * samples at SAMPLE_INTERVAL_MS spacing and median-filter per axis.
 * Returns false if the captured range is too wide on either axis
 * (jittery tap — caller should retry the same target). */
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
        ESP_LOGW(TAG, "captured only %d/%d samples - rejected", n, SAMPLE_COUNT);
        return false;
    }

    qsort(xs, n, sizeof(uint16_t), cmp_u16);
    qsort(ys, n, sizeof(uint16_t), cmp_u16);
    uint16_t x_range = xs[n - 1] - xs[0];
    uint16_t y_range = ys[n - 1] - ys[0];

    if (x_range > SAMPLE_MAX_RANGE_ADC || y_range > SAMPLE_MAX_RANGE_ADC) {
        ESP_LOGW(TAG, "tap too jittery (x range=%u, y range=%u) - rejected",
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
        ESP_LOGE(TAG, "solver reported singular system - points collinear?");
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

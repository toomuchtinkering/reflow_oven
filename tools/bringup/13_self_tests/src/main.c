/* tools/bringup/13_self_tests/src/main.c
 *
 * Bring-up 13: standalone subsystem self-test runner.
 *
 * Initialises LVGL + drivers in the same order as the main firmware
 * (display -> touch -> lvgl_port -> thermocouple -> SSR), then runs the
 * 4 validator hooks (TC, SSR, display, touch) and prints pass/fail + per-test
 * timing.
 *
 * Compare against the spec budgets (from include/config.h):
 *   OTA_SELF_TEST_TIMEOUT_MS = 2000 ms  (per-test)
 *   OTA_SELF_TEST_TOTAL_MS   = 5000 ms  (total)
 *
 * The TC hook reads 3 samples 50 ms apart, so ~150 ms total is expected.
 * Display and touch hooks only check that lv_display_get_default() and
 * lv_indev_get_next(NULL) return non-NULL — both require LVGL to be
 * properly initialised before the hooks are called.
 *
 * LVGL + display + touch init mirrors ui_task.c exactly:
 *   display_init() -> touch_init() -> lvgl_port_init() ->
 *   lvgl_port_add_disp() -> lvgl_port_add_touch()
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "drivers/display.h"
#include "drivers/touch.h"
#include "drivers/thermocouple.h"
#include "drivers/ssr.h"
#include "ota/ota_validator.h"

static const char *TAG = "bringup13";

void app_main(void) {
    /* ------------------------------------------------------------------
     * Bring up display + touch + LVGL in the same order as the main
     * firmware (see src/ui/ui_task.c :: ui_init).
     * ------------------------------------------------------------------ */
    display_handles_t d = display_init();
    esp_lcd_touch_handle_t tp = touch_init();

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    /* Panel dimensions and flags mirror the main firmware (320x480 portrait,
     * DMA buffers, byte-swap for RGB565, mirror_x for correct scan direction). */
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle    = d.io,
        .panel_handle = d.panel,
        .buffer_size  = 320 * 48,
        .double_buffer = true,
        .hres = 320, .vres = 480,
        .rotation = {.swap_xy = false, .mirror_x = true, .mirror_y = false},
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);

    /* touch_cfg.disp links the indev to the display so lv_indev_get_next(NULL)
     * returns a valid handle — required for ota_validator_hook_touch_alive(). */
    lvgl_port_touch_cfg_t touch_cfg = {.disp = disp, .handle = tp};
    lvgl_port_add_touch(&touch_cfg);

    /* Drivers needed by TC and SSR hooks. */
    thermocouple_init();
    ssr_init();

    ESP_LOGI(TAG, "=== running validator hooks ===");

    /* ------------------------------------------------------------------
     * Run the 4 hooks and capture per-test timing in microseconds.
     * ------------------------------------------------------------------ */
    int64_t t0 = esp_timer_get_time();
    bool tc      = ota_validator_hook_tc_alive();
    int64_t t1   = esp_timer_get_time();
    bool ssr     = ota_validator_hook_ssr_alive();
    int64_t t2   = esp_timer_get_time();
    bool display = ota_validator_hook_display_alive();
    int64_t t3   = esp_timer_get_time();
    bool touch   = ota_validator_hook_touch_alive();
    int64_t t4   = esp_timer_get_time();

    /* ------------------------------------------------------------------
     * Print results.  lld is correct on Xtensa ESP-IDF; int64_t is long long.
     * ------------------------------------------------------------------ */
    ESP_LOGI(TAG, "TC      : %s (%lld ms)", tc      ? "PASS" : "FAIL", (t1 - t0) / 1000);
    ESP_LOGI(TAG, "SSR     : %s (%lld ms)", ssr     ? "PASS" : "FAIL", (t2 - t1) / 1000);
    ESP_LOGI(TAG, "Display : %s (%lld ms)", display ? "PASS" : "FAIL", (t3 - t2) / 1000);
    ESP_LOGI(TAG, "Touch   : %s (%lld ms)", touch   ? "PASS" : "FAIL", (t4 - t3) / 1000);
    ESP_LOGI(TAG, "TOTAL   : %lld ms", (t4 - t0) / 1000);

    bool all_pass = tc && ssr && display && touch;
    ESP_LOGI(TAG, "=== %s ===", all_pass ? "ALL PASS" : "ONE OR MORE FAILED");

    /* Idle — keep the task alive so the serial monitor stays open. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

#include "drivers/touch.h"
#include "drivers/touch_cal_math.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_lcd_panel_io.h"
#include "driver/spi_master.h"
#include "config.h"

/* Calibration matrix measured by tools/bringup/08_touch_cal — see the
 * TOUCH CALIBRATION block in include/config.h. Built at compile time so
 * the runtime callback doesn't touch the macros. */
static const touch_cal_matrix_t s_cal = {
    .ax = TOUCH_CAL_AX, .bx = TOUCH_CAL_BX, .cx = TOUCH_CAL_CX,
    .ay = TOUCH_CAL_AY, .by = TOUCH_CAL_BY, .cy = TOUCH_CAL_CY,
};

/* esp_lcd_touch invokes this on every successful read, after the chip
 * driver has produced (near-)raw 12-bit values (we set x_max=y_max=4095
 * below so the chip's built-in scaling is a no-op). We replace each
 * point with the calibrated display-pixel position, clamping into the
 * 320x480 portrait window so an off-the-calibrated-region tap can't
 * deliver out-of-range coords to LVGL. */
static void apply_calibration(esp_lcd_touch_handle_t tp,
                              uint16_t *x, uint16_t *y, uint16_t *strength,
                              uint8_t *point_num, uint8_t max_point_num) {
    (void)tp; (void)strength; (void)max_point_num;
    for (uint8_t i = 0; i < *point_num; ++i) {
        float dx, dy;
        touch_cal_apply(&s_cal, (float)x[i], (float)y[i], &dx, &dy);
        if (dx < 0.0f) dx = 0.0f;
        if (dy < 0.0f) dy = 0.0f;
        if (dx > 319.0f) dx = 319.0f;
        if (dy > 479.0f) dy = 479.0f;
        x[i] = (uint16_t)dx;
        y[i] = (uint16_t)dy;
    }
}

esp_lcd_touch_handle_t touch_init(void) {
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(PIN_TOUCH_CS);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &tp_io_cfg, &tp_io));

    /* All mirror/swap flags off and x_max=y_max=4095 so the chip driver
     * delivers near-raw 12-bit ADC counts to apply_calibration — matching
     * exactly what tools/bringup/08_touch_cal fit the matrix against.
     *
     * int_gpio_num = -1 (04_lvgl_hello finding): ESP32 GPIO 36 has no
     * internal pull-up. esp_lvgl_port puts the indev in EVENT mode whenever
     * int_gpio_num is set; with no pull-up the IRQ never fires and LVGL
     * never polls. Setting -1 keeps it in timer-polled mode. If a hardware
     * pull-up is added later, change this back to PIN_TOUCH_IRQ.
     */
    esp_lcd_touch_handle_t tp = NULL;
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = 4095, .y_max = 4095,
        .rst_gpio_num = -1, .int_gpio_num = -1,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
        .process_coordinates = apply_calibration,
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &tp));
    return tp;
}

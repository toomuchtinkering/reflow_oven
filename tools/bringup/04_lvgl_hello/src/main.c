/* tools/bringup/04_lvgl_hello/src/main.c
 * Initialises LVGL via esp_lvgl_port over ST7796 + XPT2046.
 * Draws a single centred "Hello LVGL" label so we can confirm:
 *   - LVGL renders pixels (font rasterises)
 *   - Touch is wired into LVGL (label flips colour when tapped)
 *
 * LCD config mirrors what 02_display established for this board:
 *   - rgb_ele_order = BGR (renamed from rgb_endian in IDF >=6)
 *   - invert_color = false
 * Touch config mirrors 03_touch:
 *   - mirror_y so (0,0) is top-left of the portrait screen
 *
 * LVGL handles its own framebuffer byte order via lv_conf, so we don't
 * need the manual __builtin_bswap16 trick from 02_display.
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

static const char *TAG = "lvgl_bringup";
static lv_obj_t *label;

static void on_touch(lv_event_t *e) {
    static bool flipped;
    flipped = !flipped;
    lv_obj_set_style_text_color(label, flipped ? lv_color_hex(0xFF8C42) : lv_color_hex(0xE6E6E6), 0);
}

void app_main(void) {
    /* === LCD bus + panel === */
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCK, .mosi_io_num = PIN_LCD_MOSI, .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
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
    /* swap_xy/mirror are applied through lvgl_port_add_disp's rotation
     * field — calling esp_lcd_panel_mirror here would be overridden. */

    gpio_config_t bl = {.pin_bit_mask = 1ULL << PIN_LCD_BL, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&bl);
    gpio_set_level(PIN_LCD_BL, 1);

    /* === Touch ===
     * NOTE: int_gpio_num is intentionally -1 here. ESP32 GPIO 36 has no
     * internal pull-up; when esp_lvgl_port sees a real int_gpio_num it
     * switches the LVGL indev into EVENT mode (reads only on IRQ), so a
     * floating IRQ line means LVGL never polls touch. Setting -1 keeps
     * the indev in timer-polled mode.
     */
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(PIN_TOUCH_CS);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &tp_io_cfg, &tp_io));
    esp_lcd_touch_handle_t tp = NULL;
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = 320, .y_max = 480,
        .rst_gpio_num = -1, .int_gpio_num = -1,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 1 },
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
        /* mirror_x flips the panel's X scan so LVGL text reads left-to-right
         * on this board (default scan direction is right-to-left in portrait). */
        .rotation = {.swap_xy = false, .mirror_x = true, .mirror_y = false},
        /* swap_bytes is the same little-endian/MSB-first fix used manually in
         * 02_display, applied here at the LVGL flush stage. Without it, RGB565
         * colours come out garbled (channels rotated). */
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);

    lvgl_port_touch_cfg_t touch_cfg = {.disp = disp, .handle = tp};
    lvgl_port_add_touch(&touch_cfg);

    /* === Build the screen === */
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f1115), 0);
    /* Screens aren't clickable by default in LVGL 9; opt in explicitly. */
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    label = lv_label_create(scr);
    lv_label_set_text(label, "Hello LVGL");
    lv_obj_set_style_text_color(label, lv_color_hex(0xE6E6E6), 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(scr, on_touch, LV_EVENT_PRESSED, NULL);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL up. Tap the screen to toggle label colour.");
}

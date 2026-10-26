/* tools/bringup/03_touch/src/main.c
 * Polls XPT2046 touch and logs (x, y, pressed) to UART once per 100 ms.
 * Confirms touch wiring, IRQ pin, HSPI sharing.
 */
#include "esp_log.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_xpt2046.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch_bringup";

void app_main(void) {
    /* HSPI bus (shared with LCD in main fw; standalone here) */
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* Touch IO */
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(PIN_TOUCH_CS);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &tp_io_cfg, &tp_io));

    /* Touch driver */
    esp_lcd_touch_handle_t tp = NULL;
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = 320,
        .y_max = 480,
        .rst_gpio_num = -1,
        .int_gpio_num = PIN_TOUCH_IRQ,
        .levels = { .reset = 0, .interrupt = 0 },
        /* On this board the raw XPT2046 reading has the Y axis inverted
         * relative to the display's portrait coordinates (USB at bottom):
         * the chip reports y_max at the top of the screen and 0 at the
         * bottom. mirror_y flips this so (0,0) is top-left as expected. */
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 1 },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &tp));

    ESP_LOGI(TAG, "Tap the screen. Coordinates print below.");
    while (1) {
        uint16_t x[1], y[1], strength[1];
        uint8_t cnt = 0;
        esp_lcd_touch_read_data(tp);
        bool pressed = esp_lcd_touch_get_coordinates(tp, x, y, strength, &cnt, 1);
        if (pressed && cnt > 0) {
            ESP_LOGI(TAG, "touch: x=%u y=%u s=%u", x[0], y[0], strength[0]);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

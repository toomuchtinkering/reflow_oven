/* tools/bringup/02_display/src/main.c
 * Initialises ST7796 in portrait orientation and fills the screen with three
 * horizontal bands (red / green / blue). Confirms LCD wiring, HSPI bus,
 * portrait orientation, and backlight pin.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "config.h"
#include <string.h>

#define LCD_WIDTH  320
#define LCD_HEIGHT 480
#define BAND_LINES 80    /* draw a horizontal band of 80 lines at a time */

static const char *TAG = "disp_bringup";
static SemaphoreHandle_t flush_done;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx) {
    BaseType_t hp_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(flush_done, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

void app_main(void) {
    flush_done = xSemaphoreCreateBinary();
    /* SPI bus (HSPI = SPI2_HOST) */
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * BAND_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* Panel IO over SPI */
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    /* ST7796 panel */
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
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, false));            /* portrait */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    /* Backlight on */
    gpio_config_t bl = {.pin_bit_mask = 1ULL << PIN_LCD_BL, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&bl);
    gpio_set_level(PIN_LCD_BL, 1);

    /* Draw three horizontal bands. RGB565.
     * The ST7796 expects each pixel MSB-first on the wire, but the ESP32 is
     * little-endian, so we pre-swap the bytes of each colour constant so the
     * SPI driver pushes them out in the right order.
     */
    static uint16_t band[LCD_WIDTH * BAND_LINES];
    const uint16_t RED   = __builtin_bswap16(0xF800);
    const uint16_t GREEN = __builtin_bswap16(0x07E0);
    const uint16_t BLUE  = __builtin_bswap16(0x001F);

    for (int row = 0; row < LCD_HEIGHT; row += BAND_LINES) {
        uint16_t color = (row < 160) ? RED : (row < 320) ? GREEN : BLUE;
        for (size_t i = 0; i < LCD_WIDTH * BAND_LINES; i++) band[i] = color;
        esp_lcd_panel_draw_bitmap(panel, 0, row, LCD_WIDTH, row + BAND_LINES, band);
        /* draw_bitmap is async over SPI/DMA; block until the flush callback
         * fires before reusing the shared buffer. */
        xSemaphoreTake(flush_done, portMAX_DELAY);
    }

    ESP_LOGI(TAG, "Display painted. If you see Red/Green/Blue bands top-to-bottom, success.");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}

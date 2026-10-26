/* tools/bringup/07_safety/src/main.c
 * Wires up MAX31855 read + a synthetic "controller" that drives SSR=ON when
 * temp < 30 C and OFF otherwise — UNLESS a fault is detected, in which case
 * SSR is forced OFF and a latched fault state is reported.
 *
 * Manual test:
 *  - Start with thermocouple connected at room temp: SSR pulses ON (heating).
 *  - Disconnect thermocouple mid-loop: SSR forced OFF, FAULT latched.
 *  - Reconnect thermocouple: SSR stays OFF until you reset the board.
 * This proves the safety chain works *before* we use it on a real heater.
 *
 * !!! HEATER must be physically disconnected from the AC mains side of
 * the SSR while running this test. !!!
 */
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "config.h"

static const char *TAG = "safety_bringup";
static spi_device_handle_t spi;
static bool latched_fault = false;

static void tc_init(void) {
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_TC_SCK, .miso_io_num = PIN_TC_MISO, .mosi_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t cfg = {
        .clock_speed_hz = 1000000, .mode = 0, .spics_io_num = PIN_TC_CS, .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &cfg, &spi));
}

static bool tc_read(float *t) {
    uint8_t rx[4] = {0};
    spi_transaction_t tr = {.length = 32, .rxlength = 32, .rx_buffer = rx};
    ESP_ERROR_CHECK(spi_device_transmit(spi, &tr));
    uint32_t w = ((uint32_t)rx[0]<<24)|((uint32_t)rx[1]<<16)|((uint32_t)rx[2]<<8)|rx[3];
    if (w & (1u << 16)) return false;          /* any-fault bit */
    if (w & 0x7) return false;                 /* OC/SCG/SCV */
    int16_t raw = (int16_t)((w >> 18) & 0x3FFF);
    if (raw & 0x2000) raw |= 0xC000;
    *t = raw * 0.25f;
    return true;
}

static void ssr_init(void) {
    gpio_set_level(PIN_SSR, 0);
    gpio_config_t cfg = {.pin_bit_mask = 1ULL << PIN_SSR, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&cfg);
}

void app_main(void) {
    tc_init();
    ssr_init();
    ESP_LOGW(TAG, "Safety chain dry run. HEATER MUST BE DISCONNECTED.");
    while (1) {
        float t;
        bool ok = tc_read(&t);
        if (!ok) {
            latched_fault = true;
            gpio_set_level(PIN_SSR, 0);
            ESP_LOGE(TAG, "FAULT latched — SSR off");
        } else if (latched_fault) {
            gpio_set_level(PIN_SSR, 0);
            ESP_LOGW(TAG, "fault persists; temp=%.2f", t);
        } else if (t > OVER_TEMP_LIMIT_C) {
            latched_fault = true;
            gpio_set_level(PIN_SSR, 0);
            ESP_LOGE(TAG, "OVER-TEMP %.2f — SSR off, fault latched", t);
        } else {
            bool on = (t < 30.0f);
            gpio_set_level(PIN_SSR, on ? 1 : 0);
            ESP_LOGI(TAG, "temp=%.2f SSR=%s", t, on ? "ON" : "OFF");
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

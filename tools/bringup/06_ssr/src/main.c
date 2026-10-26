/* tools/bringup/06_ssr/src/main.c
 * Toggles PIN_SSR every 1 s. Watch the SSR's input LED pulse in time.
 * Multimeter check: when GPIO is low or unprogrammed, the 10k pull-down
 * should hold the SSR control input below ~0.4 V.
 *
 * !!! HEATER must be physically disconnected from the AC mains side of
 * the SSR while running this test. !!!
 */
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "config.h"

static const char *TAG = "ssr_bringup";

void app_main(void) {
    /* Configure SSR pin LOW *before* enabling output — keep it safe at boot. */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_SSR,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = 0,    /* external pull-down does this — don't fight it */
        .pull_up_en   = 0,
    };
    gpio_set_level(PIN_SSR, 0);   /* drive low first */
    gpio_config(&cfg);

    ESP_LOGW(TAG, "SSR toggle. HEATER MUST BE DISCONNECTED. Pin %d.", PIN_SSR);
    bool on = false;
    while (1) {
        on = !on;
        gpio_set_level(PIN_SSR, on ? 1 : 0);
        ESP_LOGI(TAG, "SSR %s", on ? "ON " : "OFF");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

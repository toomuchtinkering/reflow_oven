/* tools/bringup/01_blink/src/main.c
 * Cycles RGB LED red -> green -> blue -> off, 500 ms per state.
 * Confirms ESP-IDF + PIO + flashing + GPIO basics.
 * RGB LED pins are *active low* per board spec.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "config.h"

static void led_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_LED_R) | (1ULL << PIN_LED_G) | (1ULL << PIN_LED_B),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    /* off = HIGH (active low) */
    gpio_set_level(PIN_LED_R, 1);
    gpio_set_level(PIN_LED_G, 1);
    gpio_set_level(PIN_LED_B, 1);
}

static void all_off(void) {
    gpio_set_level(PIN_LED_R, 1);
    gpio_set_level(PIN_LED_G, 1);
    gpio_set_level(PIN_LED_B, 1);
}

void app_main(void) {
    led_init();
    while (1) {
        all_off(); gpio_set_level(PIN_LED_R, 0); vTaskDelay(pdMS_TO_TICKS(500));
        all_off(); gpio_set_level(PIN_LED_G, 0); vTaskDelay(pdMS_TO_TICKS(500));
        all_off(); gpio_set_level(PIN_LED_B, 0); vTaskDelay(pdMS_TO_TICKS(500));
        all_off();                                vTaskDelay(pdMS_TO_TICKS(500));
    }
}

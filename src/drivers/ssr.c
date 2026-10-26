#include "drivers/ssr.h"
#include "drivers/ssr_scheduler.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <stdbool.h>

void ssr_init(void) {
    /* Write the output-level register to 0 *before* enabling OE via
     * gpio_config, so the pin transitions straight from high-Z (held low
     * by the external 10 k pull-down) to actively-driven-LOW without
     * passing through actively-driven-HIGH. The second gpio_set_level
     * after gpio_config is belt-and-braces; some IDF builds reset the
     * level register inside gpio_config. */
    gpio_set_level(PIN_SSR, 0);
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_SSR,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = 0,
        .pull_up_en   = 0,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_SSR, 0);
}

static const char *TAG = "ssr";

static uint16_t s_quanta = SSR_QUANTA_PER_WINDOW;

void ssr_set_window_quanta(uint16_t quanta) {
    if (quanta < 1) {
        /* Falling back to the reflow window would silently reinstate the very
         * defect the parameterisation fixes: at 8 quanta any duty <= 6 % rounds
         * to zero and the SSR never fires, so a drying hold would simply never
         * heat. That is not hazardous (the DRY_HEATUP_TIMEOUT_MS backstop ends
         * the run), but it is invisible, so log it loudly rather than swallow
         * it. Not an assert: a programming error here does not warrant
         * rebooting an oven that may be mid-run. */
        ESP_LOGE(TAG, "ssr_set_window_quanta(0) ignored; keeping %u quanta", s_quanta);
        return;
    }
    s_quanta = quanta;
}

void ssr_tick(uint32_t now_ms, uint8_t duty_percent) {
    bool on = ssr_scheduler_should_be_on(now_ms, duty_percent, s_quanta);
    gpio_set_level(PIN_SSR, on ? 1 : 0);
}

void ssr_force_off(void) {
    gpio_set_level(PIN_SSR, 0);
}

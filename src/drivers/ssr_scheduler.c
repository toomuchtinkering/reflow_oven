#include "drivers/ssr_scheduler.h"
#include "config.h"
#include <assert.h>

bool ssr_scheduler_should_be_on(uint32_t now_ms, uint8_t duty_percent,
                                 uint16_t quanta_per_window) {
    assert(quanta_per_window >= 1);
    if (duty_percent >= 100) return true;
    if (duty_percent == 0)   return false;

    uint32_t slot = (now_ms / CONTROL_TICK_MS) % quanta_per_window;
    /* Round to nearest. */
    uint32_t threshold = ((uint32_t)duty_percent * quanta_per_window + 50) / 100;
    return slot < threshold;
}

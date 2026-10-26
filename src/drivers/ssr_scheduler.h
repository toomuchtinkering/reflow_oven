/* Pure-logic core of the slow-PWM SSR scheduler.
 * Given the current time and a duty percent, returns whether the SSR
 * pin should be HIGH or LOW for this tick. See spec §4.2.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* `quanta_per_window` DEFINES the window as quanta_per_window * CONTROL_TICK_MS.
 * Reflow passes SSR_QUANTA_PER_WINDOW (8); drying passes DRY_SSR_QUANTA (120).
 * Must be >= 1. */
bool ssr_scheduler_should_be_on(uint32_t now_ms, uint8_t duty_percent,
                                 uint16_t quanta_per_window);

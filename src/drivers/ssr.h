/* Single owner of the SSR GPIO. Wraps drivers/ssr_scheduler (pure logic)
 * and the actual gpio_set_level call. See spec §4.2 + §6.1.
 */
#pragma once
#include <stdint.h>

void ssr_init(void);                       /* GPIO low, then configure as output */

/* Select the PWM window used by subsequent ssr_tick calls. Set on mode entry
 * by control_task: SSR_QUANTA_PER_WINDOW for reflow/step test, DRY_SSR_QUANTA
 * for drying. Starts at SSR_QUANTA_PER_WINDOW.
 *
 * A `quanta` of 0 is a programming error: it is REJECTED (the current window
 * is kept) and logged at ERROR. It is deliberately not silently coerced to the
 * reflow window, because at 8 quanta any duty <= 6 % rounds to zero and a
 * drying hold would never heat -- the exact defect this parameter exists to
 * fix, reintroduced invisibly. */
void ssr_set_window_quanta(uint16_t quanta);

void ssr_tick(uint32_t now_ms, uint8_t duty_percent);   /* called from control_task */
void ssr_force_off(void);                  /* fault path */

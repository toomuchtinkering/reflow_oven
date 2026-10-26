/* Numeric keypad popup modal. Caller hands in field metadata; on OK the
 * callback receives the (clamped) value. Per spec §7.5.
 */
#pragma once
#include "lvgl.h"
#include <stdbool.h>

typedef void (*keypad_done_cb)(float value, void *user_data);

typedef struct {
    const char *field_label;  /* e.g. "Soak Temperature" */
    const char *unit;         /* e.g. "°C" */
    float       min;
    float       max;
    float       initial;
    bool        allow_decimal;
    keypad_done_cb on_ok;
    void          *user_data;
} keypad_config_t;

void screen_keypad_open(const keypad_config_t *cfg);
void screen_keypad_close(void);
bool screen_keypad_is_open(void);

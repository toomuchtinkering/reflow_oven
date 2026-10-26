#pragma once
#include "esp_lcd_touch.h"

/* Assumes display_init() has already initialised SPI2_HOST (LCD + touch share bus). */
esp_lcd_touch_handle_t touch_init(void);

#pragma once
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t    panel;
} display_handles_t;

/* SPI bus + panel + backlight. Lifted from bringup 02_display + 04_lvgl_hello.
 * NOTE: swap_xy and mirror are NOT applied here; lvgl_port_add_disp owns those
 * via its rotation field and would override any call made before.
 */
display_handles_t display_init(void);

#pragma once
#include "lvgl.h"
#include "oven_state.h"

lv_obj_t *screen_about_create(void);

/* 4 Hz refresh hook (ui_task). Updates the live electronics-temperature label;
 * cheap no-op while the About sub-screen is hidden or not yet created. */
void screen_about_update(const oven_state_t *snap);

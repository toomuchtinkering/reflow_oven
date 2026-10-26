#pragma once
#include "lvgl.h"
lv_obj_t *screen_network_create(void);

/* Called from the UI refresh tick; refreshes the live fields (status, IP,
 * signal) only while the Network sub-screen is visible. */
void screen_network_update(void);

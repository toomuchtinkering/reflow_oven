#pragma once
#include "lvgl.h"
/* Pass empty string for manual SSID entry (an SSID textarea is then shown). */
lv_obj_t *screen_wifi_password_create(const char *ssid);

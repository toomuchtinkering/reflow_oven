#pragma once
#include "lvgl.h"
lv_obj_t *screen_wifi_scan_create(void);

/* Drive from the UI tick: re-scans when the scan screen is shown with an empty
 * list (e.g. after returning from the password screen, which frees the list). */
void screen_wifi_scan_update(void);

typedef void (*wifi_scan_chosen_cb_t)(const char *ssid, bool locked);
void screen_wifi_scan_set_chosen_cb(wifi_scan_chosen_cb_t cb);

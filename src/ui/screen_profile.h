#pragma once
#include "lvgl.h"
#include "oven_state.h"

void screen_profile_create(lv_obj_t *parent);
void screen_profile_set_initial(const profile_t *p);   /* loaded from NVS at boot */

/* Called every refresh tick from ui_task. Watches snap->mode to
 * enable/disable SAVE per spec §7.6, and watches
 * snap->profile_applied_seq to flash a "saved" toast when the
 * controller acknowledges a SAVE. */
void screen_profile_update(const oven_state_t *snap);

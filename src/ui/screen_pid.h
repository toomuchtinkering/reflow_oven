#pragma once
#include "lvgl.h"
#include "oven_state.h"
#include "storage/nvs_schema.h"

void screen_pid_create(lv_obj_t *parent);
void screen_pid_set_initial(const pid_gains_t *g);

/* Called every refresh tick from ui_task. Watches
 * snap->pid_applied_seq and triggers the "✓ saved" toast when the
 * controller acknowledges a commit (i.e. after NVS save). */
void screen_pid_update(const oven_state_t *snap);

lv_obj_t *screen_pid_subscreen_create(void);

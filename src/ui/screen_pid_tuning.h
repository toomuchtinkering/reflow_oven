/* Settings → PID Tuning sub-screen. Enters step-test parameters, starts the
 * open-loop step test (touchscreen-only), shows the live rise + STOP while it
 * runs, and surfaces the browser-suggested-gain confirm prompt. See spec
 * 2026-05-31 §2.2 / §3 / §4.
 */
#pragma once
#include "lvgl.h"
#include "oven_state.h"

/* Lazy-created on first navigation (matches the other Settings sub-screens). */
lv_obj_t *screen_pid_tuning_create(void);

/* Called every UI tick from ui_refresh_timer with the latest snapshot.
 * No-op if the sub-screen has not been created yet. */
void screen_pid_tuning_update(const oven_state_t *snap);

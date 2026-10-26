#pragma once
#include "lvgl.h"

void screen_settings_create(lv_obj_t *parent);

lv_obj_t *settings_make_subscreen(const char *title);
void settings_push_subscreen(lv_obj_t *sub);
void settings_pop_subscreen(void);

/* Pop all sub-screens except the first (used to return to the root sub-screen,
 * e.g. Network status after a successful connect). */
void settings_pop_to_root_sub(void);

/* Register a callback to run when the NEXT pushed sub-screen is popped (Back or
 * programmatic). Lets a screen clean up resources on exit. Cleared after one push. */
void settings_set_next_pop_hook(void (*hook)(void));

/* Modal OK/Cancel confirmation. `on_confirm` runs on OK; `on_cancel` (may be
 * NULL) runs on Cancel OR when the dialog is closed by Back-navigation/dismiss.
 * Both fire at most once, on the LVGL task. One dialog at a time (settings
 * sub-screens are single-level). */
void settings_confirm(const char *title, const char *msg,
                      void (*on_confirm)(void), void (*on_cancel)(void));

/* Modal info box with a single OK button (no callbacks). Shares the one-at-a-
 * time dialog slot with settings_confirm. */
void settings_info(const char *title, const char *msg);

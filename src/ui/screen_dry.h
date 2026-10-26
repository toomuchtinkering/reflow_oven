/* DRY tab — filament drying preset list, editing, and START.
 * Structurally parallel to the PROFILE tab: PROFILE edits the reflow recipe,
 * DRY edits the drying recipe and starts it. A running dry renders on HOME
 * (see screen_home_set_dry_view), so this tab has no live display of its own.
 */
#pragma once
#include "lvgl.h"
#include "oven_state.h"

void screen_dry_create(lv_obj_t *parent);

/* Refresh from the latest snapshot: START is disabled unless the oven is idle
 * and cool enough for the selected preset to be accepted. */
void screen_dry_update(const oven_state_t *snap);

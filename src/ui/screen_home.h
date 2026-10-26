#pragma once
#include "lvgl.h"
#include "oven_state.h"

/* Build the Home screen content inside `parent` (a tab page).
 * Returns nothing — internal state held in static globals.
 */
void screen_home_create(lv_obj_t *parent);

/* Refresh widgets from the latest state snapshot. */
void screen_home_update(const oven_state_t *snap);

/* Re-render the planned-curve line and the stage-transition overlays
 * (vertical transition lines, transition tick labels, horizontal
 * soak/reflow target lines + temperature labels). Called from
 * ui_task whenever the snapshot's curve buffer was copied (i.e. every
 * refresh tick, since the UI owns its own copy). The caller must NOT
 * hold the state mutex during this call — LVGL operations are routed
 * through the LVGL port lock which the refresh timer already owns.
 *
 * `phase_end_ms` is the snapshot's per-stage end-time array. Used to
 * size the X-axis cycle window (end of reflow + cool-down view) and
 * to anchor the transition gridlines + labels at the right t / temp.
 */
void screen_home_set_curve(const float *samples, uint16_t n,
                           const uint32_t phase_end_ms[]);

/* Switch the graph into (or out of) drying mode. The Y axis rescales to
 * target_c + DRY_GRAPH_HEADROOM_C and the X axis becomes a rolling
 * DRY_GRAPH_WINDOW_MS window ending at "now" rather than the whole run: a
 * 6-16 h hold plotted whole is a flat line carrying no information, while the
 * rolling window shows ripple and stability. The reflow overlays (phase
 * gridlines, soak/reflow target lines) are hidden and the planned line becomes
 * a flat reference at the target.
 *
 * Pass target_c <= 0 to return the graph to reflow scaling. Idempotent — a
 * call that does not change the view does nothing, so ui_task may call it
 * every refresh tick. Changing the view clears the live trace: the two X axes
 * are not comparable, so carrying pixels across would draw a lie.
 *
 * Must be called with the LVGL port lock held, like the other entry points
 * here (the refresh timer already owns it). */
void screen_home_set_dry_view(float target_c);

#include "ui/screen_home.h"
#include "ui/theme.h"
#include "oven_state.h"
#include "control/reflow_sm.h"
#include "control/profile_curve.h"
#include "config.h"
#include "freertos/queue.h"
#include "ota/boot_status.h"
#include "ota/ota_validator.h"   /* ota_validator_last_failure for the failed banner */
#include "version.h"
#include "esp_ota_ops.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern QueueHandle_t g_cmd_queue;   /* defined in ui_task.c */

static lv_obj_t *lbl_temp, *lbl_target, *lbl_stage, *btn_action, *btn_action_lbl;
static lv_obj_t *s_pending_banner = NULL;
static lv_obj_t *s_failed_banner  = NULL;
static lv_obj_t *s_failed_banner_lbl = NULL;
static char      s_prev_version[32] = {0};   /* inactive-slot version (rollback target) */

/* Forward declaration — defined below, used in lv_obj_add_event_cb. */
static void banner_clicked_cb(lv_event_t *e);
static lv_obj_t *graph_canvas;
static oven_mode_t s_mode_cached = MODE_IDLE;   /* updated by screen_home_update */

/* Cached cycle window for live-trace coordinate math. Set by
 * screen_home_set_curve so screen_home_update places live samples on
 * the same X-axis the planned curve drew against. */
static uint32_t s_cycle_total_ms = 0;

/* Graph layout: 296 × 220 canvas at (0, 130). Use an inner plot area
 * inset for axis labels. These are render-only pixel coordinates, not
 * tunable parameters, so they stay local to this file. */
#define GRAPH_W            296
#define GRAPH_H            200
#define GRAPH_INSET_LEFT    36   /* room for "260 °C" Y-axis label */
#define GRAPH_INSET_RIGHT    8
#define GRAPH_INSET_TOP      4
#define GRAPH_INSET_BOTTOM  24   /* room for time X-axis label + clearance from the canvas border */
#define GRAPH_PLOT_W (GRAPH_W - GRAPH_INSET_LEFT - GRAPH_INSET_RIGHT)
#define GRAPH_PLOT_H (GRAPH_H - GRAPH_INSET_TOP  - GRAPH_INSET_BOTTOM)

/* Shared Y position for every X-axis text label: just under the plot
 * area, inside the canvas bottom inset, with a few px clearance from
 * the canvas border so font descenders (g/p/y) and the LVGL theme
 * border don't clip the bottom row. */
#define GRAPH_X_LABEL_Y (GRAPH_INSET_TOP + GRAPH_PLOT_H + 4)

/* Rough per-character pixel width for the default LVGL font
 * (Montserrat 14). Used for label-width estimation when centring
 * transition tick labels and detecting overlap. */
#define LBL_CHAR_W_PX 7
/* Row height of a Montserrat-14 axis label, for vertical overlap detection
 * between the Y-axis rail labels and the soak/reflow target labels. */
#define GRAPH_Y_LABEL_H 16

/* Temperature plot range. Lower rail at 0 °C; upper rail uses the
 * over-temp limit so the plot covers the whole safe operating range
 * without duplicating the constant. */
#define GRAPH_TEMP_MIN_C   0.0f

/* Planned-curve buffer. Static so it lives for the program's lifetime
 * and so lv_line_set_points is safe to call without copy.
 * PROFILE_CURVE_MAX_SAMPLES comes from profile_curve.h. */
static lv_point_precise_t s_plan_pts[PROFILE_CURVE_MAX_SAMPLES];

/* Live trace ring buffer. Capacity in include/config.h as
 * GRAPH_LIVE_RING_CAP (= PROFILE_CURVE_MAX_SAMPLES). Sample cadence
 * is PROFILE_CURVE_STEP_MS, decimated from the UI refresh tick via
 * s_live_last_idx. */
static lv_point_precise_t s_live_pts[GRAPH_LIVE_RING_CAP];
static uint16_t           s_live_n;
static uint32_t           s_live_last_idx = UINT32_MAX;

/* > 0 while a dry is being rendered; drives both axis mappings and selects
 * the drying chrome. 0 = reflow view. See screen_home_set_dry_view. */
static float s_dry_target_c = 0.0f;

/* Last curve handed to screen_home_set_curve, cached so the view can be
 * re-rendered when it flips between reflow and drying. ui_task calls
 * set_curve only when the applied profile changes (#37), so a dry starting
 * mid-session never brings a fresh call with it. `s_last_samples` points at
 * ui_task's file-static copy buffer, which outlives every render. */
static const float *s_last_samples = NULL;
static uint16_t     s_last_n       = 0;
static uint32_t     s_last_phase_end_ms[STAGE_COUNT_] = {0};

_Static_assert(DRY_GRAPH_SAMPLES <= GRAPH_LIVE_RING_CAP,
               "drying graph window must fit the existing live-trace ring");

static lv_obj_t *line_planned, *line_live;
static lv_obj_t *lbl_axis_t_min, *lbl_axis_t_max, *lbl_axis_y_top, *lbl_axis_y_bot;

/* Stage-transition overlays. Vertical lines + tick labels at the four
 * phase boundaries (end of heat-up 1 / soak / heat-up 2 / reflow) and
 * two horizontal lines + labels at the soak and reflow target temps. */
static lv_obj_t *line_v_trans[4];
static lv_obj_t *lbl_t_trans[4];
static lv_obj_t *line_h_soak, *line_h_reflow;
static lv_obj_t *lbl_h_soak, *lbl_h_reflow;
static lv_point_precise_t s_v_trans_pts[4][2];
static lv_point_precise_t s_h_soak_pts[2];
static lv_point_precise_t s_h_reflow_pts[2];

static int32_t graph_x_from_ms(uint32_t t_ms, uint32_t t_max_ms) {
    if (t_max_ms == 0) return GRAPH_INSET_LEFT;
    if (t_ms > t_max_ms) t_ms = t_max_ms;
    return GRAPH_INSET_LEFT +
           (int32_t)((uint64_t)t_ms * GRAPH_PLOT_W / t_max_ms);
}

/* Top of the temperature axis. Drying runs at 40-100 C, which occupies the
 * bottom third of the reflow axis and hides every deviation that matters at
 * a +/-2 C hold band, so the axis follows the target instead. */
static float graph_temp_max(void) {
    return (s_dry_target_c > 0.0f) ? (s_dry_target_c + DRY_GRAPH_HEADROOM_C)
                                   : OVER_TEMP_LIMIT_C;
}

static int32_t graph_y_from_c(float t_c) {
    const float t_max = graph_temp_max();
    if (t_c < GRAPH_TEMP_MIN_C) t_c = GRAPH_TEMP_MIN_C;
    if (t_c > t_max) t_c = t_max;
    float ratio = (t_c - GRAPH_TEMP_MIN_C) / (t_max - GRAPH_TEMP_MIN_C);
    /* Invert: higher temp = lower Y pixel. */
    return GRAPH_INSET_TOP + (int32_t)((1.0f - ratio) * GRAPH_PLOT_H);
}

/* X for live-trace slot `i` of the rolling drying window. The slot index IS
 * the time coordinate: the ring is appended at exactly DRY_GRAPH_SAMPLE_MS
 * and shifts by one slot when full, so slot 0 is always the oldest sample in
 * view and slot DRY_GRAPH_SAMPLES-1 is "now". This is why the drying trace
 * stores no timestamps: X depends only on the slot, and therefore never has
 * to be recomputed as the window scrolls. */
static int32_t dry_x_from_slot(uint16_t i) {
    return GRAPH_INSET_LEFT +
           (int32_t)((uint32_t)i * GRAPH_PLOT_W / (DRY_GRAPH_SAMPLES - 1u));
}

static void action_clicked_cb(lv_event_t *e) {
    (void)e;
    if (s_mode_cached == MODE_PENDING_VALIDATION) {
        /* TODO: surface a toast — for now silently no-op. The disabled
         * button already prevents the click, but we keep this gate as
         * defense-in-depth. */
        return;
    }
    cmd_msg_t msg = {0};
    /* Dispatch by cached mode, not label text — label is refreshed
     * every 250 ms so a strcmp on it can lag the real state. */
    switch (s_mode_cached) {
        case MODE_IDLE:      msg.type = CMD_START;     break;
        case MODE_RUNNING:   msg.type = CMD_STOP;      break;
        case MODE_STEP_TEST: msg.type = CMD_STOP;      break;  /* STOP available from any screen */
        case MODE_DRYING:    msg.type = CMD_STOP;      break;  /* the button already reads STOP */
        case MODE_FAULT:     msg.type = CMD_ACK_FAULT; break;
        default: return;
    }
    xQueueSend(g_cmd_queue, &msg, 0);
}

static void banner_clicked_cb(lv_event_t *e) {
    (void)e;
    /* Only request the transition. The control task performs
     * esp_ota_mark_app_valid_cancel_rollback(), bound to the actual
     * PENDING→IDLE transition, so rollback can't be cancelled off a fault
     * path (finding #4). */
    cmd_msg_t m = { .type = CMD_CLEAR_PENDING };
    xQueueSend(g_cmd_queue, &m, 0);
}

void screen_home_create(lv_obj_t *parent) {
    /* Explicit BG_COVER + border_opa 0 — the LVGL 9 default dark theme
     * paints the tab page with a primary-tinted panel by default; this
     * forces THEME_BG so the temperature label sits on the same dark
     * colour as the rest of the screen. */
    lv_obj_set_style_bg_color(parent, THEME_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(parent, 8, 0);
    /* Everything is laid out to fit the 420 px tab page — no scrolling. */
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* Pending banner — orange, tap-to-confirm */
    s_pending_banner = lv_obj_create(parent);
    lv_obj_set_size(s_pending_banner, LV_HOR_RES - 16, 32);
    lv_obj_align(s_pending_banner, LV_ALIGN_TOP_MID, 0, 0);   /* centred: equal side margins */
    lv_obj_set_style_bg_color(s_pending_banner, lv_color_hex(0xCC8800), 0);
    lv_obj_set_style_bg_opa(s_pending_banner, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_pending_banner, LV_OBJ_FLAG_HIDDEN);
    /* Fits today (249 of 274 px), but this banner is tap-to-confirm and the
     * only way to validate a pending image, so a scroll gesture swallowing
     * that tap would strand the update. Same one-line guard. */
    lv_obj_remove_flag(s_pending_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_pending_banner, banner_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(s_pending_banner);
    /* Show the version transition "<old> → <new>" (finding #35). The running
     * image is the new (pending) firmware; the inactive slot holds what a
     * rollback would restore. One-time read at create — not per tick. */
    /* Capture the would-be-rolled-back version (inactive slot) for the
     * version-transition line shown below. The banner itself stays a short,
     * dark-on-orange call-to-action — two 13-char version hashes plus text do
     * not fit one 320 px line, which is what ran off-screen before. */
    {
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        esp_app_desc_t od = {0};
        if (other && esp_ota_get_partition_description(other, &od) == ESP_OK && od.version[0])
            snprintf(s_prev_version, sizeof s_prev_version, "%s", od.version);
        else
            s_prev_version[0] = '\0';
    }
    lv_label_set_text(bl, LV_SYMBOL_WARNING " Update pending - tap to confirm");
    lv_obj_set_style_text_color(bl, THEME_BG, 0);   /* dark text reads on orange */
    lv_obj_center(bl);

    /* Failed banner — red, informational */
    s_failed_banner = lv_obj_create(parent);
    lv_obj_set_size(s_failed_banner, LV_HOR_RES - 16, 32);
    lv_obj_align(s_failed_banner, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_failed_banner, lv_color_hex(0x9C2424), 0);  /* dark red */
    lv_obj_set_style_bg_opa(s_failed_banner, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_failed_banner, LV_OBJ_FLAG_HIDDEN);
    /* lv_obj_create leaves SCROLLABLE + SCROLL_ELASTIC on. Text wider than
     * the 274 px content box made this banner draggable under a finger.
     * The shortened text below removes today's overflow; this makes any
     * future one clip instead of silently restoring the drag. */
    lv_obj_remove_flag(s_failed_banner, LV_OBJ_FLAG_SCROLLABLE);
    /* Text is filled in by screen_home_update once the validator has run —
     * this screen is built before the boot self-tests, so the failure reason
     * isn't known yet (and 'Update failed' read as a transfer error during
     * bench testing when the real cause was a noisy thermocouple). */
    s_failed_banner_lbl = lv_label_create(s_failed_banner);
    lv_label_set_text(s_failed_banner_lbl, LV_SYMBOL_CLOSE " Self-tests failed");
    lv_obj_set_style_text_color(s_failed_banner_lbl, lv_color_white(), 0);
    lv_obj_t *fl = s_failed_banner_lbl;
    lv_obj_center(fl);

    /* Big temp. THEME_FG (off-white) at boot/IDLE so the value is
     * readable; screen_home_update recolours for RUNNING/FAULT.
     * bg_opa TRANSP keeps the dark tab page showing through. */
    lbl_temp = lv_label_create(parent);
    lv_label_set_text(lbl_temp, "--°C");
    lv_obj_set_style_text_color(lbl_temp, THEME_FG, 0);
    lv_obj_set_style_text_font(lbl_temp, &lv_font_montserrat_48, 0);
    lv_obj_set_style_bg_opa(lbl_temp, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(lbl_temp, 0, 0);

    /* Target line */
    lbl_target = lv_label_create(parent);
    lv_label_set_text(lbl_target, "Ambient - ready to start");
    lv_obj_set_style_text_color(lbl_target, THEME_FG, 0);
    lv_obj_set_style_bg_opa(lbl_target, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(lbl_target, 0, 64);

    /* Stage chip — fixed full width so it doesn't pop as the time
     * digits change width across ticks. */
    lbl_stage = lv_label_create(parent);
    lv_label_set_text(lbl_stage, "Stage Idle");
    lv_obj_set_size(lbl_stage, GRAPH_W, 24);
    lv_obj_set_style_text_color(lbl_stage, THEME_FG, 0);
    lv_obj_set_style_text_align(lbl_stage, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(lbl_stage, THEME_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(lbl_stage, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(lbl_stage, 4, 0);
    lv_obj_set_style_pad_bottom(lbl_stage, 4, 0);
    lv_obj_set_pos(lbl_stage, 0, 90);

    /* === Graph === per spec §7.2.
     *   - vertical transition lines + tick labels at phase boundaries
     *   - horizontal lines at soak/reflow target temps + labels
     *   - planned curve in grey (THEME_FG_FAINT)
     *   - live measured trace in orange (THEME_ACCENT)
     *   - dimmed in FAULT (handled in screen_home_update)
     */
    graph_canvas = lv_obj_create(parent);
    lv_obj_set_size(graph_canvas, GRAPH_W, GRAPH_H);
    lv_obj_set_style_bg_color(graph_canvas, THEME_PANEL_BG, 0);
    lv_obj_set_style_border_color(graph_canvas, THEME_BORDER, 0);
    lv_obj_set_style_border_width(graph_canvas, 1, 0);
    lv_obj_set_style_pad_all(graph_canvas, 0, 0);
    lv_obj_clear_flag(graph_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(graph_canvas, 0, 126);

    /* Vertical transition gridlines (filled in by screen_home_set_curve). */
    for (int i = 0; i < 4; i++) {
        line_v_trans[i] = lv_line_create(graph_canvas);
        lv_obj_set_style_line_color(line_v_trans[i], THEME_BORDER, 0);
        lv_obj_set_style_line_width(line_v_trans[i], 1, 0);
        s_v_trans_pts[i][0].x = GRAPH_INSET_LEFT;
        s_v_trans_pts[i][0].y = GRAPH_INSET_TOP;
        s_v_trans_pts[i][1].x = GRAPH_INSET_LEFT;
        s_v_trans_pts[i][1].y = GRAPH_INSET_TOP + GRAPH_PLOT_H;
        lv_line_set_points(line_v_trans[i], s_v_trans_pts[i], 2);

        lbl_t_trans[i] = lv_label_create(graph_canvas);
        lv_label_set_text(lbl_t_trans[i], "");
        lv_obj_set_style_text_color(lbl_t_trans[i], THEME_FG_FAINT, 0);
        lv_obj_set_style_bg_opa(lbl_t_trans[i], LV_OPA_TRANSP, 0);
    }

    /* Horizontal soak/reflow target lines (filled in by set_curve). */
    line_h_soak = lv_line_create(graph_canvas);
    lv_obj_set_style_line_color(line_h_soak, THEME_BORDER, 0);
    lv_obj_set_style_line_width(line_h_soak, 1, 0);

    line_h_reflow = lv_line_create(graph_canvas);
    lv_obj_set_style_line_color(line_h_reflow, THEME_BORDER, 0);
    lv_obj_set_style_line_width(line_h_reflow, 1, 0);

    lbl_h_soak = lv_label_create(graph_canvas);
    lv_label_set_text(lbl_h_soak, "");
    lv_obj_set_style_text_color(lbl_h_soak, THEME_FG_FAINT, 0);
    lv_obj_set_style_bg_opa(lbl_h_soak, LV_OPA_TRANSP, 0);

    lbl_h_reflow = lv_label_create(graph_canvas);
    lv_label_set_text(lbl_h_reflow, "");
    lv_obj_set_style_text_color(lbl_h_reflow, THEME_FG_FAINT, 0);
    lv_obj_set_style_bg_opa(lbl_h_reflow, LV_OPA_TRANSP, 0);

    /* Y-axis rail labels (top = OVER_TEMP_LIMIT_C, bottom = 0 °C). */
    lbl_axis_y_top = lv_label_create(graph_canvas);
    char ytop[12];
    snprintf(ytop, sizeof(ytop), "%d °C", (int)OVER_TEMP_LIMIT_C);
    lv_label_set_text(lbl_axis_y_top, ytop);
    lv_obj_set_style_text_color(lbl_axis_y_top, THEME_FG_FAINT, 0);
    lv_obj_set_style_bg_opa(lbl_axis_y_top, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(lbl_axis_y_top, 2, GRAPH_INSET_TOP);

    lbl_axis_y_bot = lv_label_create(graph_canvas);
    lv_label_set_text(lbl_axis_y_bot, "0 °C");
    lv_obj_set_style_text_color(lbl_axis_y_bot, THEME_FG_FAINT, 0);
    lv_obj_set_style_bg_opa(lbl_axis_y_bot, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(lbl_axis_y_bot, 2, GRAPH_INSET_TOP + GRAPH_PLOT_H - 12);

    /* X-axis end labels. All bottom labels share GRAPH_X_LABEL_Y; the
     * right-edge label uses an estimated width to right-align inside
     * the canvas. */
    lbl_axis_t_min = lv_label_create(graph_canvas);
    lv_label_set_text(lbl_axis_t_min, "0s");
    lv_obj_set_style_text_color(lbl_axis_t_min, THEME_FG_FAINT, 0);
    lv_obj_set_style_bg_opa(lbl_axis_t_min, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(lbl_axis_t_min, GRAPH_INSET_LEFT, GRAPH_X_LABEL_Y);

    lbl_axis_t_max = lv_label_create(graph_canvas);
    lv_label_set_text(lbl_axis_t_max, "--");
    lv_obj_set_style_text_color(lbl_axis_t_max, THEME_FG_FAINT, 0);
    lv_obj_set_style_bg_opa(lbl_axis_t_max, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(lbl_axis_t_max,
                   GRAPH_W - GRAPH_INSET_RIGHT - (int)strlen("--") * LBL_CHAR_W_PX,
                   GRAPH_X_LABEL_Y);

    /* Planned curve line. Populated by screen_home_set_curve. */
    line_planned = lv_line_create(graph_canvas);
    lv_obj_set_style_line_color(line_planned, THEME_FG_FAINT, 0);
    lv_obj_set_style_line_width(line_planned, 2, 0);

    /* Live trace line. */
    line_live = lv_line_create(graph_canvas);
    lv_obj_set_style_line_color(line_live, THEME_ACCENT, 0);
    lv_obj_set_style_line_width(line_live, 2, 0);

    /* Action button */
    btn_action = lv_button_create(parent);
    lv_obj_set_size(btn_action, 296, 50);
    lv_obj_set_pos(btn_action, 0, 334);
    lv_obj_set_style_bg_color(btn_action, THEME_GO, 0);
    lv_obj_set_style_opa(btn_action, THEME_OPA_DISABLED, LV_STATE_DISABLED);
    lv_obj_add_event_cb(btn_action, action_clicked_cb, LV_EVENT_CLICKED, NULL);
    btn_action_lbl = lv_label_create(btn_action);
    lv_label_set_text(btn_action_lbl, "START");
    lv_obj_set_style_text_color(btn_action_lbl, THEME_FG, 0);
    lv_obj_center(btn_action_lbl);

}

/* Show or hide the reflow-only overlays in one place: four phase gridlines
 * with their tick labels, and the soak/reflow target lines with theirs. The
 * reflow render path decides label visibility for itself afterwards; this
 * only has to restore the LINES, which that path never hides. */
static void set_reflow_overlays_hidden(bool hide) {
    lv_obj_t *const objs[] = {
        line_v_trans[0], line_v_trans[1], line_v_trans[2], line_v_trans[3],
        lbl_t_trans[0],  lbl_t_trans[1],  lbl_t_trans[2],  lbl_t_trans[3],
        line_h_soak, line_h_reflow, lbl_h_soak, lbl_h_reflow,
    };
    for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
        if (hide) lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_remove_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Repaint the axis rail labels for whichever view is active. */
static void render_axis_rails(void) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d °C", (int)graph_temp_max());
    lv_label_set_text(lbl_axis_y_top, buf);

    if (s_dry_target_c > 0.0f) {
        /* Rolling window: the left edge is always DRY_GRAPH_WINDOW_MS ago and
         * the right edge is always now, so both labels are constants. */
        snprintf(buf, sizeof(buf), "-%u min",
                 (unsigned)(DRY_GRAPH_WINDOW_MS / 60000u));
        lv_label_set_text(lbl_axis_t_min, buf);
        lv_label_set_text(lbl_axis_t_max, "now");
        lv_obj_set_pos(lbl_axis_t_max,
                       GRAPH_W - GRAPH_INSET_RIGHT - (int)strlen("now") * LBL_CHAR_W_PX,
                       GRAPH_X_LABEL_Y);
    } else {
        /* The reflow path rewrites lbl_axis_t_max itself; only the left edge
         * has to be undone here. */
        lv_label_set_text(lbl_axis_t_min, "0s");
    }
    lv_obj_set_pos(lbl_axis_t_min, GRAPH_INSET_LEFT, GRAPH_X_LABEL_Y);
}

/* Drying view: the "planned curve" is a flat reference at the target. There is
 * no ramp segment to draw — the setpoint ramp is over within the first minutes
 * of a run measured in hours, and at 2 s per pixel-column it is not resolvable
 * anyway. */
static void render_dry_curve(void) {
    set_reflow_overlays_hidden(true);
    render_axis_rails();

    const int32_t y = graph_y_from_c(s_dry_target_c);
    s_plan_pts[0].x = GRAPH_INSET_LEFT;
    s_plan_pts[0].y = y;
    s_plan_pts[1].x = GRAPH_INSET_LEFT + GRAPH_PLOT_W;
    s_plan_pts[1].y = y;
    lv_line_set_points(line_planned, s_plan_pts, 2);
}

static void render_curve(const float *samples, uint16_t n,
                         const uint32_t phase_end_ms[]) {
    if (s_dry_target_c > 0.0f) {
        render_dry_curve();
        return;
    }
    set_reflow_overlays_hidden(false);
    render_axis_rails();

    /* Cycle window = end of planned cool-down (set by profile_curve_generate).
     * Fall back to end-of-reflow only if the cool-down field is 0
     * (i.e. the snapshot was published before the first curve gen). */
    uint32_t cycle_total_ms = phase_end_ms[STAGE_COOL_DOWN]
                            ? phase_end_ms[STAGE_COOL_DOWN]
                            : phase_end_ms[STAGE_REFLOW];
    s_cycle_total_ms = cycle_total_ms;

    if (n > PROFILE_CURVE_MAX_SAMPLES) n = PROFILE_CURVE_MAX_SAMPLES;
    if (n == 0 || cycle_total_ms == 0) {
        lv_line_set_points(line_planned, NULL, 0);
        lv_label_set_text(lbl_axis_t_max, "--");
        for (int i = 0; i < 4; i++) lv_label_set_text(lbl_t_trans[i], "");
        return;
    }

    /* Planned curve: each sample = PROFILE_CURVE_STEP_MS apart. The
     * curve now includes the cool-down tail (see profile_curve.c). */
    for (uint16_t i = 0; i < n; i++) {
        uint32_t t_ms = (uint32_t)i * PROFILE_CURVE_STEP_MS;
        s_plan_pts[i].x = graph_x_from_ms(t_ms, cycle_total_ms);
        s_plan_pts[i].y = graph_y_from_c(samples[i]);
    }
    lv_line_set_points(line_planned, s_plan_pts, n);

    /* Right-edge x-axis label = end of cool-down. */
    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "%us", (unsigned)(cycle_total_ms / 1000));
    lv_label_set_text(lbl_axis_t_max, tbuf);
    lv_obj_set_pos(lbl_axis_t_max,
                   GRAPH_W - GRAPH_INSET_RIGHT - (int)strlen(tbuf) * LBL_CHAR_W_PX,
                   GRAPH_X_LABEL_Y);

    /* Vertical transition lines + tick labels at the 4 phase
     * boundaries (end of heat-up 1, soak, heat-up 2, reflow).
     * Overlap detection: walk left-to-right, hide any label whose
     * left edge would land within 4 px of the previous visible
     * label's right edge (or of the bordering start/end labels). */
    int prev_right = GRAPH_INSET_LEFT + (int)strlen("0s") * LBL_CHAR_W_PX;
    int end_left = GRAPH_W - GRAPH_INSET_RIGHT - (int)strlen(tbuf) * LBL_CHAR_W_PX;

    for (int i = 0; i < 4; i++) {
        stage_t st = (stage_t)(STAGE_HEAT_UP_1 + i);
        uint32_t t_ms = phase_end_ms[st];
        int32_t x = graph_x_from_ms(t_ms, cycle_total_ms);
        s_v_trans_pts[i][0].x = x;
        s_v_trans_pts[i][0].y = GRAPH_INSET_TOP;
        s_v_trans_pts[i][1].x = x;
        s_v_trans_pts[i][1].y = GRAPH_INSET_TOP + GRAPH_PLOT_H;
        lv_line_set_points(line_v_trans[i], s_v_trans_pts[i], 2);

        char ttick[12];
        snprintf(ttick, sizeof(ttick), "%us", (unsigned)(t_ms / 1000));
        int w = (int)strlen(ttick) * LBL_CHAR_W_PX;
        int label_left = x - w / 2;
        int label_right = x + w / 2;
        bool overlaps = (label_left < prev_right + 4)
                     || (label_right > end_left - 4);
        if (overlaps) {
            lv_obj_add_flag(lbl_t_trans[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(lbl_t_trans[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lbl_t_trans[i], ttick);
            lv_obj_set_pos(lbl_t_trans[i], label_left, GRAPH_X_LABEL_Y);
            prev_right = label_right;
        }
    }

    /* Horizontal soak/reflow target temperature lines.
     * Derive each target from the curve sample at the END of the
     * corresponding heat-up stage — that sample is the ramp target
     * by construction (see profile_curve.c::append_ramp). */
    uint16_t idx_soak   = (phase_end_ms[STAGE_HEAT_UP_1] / PROFILE_CURVE_STEP_MS);
    uint16_t idx_reflow = (phase_end_ms[STAGE_HEAT_UP_2] / PROFILE_CURVE_STEP_MS);
    if (idx_soak   > 0 && idx_soak   <= n) idx_soak--;
    if (idx_reflow > 0 && idx_reflow <= n) idx_reflow--;
    float soak_temp   = (idx_soak   < n) ? samples[idx_soak]   : 0.0f;
    float reflow_temp = (idx_reflow < n) ? samples[idx_reflow] : 0.0f;

    int32_t y_soak   = graph_y_from_c(soak_temp);
    int32_t y_reflow = graph_y_from_c(reflow_temp);

    s_h_soak_pts[0].x = GRAPH_INSET_LEFT;
    s_h_soak_pts[0].y = y_soak;
    s_h_soak_pts[1].x = GRAPH_INSET_LEFT + GRAPH_PLOT_W;
    s_h_soak_pts[1].y = y_soak;
    lv_line_set_points(line_h_soak, s_h_soak_pts, 2);

    s_h_reflow_pts[0].x = GRAPH_INSET_LEFT;
    s_h_reflow_pts[0].y = y_reflow;
    s_h_reflow_pts[1].x = GRAPH_INSET_LEFT + GRAPH_PLOT_W;
    s_h_reflow_pts[1].y = y_reflow;
    lv_line_set_points(line_h_reflow, s_h_reflow_pts, 2);

    /* Y-axis label de-confliction. The fixed rail labels (0 / OVER_TEMP_LIMIT_C)
     * own their rows; a soak/reflow target label is hidden when its row would
     * collide with a rail label, or (soak) with the reflow label. The reference
     * LINE still draws, and the reflow target is also in the status line, so
     * nothing essential is lost — this just stops "240 °C" printing on top of
     * "260 °C" for high reflow targets. Rails keep priority so the axis bounds
     * stay readable. Two rows collide when their top-y differ by < a label height. */
    const int32_t y_top_row    = GRAPH_INSET_TOP;                      /* lbl_axis_y_top */
    const int32_t y_bot_row    = GRAPH_INSET_TOP + GRAPH_PLOT_H - 12;  /* lbl_axis_y_bot */
    const int32_t y_soak_row   = y_soak   - 6;
    const int32_t y_reflow_row = y_reflow - 6;
    #define ROWS_COLLIDE(a, b) (((a) - (b) < GRAPH_Y_LABEL_H) && ((b) - (a) < GRAPH_Y_LABEL_H))

    char tlabel[16];
    snprintf(tlabel, sizeof(tlabel), "%d °C", (int)reflow_temp);
    lv_label_set_text(lbl_h_reflow, tlabel);
    lv_obj_set_pos(lbl_h_reflow, 2, y_reflow_row);
    bool reflow_lbl_shown = !ROWS_COLLIDE(y_reflow_row, y_top_row)
                         && !ROWS_COLLIDE(y_reflow_row, y_bot_row);
    if (reflow_lbl_shown) lv_obj_remove_flag(lbl_h_reflow, LV_OBJ_FLAG_HIDDEN);
    else                  lv_obj_add_flag(lbl_h_reflow, LV_OBJ_FLAG_HIDDEN);

    snprintf(tlabel, sizeof(tlabel), "%d °C", (int)soak_temp);
    lv_label_set_text(lbl_h_soak, tlabel);
    lv_obj_set_pos(lbl_h_soak, 2, y_soak_row);
    bool soak_lbl_shown = !ROWS_COLLIDE(y_soak_row, y_top_row)
                       && !ROWS_COLLIDE(y_soak_row, y_bot_row)
                       && !(reflow_lbl_shown && ROWS_COLLIDE(y_soak_row, y_reflow_row));
    if (soak_lbl_shown) lv_obj_remove_flag(lbl_h_soak, LV_OBJ_FLAG_HIDDEN);
    else                lv_obj_add_flag(lbl_h_soak, LV_OBJ_FLAG_HIDDEN);
    #undef ROWS_COLLIDE
}

void screen_home_set_curve(const float *samples, uint16_t n,
                           const uint32_t phase_end_ms[]) {
    s_last_samples = samples;
    s_last_n       = n;
    memcpy(s_last_phase_end_ms, phase_end_ms, sizeof(s_last_phase_end_ms));
    render_curve(samples, n, phase_end_ms);
}

void screen_home_set_dry_view(float target_c) {
    const float want = (target_c > 0.0f) ? target_c : 0.0f;
    if (want == s_dry_target_c) return;   /* idempotent; called every tick */
    s_dry_target_c = want;

    /* Both axes change meaning, so the pixels already on screen no longer
     * describe anything real. Clearing is the only honest option. */
    s_live_n = 0;
    s_live_last_idx = UINT32_MAX;
    lv_line_set_points(line_live, NULL, 0);

    if (s_last_samples) {
        render_curve(s_last_samples, s_last_n, s_last_phase_end_ms);
    } else {
        /* No curve has ever been published (dry started before the first
         * profile publish). The drying view needs none; the reflow view has
         * nothing to draw either way. */
        if (s_dry_target_c > 0.0f) render_dry_curve();
    }
}

void screen_home_update(const oven_state_t *s) {
    s_mode_cached = s->mode;

    /* Recolour both lines based on mode (FAULT dims everything to
     * convey "this trace is frozen / not authoritative"). */
    lv_color_t plan_col = (s->mode == MODE_FAULT) ? THEME_BORDER  : THEME_FG_FAINT;
    lv_color_t live_col = (s->mode == MODE_FAULT) ? THEME_FG_DIM  : THEME_ACCENT;
    lv_obj_set_style_line_color(line_planned, plan_col, 0);
    lv_obj_set_style_line_color(line_live,    live_col, 0);

    if (s->mode == MODE_RUNNING) {
        /* Decimate to PROFILE_CURVE_STEP_MS so the live trace matches
         * the planned curve's temporal resolution (one sample per
         * planned-step bucket). The ring is sized to match. */
        uint32_t idx = s->elapsed_ms / PROFILE_CURVE_STEP_MS;
        if (idx != s_live_last_idx) {
            s_live_last_idx = idx;
            int32_t x = graph_x_from_ms(s->elapsed_ms, s_cycle_total_ms);
            int32_t y = graph_y_from_c(s->temp_c);
            if (s_live_n < GRAPH_LIVE_RING_CAP) {
                s_live_pts[s_live_n].x = x;
                s_live_pts[s_live_n].y = y;
                s_live_n++;
            } else {
                memmove(s_live_pts, s_live_pts + 1,
                        (GRAPH_LIVE_RING_CAP - 1) * sizeof(s_live_pts[0]));
                s_live_pts[GRAPH_LIVE_RING_CAP - 1].x = x;
                s_live_pts[GRAPH_LIVE_RING_CAP - 1].y = y;
            }
            lv_line_set_points(line_live, s_live_pts, s_live_n);
        }
    } else if (s->mode == MODE_DRYING) {
        /* Rolling window, coarser cadence: 30 min at 2 s = 900 of the ring's
         * 1600 slots. At the reflow cadence a 16 h dry would overrun the ring
         * 36 times over. */
        uint32_t idx = s->elapsed_ms / DRY_GRAPH_SAMPLE_MS;
        if (idx != s_live_last_idx) {
            s_live_last_idx = idx;
            int32_t y = graph_y_from_c(s->temp_c);
            if (s_live_n < DRY_GRAPH_SAMPLES) {
                s_live_pts[s_live_n].x = dry_x_from_slot(s_live_n);
                s_live_pts[s_live_n].y = y;
                s_live_n++;
            } else {
                /* Scroll by one slot. Only the Y values move: every slot's X
                 * is dry_x_from_slot(slot) and was written during the fill, so
                 * shifting the whole point would carry the wrong X along and
                 * bunch the trace against the right edge. */
                for (uint16_t i = 0; i + 1 < DRY_GRAPH_SAMPLES; i++) {
                    s_live_pts[i].y = s_live_pts[i + 1].y;
                }
                s_live_pts[DRY_GRAPH_SAMPLES - 1].y = y;
            }
            lv_line_set_points(line_live, s_live_pts, s_live_n);
        }
    } else if (s->mode == MODE_IDLE) {
        /* Returning to IDLE — clear the live trace so the next run
         * starts from an empty canvas. */
        s_live_n = 0;
        s_live_last_idx = UINT32_MAX;
        lv_line_set_points(line_live, NULL, 0);
    }
    /* MODE_FAULT: leave the live trace as it stood at the moment of
     * the fault (already drawn, just recoloured). */

    char buf[80];
    snprintf(buf, sizeof(buf), "%.0f°C", s->temp_c);
    lv_label_set_text(lbl_temp, buf);

    switch (s->mode) {
    case MODE_IDLE:
        if (s->temp_c > RUN_START_MAX_TEMP_C) {
            /* START is refused above this temperature (control task enforces it);
             * say why and how, so the disabled button isn't a mystery. */
            snprintf(buf, sizeof(buf), "Too hot (%.0f°C) - cool below %d°C to start",
                     s->temp_c, (int)RUN_START_MAX_TEMP_C);
            lv_label_set_text(lbl_target, buf);
        } else {
            lv_label_set_text(lbl_target, "Ambient - ready to start");
        }
        lv_label_set_text(lbl_stage,  "Stage Idle");
        lv_label_set_text(btn_action_lbl, "START");
        lv_obj_set_style_bg_color(btn_action, THEME_GO, 0);
        lv_obj_set_style_text_color(lbl_temp, THEME_FG, 0);
        break;
    case MODE_RUNNING: {
        /* Cast uint32_t to unsigned for %u (xtensa-gcc treats uint32_t as
         * long unsigned; bare %u would trip -Werror=format). */
        snprintf(buf, sizeof(buf), "Target %.0f °C - elapsed %u:%02u",
                 s->setpoint_c,
                 (unsigned)(s->elapsed_ms / 60000),
                 (unsigned)((s->elapsed_ms / 1000) % 60));
        lv_label_set_text(lbl_target, buf);

        /* Stage chip per spec §7.2.
         * stage_start = phase_end_ms[stage-1] (or 0 for the first stage);
         * stage_dur   = phase_end_ms[stage] - stage_start.
         * Cool-down has no planned duration — show elapsed only. */
        const char *name = stage_name(s->stage);
        if (s->stage == STAGE_COOL_DOWN) {
            uint32_t stage_start = s->phase_end_ms[STAGE_REFLOW];
            uint32_t stage_elapsed = (s->elapsed_ms > stage_start)
                                    ? (s->elapsed_ms - stage_start) : 0;
            snprintf(buf, sizeof(buf), "Stage %s - %us",
                     name, (unsigned)(stage_elapsed / 1000));
        } else if (s->stage >= STAGE_HEAT_UP_1 && s->stage < STAGE_COOL_DOWN) {
            uint32_t stage_start = (s->stage > STAGE_HEAT_UP_1)
                                 ? s->phase_end_ms[s->stage - 1] : 0;
            uint32_t stage_dur = s->phase_end_ms[s->stage] - stage_start;
            uint32_t stage_elapsed = (s->elapsed_ms > stage_start)
                                    ? (s->elapsed_ms - stage_start) : 0;
            snprintf(buf, sizeof(buf), "Stage %s - %u / %u s",
                     name,
                     (unsigned)(stage_elapsed / 1000),
                     (unsigned)(stage_dur / 1000));
        } else {
            snprintf(buf, sizeof(buf), "Stage %s", name);
        }
        lv_label_set_text(lbl_stage, buf);

        lv_label_set_text(btn_action_lbl, "STOP");
        lv_obj_set_style_bg_color(btn_action, THEME_STOP, 0);
        lv_obj_set_style_text_color(lbl_temp, THEME_ACCENT, 0);
        break;
    }
    case MODE_DRYING:
        snprintf(buf, sizeof(buf), "Drying %.0f °C - setpoint %.0f °C",
                 s->dry_target_c, s->setpoint_c);
        lv_label_set_text(lbl_target, buf);

        /* Hold progress is the number the user actually wants: a dry runs for
         * hours and "elapsed" alone does not say how much longer. During
         * heat-up there is no answer yet, so say so rather than show 0:00. */
        if (s->dry_stage == DRY_STAGE_HOLD) {
            snprintf(buf, sizeof(buf), "Stage Hold - %u:%02u / %u:%02u",
                     (unsigned)(s->dry_hold_elapsed_ms / 3600000u),
                     (unsigned)((s->dry_hold_elapsed_ms / 60000u) % 60u),
                     (unsigned)(s->dry_hold_ms / 3600000u),
                     (unsigned)((s->dry_hold_ms / 60000u) % 60u));
        } else {
            snprintf(buf, sizeof(buf), "Stage Heat-up - %u:%02u",
                     (unsigned)(s->elapsed_ms / 60000u),
                     (unsigned)((s->elapsed_ms / 1000u) % 60u));
        }
        lv_label_set_text(lbl_stage, buf);

        lv_label_set_text(btn_action_lbl, "STOP");
        lv_obj_set_style_bg_color(btn_action, THEME_STOP, 0);
        lv_obj_set_style_text_color(lbl_temp, THEME_ACCENT, 0);
        break;
    case MODE_STEP_TEST:
        /* Open-loop PID step test (started from Settings → PID Tuning).
         * Show live status + a working STOP on the home screen too. */
        snprintf(buf, sizeof(buf), "Step test %u%% - elapsed %u:%02u",
                 (unsigned)s->step_duty,
                 (unsigned)(s->elapsed_ms / 60000),
                 (unsigned)((s->elapsed_ms / 1000) % 60));
        lv_label_set_text(lbl_target, buf);
        lv_label_set_text(lbl_stage,  "Stage Step test");
        lv_label_set_text(btn_action_lbl, "STOP");
        lv_obj_set_style_bg_color(btn_action, THEME_STOP, 0);
        lv_obj_set_style_text_color(lbl_temp, THEME_ACCENT, 0);
        break;
    case MODE_FAULT:
        snprintf(buf, sizeof(buf), "FAULT: %s",
                 fault_text(s->fault_flags));
        lv_label_set_text(lbl_target, buf);
        lv_label_set_text(lbl_stage,  "Tap ACKNOWLEDGE to clear");
        lv_label_set_text(btn_action_lbl, "ACKNOWLEDGE");
        lv_obj_set_style_bg_color(btn_action, THEME_STAGE, 0);
        lv_obj_set_style_text_color(lbl_temp, THEME_STOP, 0);
        break;
    case MODE_PENDING_VALIDATION:
        if (boot_status_validation() == BOOT_VALIDATION_FAILED) {
            /* Include WHICH self-test failed (tc/ssr/display/touch/...), so a
             * sensor problem on the bench isn't mistaken for a bad image. The
             * reason is constant after boot; set the labels once, not per tick
             * (lv_label_set_text reallocs on this heap-tight device). */
            static bool s_failed_texts_set = false;
            if (!s_failed_texts_set) {
                s_failed_texts_set = true;
                const char *why = ota_validator_last_failure();
                lv_label_set_text_fmt(lbl_target, "Self-tests failed: %s", why);
                lv_label_set_text_fmt(s_failed_banner_lbl,
                                      LV_SYMBOL_CLOSE " Self-tests failed (%s)", why);
            }
            lv_label_set_text(lbl_stage,  "Reboot to roll back");
        } else if (s_prev_version[0]) {
            lv_label_set_text_fmt(lbl_target, "%s " LV_SYMBOL_RIGHT " %s",
                                  s_prev_version, FIRMWARE_VERSION_STR);
            lv_label_set_text(lbl_stage,  "Confirm update first");
        } else {
            lv_label_set_text(lbl_target, "New firmware");
            lv_label_set_text(lbl_stage,  "Confirm update first");
        }
        lv_label_set_text(btn_action_lbl, "START");
        lv_obj_set_style_bg_color(btn_action, THEME_GO, 0);
        break;
    }

    /* Disable the START button while a firmware update is pending validation,
     * or while the oven is too hot to begin a run (control task enforces both;
     * this + the action_clicked_cb gate are defense-in-depth). */
    bool block_start = (s->mode == MODE_PENDING_VALIDATION)
                    || (s->mode == MODE_IDLE && s->temp_c > RUN_START_MAX_TEMP_C);
    if (block_start) lv_obj_add_state(btn_action, LV_STATE_DISABLED);
    else             lv_obj_remove_state(btn_action, LV_STATE_DISABLED);

    /* Banner visibility: pending (orange) xor failed (red) xor neither.
     * Both block START via MODE_PENDING_VALIDATION; the cached boot-time
     * validation result picks the banner (no per-tick OTA flash read). */
    if (s->mode == MODE_PENDING_VALIDATION
        && boot_status_validation() == BOOT_VALIDATION_FAILED) {
        lv_obj_clear_flag(s_failed_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pending_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_failed_banner);          /* sit above the temp label */
        lv_obj_add_flag(lbl_temp, LV_OBJ_FLAG_HIDDEN);    /* banner owns the top strip */
    } else if (s->mode == MODE_PENDING_VALIDATION) {
        lv_obj_clear_flag(s_pending_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_failed_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_pending_banner);
        lv_obj_add_flag(lbl_temp, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_pending_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_failed_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_temp, LV_OBJ_FLAG_HIDDEN);
    }
}

#include "ui/screen_keypad.h"
#include "ui/theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Modal geometry. The content box is the modal minus padding AND border on
 * each side — lv_obj_get_style_space_left() counts both — so the border width
 * is pinned here instead of inherited from the theme, where it is DPI-dependent
 * (LV_DPX_CALC(dpi, 2), which is 2 at the configured LV_DPI_DEF of 130 but
 * would change the content width on another display). */
#define KP_MODAL_W   320
#define KP_MODAL_H   320
#define KP_PAD         8
#define KP_BORDER      2
#define KP_CONTENT_W  (KP_MODAL_W - 2 * (KP_PAD + KP_BORDER))   /* 300 */

/* Key grid. One lv_buttonmatrix draws all 15 keys, replacing the 16 buttons
 * and 16 child labels this screen used to build — ~32 objects down to one.
 *
 * That is the whole point of the widget here, not tidiness: the LVGL pool is a
 * fixed 64 KB arena resting at ~72% because every screen is pre-built and only
 * hidden, and an open keypad used to push it to 97%. Two separate crashes were
 * traced to allocations failing at that peak (lv_draw_label.c:648 for a glyph
 * bitmap, lv_draw_sw_mask.c:1096 for a rounded-corner mask), both surfacing as
 * LV_ASSERT_MALLOC — a silent while(1) in the UI task before the assert trap.
 *
 * Sizes are chosen so LVGL's own division reproduces the previous 70x50 keys:
 * (298 - 3*6)/4 = 70 wide, (218 - 3*6)/4 = 50 high. */
#define KP_GRID_X      1                       /* centres 298 in the 300 box */
#define KP_GRID_Y     80
#define KP_GRID_W    298
#define KP_GRID_H    218
#define KP_GAP         6

_Static_assert(KP_GRID_X >= 0 && KP_GRID_X + KP_GRID_W <= KP_CONTENT_W,
               "keypad grid overflows the modal content box horizontally");
_Static_assert(KP_GRID_Y + KP_GRID_H <= KP_MODAL_H - 2 * (KP_PAD + KP_BORDER),
               "keypad grid overflows the modal content box vertically");

/* Flat button indices into kp_map, skipping the "\n" row breaks:
 *    0  1  2  3     7   8   9  DEL
 *    4  5  6  7     4   5   6  CLR
 *    8  9 10 11     1   2   3  ESC
 *   12 13 14        0   .   OK(span 2) */
#define KP_ID_DEL   3
#define KP_ID_CLR   7
#define KP_ID_ESC  11
#define KP_ID_OK   14
#define KP_ID_MAX  KP_ID_OK

static const char *kp_map[] = {
    "7", "8", "9", "DEL", "\n",
    "4", "5", "6", "CLR", "\n",
    "1", "2", "3", "ESC", "\n",
    "0", ".", "OK", ""
};

static lv_obj_t *s_modal;
static lv_obj_t *s_disp;        /* big right-aligned value label; stored
                                   directly rather than fetched by child
                                   index so adding new modal children
                                   doesn't break update_display_label. */
static char      s_buf[16];
static keypad_config_t s_cfg;

static void update_display_label(void) {
    lv_label_set_text(s_disp, s_buf[0] ? s_buf : "0");
}

static void close_btn_cb(lv_event_t *e) {
    (void)e;
    screen_keypad_close();
}

static void handle_key(const char *key) {
    if (strcmp(key, "OK") == 0) {
        float v = (s_buf[0]) ? strtof(s_buf, NULL) : s_cfg.initial;
        if (v < s_cfg.min) v = s_cfg.min;
        if (v > s_cfg.max) v = s_cfg.max;
        keypad_done_cb cb = s_cfg.on_ok;
        void *ud = s_cfg.user_data;
        screen_keypad_close();
        if (cb) cb(v, ud);
        return;
    }
    if (strcmp(key, "ESC") == 0) { screen_keypad_close(); return; }
    if (strcmp(key, "CLR") == 0) { s_buf[0] = 0; update_display_label(); return; }
    if (strcmp(key, "DEL") == 0) {
        size_t n = strlen(s_buf);
        if (n > 0) s_buf[n - 1] = 0;
        update_display_label();
        return;
    }
    /* Digit or decimal */
    size_t n = strlen(s_buf);
    if (n >= sizeof(s_buf) - 1) return;
    if (key[0] == '.' && (!s_cfg.allow_decimal || strchr(s_buf, '.'))) return;
    s_buf[n] = key[0];
    s_buf[n + 1] = 0;
    update_display_label();
}

static void keys_cb(lv_event_t *e) {
    lv_obj_t *btnm = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(btnm);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *key = lv_buttonmatrix_get_button_text(btnm, id);
    if (key) handle_key(key);
}

static lv_color_t key_color(uint32_t id) {
    if (id == KP_ID_OK) return THEME_GO;
    if (id == KP_ID_DEL || id == KP_ID_CLR || id == KP_ID_ESC) return THEME_STOP;
    return THEME_BTN_NEUTRAL;
}

/* A button matrix draws every key from one style, so the per-key colouring the
 * old individual buttons had has to be reapplied at draw time. This is the
 * mechanism lv_calendar uses for the same reason: opt in with
 * LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS and recolour by button index. */
static void key_draw_cb(lv_event_t *e) {
    lv_draw_task_t *t = lv_event_get_param(e);
    if (lv_draw_task_get_type(t) != LV_DRAW_TASK_TYPE_FILL) return;

    lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(t);
    if (!base || base->part != LV_PART_ITEMS || base->id1 > KP_ID_MAX) return;

    lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(t);
    if (fill) fill->color = key_color(base->id1);
}

void screen_keypad_open(const keypad_config_t *cfg) {
    if (s_modal) screen_keypad_close();
    s_cfg = *cfg;
    snprintf(s_buf, sizeof(s_buf), "%g", cfg->initial);

    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_modal, KP_MODAL_W, KP_MODAL_H);
    lv_obj_align(s_modal, LV_ALIGN_BOTTOM_MID, 0, -36);   /* sits above tab bar */
    lv_obj_set_style_bg_color(s_modal, THEME_MODAL_BG, 0);
    lv_obj_set_style_border_color(s_modal, THEME_BORDER, 0);
    lv_obj_set_style_border_width(s_modal, KP_BORDER, 0);
    lv_obj_set_style_pad_all(s_modal, KP_PAD, 0);
    /* A fixed-size keypad has nothing to scroll, and lv_obj_create turns on
     * SCROLLABLE + SCROLL_ELASTIC + SCROLL_MOMENTUM by default. Left on, any
     * overflow lets a press that drifts a pixel drag the whole modal and
     * spring it back — the keypad "wiping" under a typing finger. */
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);

    /* Header. ASCII separators only — the LVGL Montserrat build doesn't
     * ship U+00B7 (·) by default, so it would render as placeholder
     * rectangles between field name, unit, and range. */
    lv_obj_t *hdr = lv_label_create(s_modal);
    char hdrbuf[128];
    snprintf(hdrbuf, sizeof(hdrbuf), "%s - %s - %g to %g",
             cfg->field_label, cfg->unit, cfg->min, cfg->max);
    lv_label_set_text(hdr, hdrbuf);
    lv_obj_set_style_text_color(hdr, THEME_FG_DIM, 0);
    lv_obj_set_pos(hdr, 4, 4);

    /* Close (X) button — top-right of the modal. Lets the user dismiss
     * without scanning the grid for ESC. */
    lv_obj_t *xbtn = lv_button_create(s_modal);
    lv_obj_set_size(xbtn, 28, 28);
    lv_obj_align(xbtn, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_set_style_bg_color(xbtn, THEME_STOP, 0);
    lv_obj_set_style_radius(xbtn, 14, 0);
    lv_obj_add_event_cb(xbtn, close_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(xbtn);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xl, THEME_FG, 0);
    lv_obj_center(xl);

    /* Display label. Sits below the X button (which spans y=4 to y=32). */
    s_disp = lv_label_create(s_modal);
    lv_label_set_text(s_disp, s_buf);
    lv_obj_set_style_text_color(s_disp, THEME_ACCENT, 0);
    lv_obj_set_style_text_font(s_disp, &lv_font_montserrat_28, 0);
    lv_obj_align(s_disp, LV_ALIGN_TOP_RIGHT, -8, 40);

    lv_obj_t *keys = lv_buttonmatrix_create(s_modal);
    lv_buttonmatrix_set_map(keys, kp_map);
    lv_obj_set_size(keys, KP_GRID_W, KP_GRID_H);
    lv_obj_set_pos(keys, KP_GRID_X, KP_GRID_Y);
    lv_buttonmatrix_set_button_width(keys, KP_ID_OK, 2);   /* OK spans two columns */

    /* CLICK_TRIG defaults off, which would fire on press and again on
     * long-press repeat — a held key would type duplicate digits. Setting both
     * gives exactly one event per press, on release, matching the
     * LV_EVENT_CLICKED semantics the individual buttons had. */
    for (uint32_t i = 0; i <= KP_ID_MAX; i++) {
        lv_buttonmatrix_set_button_ctrl(keys, i, LV_BUTTONMATRIX_CTRL_CLICK_TRIG
                                                 | LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    }

    /* The matrix itself is only a container for the keys; the modal already
     * paints the background behind it. */
    lv_obj_set_style_bg_opa(keys, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(keys, 0, 0);
    lv_obj_set_style_pad_all(keys, 0, 0);
    lv_obj_set_style_pad_row(keys, KP_GAP, 0);
    lv_obj_set_style_pad_column(keys, KP_GAP, 0);
    lv_obj_remove_flag(keys, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_radius(keys, 3, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(keys, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keys, THEME_BTN_NEUTRAL, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keys, THEME_FG, LV_PART_ITEMS);

    lv_obj_add_flag(keys, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(keys, key_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_event_cb(keys, keys_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

void screen_keypad_close(void) {
    if (s_modal) { lv_obj_delete(s_modal); s_modal = NULL; }
}

bool screen_keypad_is_open(void) { return s_modal != NULL; }

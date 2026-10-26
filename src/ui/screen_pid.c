#include "ui/screen_pid.h"
#include "ui/screen_settings.h"
#include "ui/theme.h"
#include "ui/screen_keypad.h"
#include "oven_state.h"
#include "config.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern QueueHandle_t g_cmd_queue;

typedef enum { F_KP, F_KI, F_KD, F_KHOLD, F_KRATE, F_COUNT_ } pid_field_id_t;

static pid_gains_t s_buf;
static lv_obj_t   *s_val_labels[F_COUNT_];
static lv_obj_t   *s_saved_label;
static uint16_t    s_last_seen_pid_seq = 0;

/* Pending initial values — set before the sub-screen is first created. */
static pid_gains_t s_pending_initial;
static bool        s_initial_pending = false;

/* Layout constants — mirror screen_profile.c so the two editor screens
 * share the same card / sub-row visual language. */
#define ROW_H              34
#define ROW_LABEL_X        12
#define ROW_VALUE_X       146
#define ROW_VALUE_W        76
#define ROW_VALUE_H        28
#define ROW_UNIT_X        232
#define CARD_W            296
#define CARD_TITLE_H       22

static void refresh_label(pid_field_id_t id) {
    char buf[16];
    float v = (id == F_KP) ? s_buf.kp : (id == F_KI) ? s_buf.ki
            : (id == F_KD) ? s_buf.kd : (id == F_KHOLD) ? s_buf.k_hold
            : s_buf.k_rate;
    /* Ki needs more digits (identified values ~0.007); K.rate none (~71). */
    snprintf(buf, sizeof(buf), (id == F_KI) ? "%.4f"
                             : (id == F_KRATE) ? "%.0f" : "%.2f", v);
    lv_label_set_text(s_val_labels[id], buf);
}

static lv_timer_t *s_saved_timer = NULL;
static void clear_saved(lv_timer_t *t) {
    lv_obj_add_flag(s_saved_label, LV_OBJ_FLAG_HIDDEN);
    lv_timer_delete(t);
    s_saved_timer = NULL;
}
static void show_saved(void) {
    lv_obj_remove_flag(s_saved_label, LV_OBJ_FLAG_HIDDEN);
    /* Cancel any in-flight fade timer so rapid re-saves don't leak timers
     * (and the toast stays visible a full interval after the last save) (#41). */
    if (s_saved_timer) lv_timer_delete(s_saved_timer);
    s_saved_timer = lv_timer_create(clear_saved, 1500, NULL);
}

static void keypad_done(float value, void *user_data) {
    pid_field_id_t id = (pid_field_id_t)(intptr_t)user_data;
    if      (id == F_KP)    s_buf.kp     = value;
    else if (id == F_KI)    s_buf.ki     = value;
    else if (id == F_KD)    s_buf.kd     = value;
    else if (id == F_KHOLD) s_buf.k_hold = value;
    else                    s_buf.k_rate = value;
    refresh_label(id);
    cmd_msg_t msg = {.type = CMD_APPLY_PID, .payload.pid = s_buf};
    xQueueSend(g_cmd_queue, &msg, 0);
    /* Toast fires from screen_pid_update once controller bumps
     * pid_applied_seq — i.e. once NVS save has actually succeeded. */
}

static void field_clicked_cb(lv_event_t *e) {
    pid_field_id_t id = (pid_field_id_t)(intptr_t)lv_event_get_user_data(e);
    keypad_config_t cfg = {
        .on_ok = keypad_done, .user_data = (void *)(intptr_t)id, .allow_decimal = true,
    };
    switch (id) {
    case F_KP: cfg.field_label = "Kp"; cfg.unit = ""; cfg.min = KP_MIN; cfg.max = KP_MAX; cfg.initial = s_buf.kp; break;
    case F_KI: cfg.field_label = "Ki"; cfg.unit = ""; cfg.min = KI_MIN; cfg.max = KI_MAX; cfg.initial = s_buf.ki; break;
    case F_KD: cfg.field_label = "Kd"; cfg.unit = ""; cfg.min = KD_MIN; cfg.max = KD_MAX; cfg.initial = s_buf.kd; break;
    case F_KHOLD: cfg.field_label = "K.hold"; cfg.unit = "%/C";
        cfg.min = KFF_HOLD_MIN; cfg.max = KFF_HOLD_MAX; cfg.initial = s_buf.k_hold; break;
    case F_KRATE: cfg.field_label = "K.rate"; cfg.unit = "%s/C";
        cfg.min = KFF_RATE_MIN; cfg.max = KFF_RATE_MAX; cfg.initial = s_buf.k_rate; break;
    default: return;
    }
    screen_keypad_open(&cfg);
}

/* Sub-row: <label> [value chip] <unit>. Same template as
 * screen_profile.c::make_sub_row so the two editor screens look
 * identical. unit may be "" (PID gains are dimensionless). */
static void make_sub_row(lv_obj_t *card, int y,
                         const char *label, const char *unit,
                         pid_field_id_t id) {
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_size(row, CARD_W, ROW_H);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, field_clicked_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)id);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, THEME_FG_DIM, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, ROW_LABEL_X, 0);

    lv_obj_t *vchip = lv_obj_create(row);
    lv_obj_set_size(vchip, ROW_VALUE_W, ROW_VALUE_H);
    lv_obj_set_pos(vchip, ROW_VALUE_X, (ROW_H - ROW_VALUE_H) / 2);
    lv_obj_set_style_bg_color(vchip, THEME_BG, 0);
    lv_obj_set_style_bg_opa(vchip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(vchip, THEME_FG_FAINT, 0);
    lv_obj_set_style_border_width(vchip, 1, 0);
    lv_obj_set_style_pad_all(vchip, 0, 0);
    lv_obj_clear_flag(vchip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(vchip, field_clicked_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)id);

    lv_obj_t *v = lv_label_create(vchip);
    lv_obj_set_style_text_color(v, THEME_ACCENT, 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_20, 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_TRANSP, 0);
    lv_obj_center(v);
    s_val_labels[id] = v;

    if (unit && unit[0]) {
        lv_obj_t *u = lv_label_create(row);
        lv_label_set_text(u, unit);
        lv_obj_set_style_text_color(u, THEME_FG_FAINT, 0);
        lv_obj_set_style_bg_opa(u, LV_OPA_TRANSP, 0);
        lv_obj_align(u, LV_ALIGN_LEFT_MID, ROW_UNIT_X, 0);
    }
}

/* One card, five sub-rows: three feedback gains + two feedforward
 * coefficients. Five per-gain title cards (328 px) would still fit the
 * 372 px content area, but the single card halves the LVGL object count
 * against the 64 KB pool and keeps related fields visually grouped. */
static void make_gains_card(lv_obj_t *parent, int y) {
    int card_h = CARD_TITLE_H + 5 * ROW_H + 2;
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, CARD_W, card_h);
    lv_obj_set_pos(card, 0, y);
    lv_obj_set_style_bg_color(card, THEME_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, THEME_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, "PID + Feedforward");
    lv_obj_set_style_text_color(t, THEME_FG, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(t, 10, 4);

    make_sub_row(card, CARD_TITLE_H + 0 * ROW_H, "Kp",     "",     F_KP);
    make_sub_row(card, CARD_TITLE_H + 1 * ROW_H, "Ki",     "",     F_KI);
    make_sub_row(card, CARD_TITLE_H + 2 * ROW_H, "Kd",     "",     F_KD);
    make_sub_row(card, CARD_TITLE_H + 3 * ROW_H, "K.hold", "%/C",  F_KHOLD);
    make_sub_row(card, CARD_TITLE_H + 4 * ROW_H, "K.rate", "%s/C", F_KRATE);
}

void screen_pid_create(lv_obj_t *parent) {
    lv_obj_set_style_bg_color(parent, THEME_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(parent, 8, 0);

    /* "Saved" toast — hidden by default, anchored top-right. */
    s_saved_label = lv_label_create(parent);
    lv_label_set_text(s_saved_label, LV_SYMBOL_OK " saved");
    lv_obj_set_style_text_color(s_saved_label, THEME_OK, 0);
    lv_obj_set_style_bg_opa(s_saved_label, LV_OPA_TRANSP, 0);
    lv_obj_align(s_saved_label, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_add_flag(s_saved_label, LV_OBJ_FLAG_HIDDEN);

    /* Gains card. Starts below the saved-toast strip so the toast
     * doesn't collide with the card border. */
    make_gains_card(parent, 22);

    /* Apply any initial values that were stashed before the sub-screen
     * was created (lazy creation pattern). */
    if (s_initial_pending) {
        s_buf = s_pending_initial;
        s_initial_pending = false;
    }
    for (int f = 0; f < F_COUNT_; f++) refresh_label((pid_field_id_t)f);
}

void screen_pid_set_initial(const pid_gains_t *g) {
    s_buf = *g;
    if (s_val_labels[0] == NULL) {
        /* Sub-screen not yet created — stash for deferred apply. */
        s_pending_initial = *g;
        s_initial_pending = true;
        return;
    }
    for (int f = 0; f < F_COUNT_; f++) refresh_label((pid_field_id_t)f);
}

void screen_pid_update(const oven_state_t *snap) {
    if (s_saved_label == NULL) return;  /* sub-screen not yet created */
    if (snap->pid_applied_seq != s_last_seen_pid_seq) {
        s_last_seen_pid_seq = snap->pid_applied_seq;
        /* The save may have come from outside this screen — the tuning
         * screen's browser-suggestion confirm dialog applies gains this
         * editor never saw. Resync the displayed set from the controller's
         * published active gains instead of trusting our local buffer. */
        s_buf = snap->pid_active_gains;
        for (int f = 0; f < F_COUNT_; f++) refresh_label((pid_field_id_t)f);
        show_saved();
    }
}

lv_obj_t *screen_pid_subscreen_create(void) {
    lv_obj_t *sub = settings_make_subscreen("PID");
    lv_obj_t *inner = lv_obj_create(sub);
    /* Width = full sub content (LV_HOR_RES - 2×4 sub pad); at x=0 it spans the
     * sub exactly. screen_pid_create() pads this by 8, leaving CARD_W=296 of
     * usable width — the cards fit with no horizontal overflow. Height leaves
     * room under the title bar and an 8 px bottom margin (was overflowing both
     * axes at (4,36)/380, which raised both scrollbars). */
    lv_obj_set_size(inner, LV_HOR_RES - 8, LV_VER_RES - 24 - 36 - 48);
    lv_obj_set_pos(inner, 0, 36);
    lv_obj_set_style_bg_opa(inner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(inner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(inner, 0, 0);
    lv_obj_clear_flag(inner, LV_OBJ_FLAG_SCROLLABLE);
    screen_pid_create(inner);
    return sub;
}

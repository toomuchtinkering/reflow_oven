#include "ui/screen_profile.h"
#include "ui/theme.h"
#include "ui/screen_keypad.h"
#include "control/profile_curve.h"
#include "config.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern QueueHandle_t g_cmd_queue;

static profile_t s_buf;          /* in-progress edits */

typedef enum {
    F_RATE1, F_SOAK_T, F_SOAK_TIME, F_RATE2, F_REFLOW_T, F_REFLOW_TIME, F_COOL_RATE,
    F_COUNT_
} field_id_t;

static lv_obj_t *s_val_labels[F_COUNT_];
static lv_obj_t *s_btn_save;
static lv_obj_t *s_saved_label;
static uint16_t  s_last_seen_profile_seq = 0;

static void clear_saved(lv_timer_t *t) {
    lv_obj_add_flag(s_saved_label, LV_OBJ_FLAG_HIDDEN);
    lv_timer_delete(t);
}

static void show_saved(void) {
    lv_obj_remove_flag(s_saved_label, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(clear_saved, 1500, NULL);
}

static void refresh_label(field_id_t id) {
    char buf[16];
    switch (id) {
        case F_RATE1:       snprintf(buf, sizeof(buf), "%.1f", s_buf.first_heatup_rate);  break;
        case F_SOAK_T:      snprintf(buf, sizeof(buf), "%u",  s_buf.soak_temp);           break;
        case F_SOAK_TIME:   snprintf(buf, sizeof(buf), "%u",  s_buf.soak_time);           break;
        case F_RATE2:       snprintf(buf, sizeof(buf), "%.1f", s_buf.second_heatup_rate); break;
        case F_REFLOW_T:    snprintf(buf, sizeof(buf), "%u",  s_buf.reflow_temp);         break;
        case F_REFLOW_TIME: snprintf(buf, sizeof(buf), "%u",  s_buf.reflow_time);         break;
        case F_COOL_RATE:   snprintf(buf, sizeof(buf), "%.1f", s_buf.cool_down_rate);     break;
        default: return;
    }
    lv_label_set_text(s_val_labels[id], buf);
}

static void keypad_done(float value, void *user_data) {
    field_id_t id = (field_id_t)(intptr_t)user_data;
    switch (id) {
        case F_RATE1:       s_buf.first_heatup_rate  = value;           break;
        case F_SOAK_T:      s_buf.soak_temp          = (uint16_t)value; break;
        case F_SOAK_TIME:   s_buf.soak_time          = (uint16_t)value; break;
        case F_RATE2:       s_buf.second_heatup_rate = value;           break;
        case F_REFLOW_T:    s_buf.reflow_temp        = (uint16_t)value; break;
        case F_REFLOW_TIME: s_buf.reflow_time        = (uint16_t)value; break;
        case F_COOL_RATE:   s_buf.cool_down_rate     = value;           break;
        default: return;
    }
    refresh_label(id);
}

static void field_clicked_cb(lv_event_t *e) {
    field_id_t id = (field_id_t)(intptr_t)lv_event_get_user_data(e);
    keypad_config_t cfg;
    switch (id) {
    case F_RATE1:       cfg = (keypad_config_t){"Heat-up 1 rate",     "°C/s", HEATUP_RATE_MIN,     HEATUP_RATE_MAX,     s_buf.first_heatup_rate,  true,  keypad_done, (void*)(intptr_t)id}; break;
    case F_SOAK_T:      cfg = (keypad_config_t){"Soak Temperature",   "°C",   SOAK_TEMP_MIN,       SOAK_TEMP_MAX,       s_buf.soak_temp,          false, keypad_done, (void*)(intptr_t)id}; break;
    case F_SOAK_TIME:   cfg = (keypad_config_t){"Soak Time",          "s",    SOAK_TIME_MIN,       SOAK_TIME_MAX,       s_buf.soak_time,          false, keypad_done, (void*)(intptr_t)id}; break;
    case F_RATE2:       cfg = (keypad_config_t){"Heat-up 2 rate",     "°C/s", HEATUP_RATE_MIN,     HEATUP_RATE_MAX,     s_buf.second_heatup_rate, true,  keypad_done, (void*)(intptr_t)id}; break;
    case F_REFLOW_T:    cfg = (keypad_config_t){"Reflow Temperature", "°C",   REFLOW_TEMP_MIN,     REFLOW_TEMP_MAX,     s_buf.reflow_temp,        false, keypad_done, (void*)(intptr_t)id}; break;
    case F_REFLOW_TIME: cfg = (keypad_config_t){"Reflow Time",        "s",    REFLOW_TIME_MIN,     REFLOW_TIME_MAX,     s_buf.reflow_time,        false, keypad_done, (void*)(intptr_t)id}; break;
    case F_COOL_RATE:   cfg = (keypad_config_t){"Cool-down rate",     "°C/s", COOL_DOWN_RATE_MIN,  COOL_DOWN_RATE_MAX,  s_buf.cool_down_rate,     true,  keypad_done, (void*)(intptr_t)id}; break;
    default: return;
    }
    screen_keypad_open(&cfg);
}

static void reset_clicked_cb(lv_event_t *e) {
    (void)e;
    profile_defaults(&s_buf);
    for (int i = 0; i < F_COUNT_; i++) refresh_label((field_id_t)i);
}

static void save_clicked_cb(lv_event_t *e) {
    (void)e;
    if (!profile_validate(&s_buf)) return;
    cmd_msg_t msg = {.type = CMD_APPLY_PROFILE, .payload.profile = s_buf};
    xQueueSend(g_cmd_queue, &msg, 0);
}

/* === Sub-row layout (all cards use the same row template) =================
 *
 * Card width 296. Inside the card we lay each row out as:
 *   [12 left-pad] [label, ~110 px] [value chip, 76 px] [8 gap] [unit, ~36 px]
 * The value chip is the focus point: PANEL_BG-on-darker fill, faint border,
 * Montserrat 20 in THEME_ACCENT. The whole row is tap-clickable.
 *
 * ROW_H = 36 px (room for 20-pt value chip with vertical padding).
 */
#define ROW_H              30   /* 28-px value chip + 1 px clearance top/bottom; trims the card stack so the last card clears the action row (was 34 → cards overlapped RESET/SAVE). */
#define ROW_LABEL_X        12
#define ROW_VALUE_X       146
#define ROW_VALUE_W        76
#define ROW_VALUE_H        28
#define ROW_UNIT_X        232
#define CARD_W            296
#define CARD_TITLE_H       22

static void make_sub_row(lv_obj_t *card, int y,
                         const char *label, const char *unit,
                         field_id_t id) {
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_size(row, CARD_W, ROW_H);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, field_clicked_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)id);

    /* Label (left-aligned). */
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, THEME_FG_DIM, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, ROW_LABEL_X, 0);

    /* Value chip: small panel with a brighter border and bigger font so
     * the editable number is the visual focus of the row. */
    lv_obj_t *vchip = lv_obj_create(row);
    lv_obj_set_size(vchip, ROW_VALUE_W, ROW_VALUE_H);
    lv_obj_set_pos(vchip, ROW_VALUE_X, (ROW_H - ROW_VALUE_H) / 2);
    lv_obj_set_style_bg_color(vchip, THEME_BG, 0);
    lv_obj_set_style_bg_opa(vchip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(vchip, THEME_FG_FAINT, 0);
    lv_obj_set_style_border_width(vchip, 1, 0);
    lv_obj_set_style_pad_all(vchip, 0, 0);
    lv_obj_clear_flag(vchip, LV_OBJ_FLAG_SCROLLABLE);
    /* Forward clicks on the chip to the row's handler (parent), since
     * the chip sits on top of the row and would otherwise swallow them. */
    lv_obj_add_event_cb(vchip, field_clicked_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)id);

    lv_obj_t *v = lv_label_create(vchip);
    lv_obj_set_style_text_color(v, THEME_ACCENT, 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_20, 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_TRANSP, 0);
    lv_obj_center(v);
    s_val_labels[id] = v;

    /* Unit (faint, right of value chip, on the SAME row). */
    lv_obj_t *u = lv_label_create(row);
    lv_label_set_text(u, unit);
    lv_obj_set_style_text_color(u, THEME_FG_FAINT, 0);
    lv_obj_set_style_bg_opa(u, LV_OPA_TRANSP, 0);
    lv_obj_align(u, LV_ALIGN_LEFT_MID, ROW_UNIT_X, 0);
}

/* Build a stage card with a title bar and `n_rows` sub-rows. Returns
 * the total card height it consumed (so the caller can compute the
 * Y position of the next card without hard-coding it). */
static int make_stage_card(lv_obj_t *parent, int y, const char *title,
                           int n_rows,
                           const char *labels[], const char *units[],
                           const field_id_t ids[]) {
    int card_h = CARD_TITLE_H + n_rows * ROW_H + 2;
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, CARD_W, card_h);
    lv_obj_set_pos(card, 0, y);
    lv_obj_set_style_bg_color(card, THEME_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, THEME_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Title bar */
    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, THEME_FG, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(t, 10, 4);

    /* Sub-rows */
    for (int i = 0; i < n_rows; i++) {
        make_sub_row(card, CARD_TITLE_H + i * ROW_H,
                     labels[i], units[i], ids[i]);
    }
    return card_h;
}

void screen_profile_create(lv_obj_t *parent) {
    lv_obj_set_style_bg_color(parent, THEME_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(parent, 8, 0);
    /* Card stack + action row are sized to fit the 420 px tab page — no scroll. */
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* Five stage cards (one per stage in the planned cycle).
     * Single-value cards: Heat-up 1 / Heat-up 2 / Cool-down.
     * Dual-value cards:   Soak (temp + time) / Reflow (temp + time).
     * Vertical layout is computed bottom-up from card heights so future
     * tweaks (new field, taller chip) don't need every Y to be edited. */
    const int GAP = 4;
    int y = 0;

    {   const char *L[] = {"Rate"};
        const char *U[] = {"°C/s"};
        const field_id_t I[] = {F_RATE1};
        y += make_stage_card(parent, y, "Heat-up 1", 1, L, U, I) + GAP;
    }
    {   const char *L[] = {"Temperature", "Time"};
        const char *U[] = {"°C",          "s"};
        const field_id_t I[] = {F_SOAK_T,  F_SOAK_TIME};
        y += make_stage_card(parent, y, "Soak",     2, L, U, I) + GAP;
    }
    {   const char *L[] = {"Rate"};
        const char *U[] = {"°C/s"};
        const field_id_t I[] = {F_RATE2};
        y += make_stage_card(parent, y, "Heat-up 2", 1, L, U, I) + GAP;
    }
    {   const char *L[] = {"Temperature", "Time"};
        const char *U[] = {"°C",          "s"};
        const field_id_t I[] = {F_REFLOW_T, F_REFLOW_TIME};
        y += make_stage_card(parent, y, "Reflow",   2, L, U, I) + GAP;
    }
    {   const char *L[] = {"Rate"};
        const char *U[] = {"°C/s"};
        const field_id_t I[] = {F_COOL_RATE};
        y += make_stage_card(parent, y, "Cool-down", 1, L, U, I) + GAP;
    }

    /* Action row: RESET (left) + SAVE (right), each ~half the card
     * width. Anchored to the bottom so cards above can grow without
     * shifting the buttons. */
    const int ACTION_H = 40;
    /* Usable content height = 420 (tab page) - 16 (pad_all 8). Leave an 8 px
     * bottom margin so the row never nudges a scrollbar into existence. */
    const int ACTION_Y = 420 - 16 - 8 - ACTION_H;
    const int ACTION_W = 146;                  /* (296 - 4 gap) / 2 */

    lv_obj_t *reset = lv_button_create(parent);
    lv_obj_set_size(reset, ACTION_W, ACTION_H); lv_obj_set_pos(reset, 0, ACTION_Y);
    lv_obj_set_style_bg_color(reset, THEME_BTN_NEUTRAL, 0);
    lv_obj_add_event_cb(reset, reset_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(reset); lv_label_set_text(rl, "RESET"); lv_obj_center(rl);

    s_btn_save = lv_button_create(parent);
    lv_obj_set_size(s_btn_save, ACTION_W, ACTION_H); lv_obj_set_pos(s_btn_save, ACTION_W + 4, ACTION_Y);
    lv_obj_set_style_bg_color(s_btn_save, THEME_INFO, 0);
    lv_obj_add_event_cb(s_btn_save, save_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(s_btn_save); lv_label_set_text(sl, "SAVE"); lv_obj_center(sl);

    /* "Saved" toast — hidden by default. */
    s_saved_label = lv_label_create(parent);
    lv_label_set_text(s_saved_label, LV_SYMBOL_OK " saved");
    lv_obj_set_style_text_color(s_saved_label, THEME_OK, 0);
    lv_obj_set_style_bg_opa(s_saved_label, LV_OPA_TRANSP, 0);
    lv_obj_align(s_saved_label, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_add_flag(s_saved_label, LV_OBJ_FLAG_HIDDEN);
}

void screen_profile_set_initial(const profile_t *p) {
    s_buf = *p;
    for (int i = 0; i < F_COUNT_; i++) refresh_label((field_id_t)i);
}

void screen_profile_update(const oven_state_t *snap) {
    /* §7.6: SAVE disabled while mode != MODE_IDLE. */
    if (snap->mode == MODE_IDLE) {
        lv_obj_remove_state(s_btn_save, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_btn_save, LV_STATE_DISABLED);
    }

    /* §7.6: toast on controller ACK (seq advance). */
    if (snap->profile_applied_seq != s_last_seen_profile_seq) {
        s_last_seen_profile_seq = snap->profile_applied_seq;
        show_saved();
    }
}

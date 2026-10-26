#include "ui/screen_dry.h"
#include "ui/screen_keypad.h"
#include "ui/theme.h"
#include "storage/nvs_schema.h"
#include "storage/nvs_store.h"
#include "config.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <stdint.h>

extern QueueHandle_t g_cmd_queue;

/* Row layout: <name>          <temp>          <hold>
 * name left, temperature centred, hold time right. The two value zones are
 * independently tappable so either field can be edited on its own — the
 * previous design reached the hold time only by confirming through the
 * temperature keypad, so editing one field forced a decision about the other
 * and an ESC out of the temperature entry skipped the hold time entirely. */
#define DRY_VAL_W        76      /* width of each tappable value zone, px    */
#define DRY_VAL_EXT_HIT   8      /* grows the hit box to the full row height */

/* Bottom offset of the START hint. START is 40 px tall and sits 4 px off the
 * bottom, so its top edge is 44 px up; 2 px more keeps the hint clear of it. */
#define DRY_HINT_BOTTOM_OFS  46

/* Hold-time keypad bounds, in the same whole minutes hold_min is stored in.
 * These must equal dry_presets_validate's hold_min_min / hold_max_min: the
 * keypad is the primary gate and persist()'s validate is only a backstop, and
 * a preset the validator rejects reverts ALL SIX to defaults. The low bound
 * mirrors nvs_schema.c's ceiling rather than a floor for exactly the reason
 * given there — at a DRY_HOLD_MIN_MS of 90000 a floor would admit 1 min, which
 * the validator then rejects. Both are 1 today; this keeps them equal if the
 * constant ever stops dividing evenly. */
#define DRY_HOLD_MIN_MINUTES  ((DRY_HOLD_MIN_MS + 59999u) / 60000u)
#define DRY_HOLD_MAX_MINUTES  (DRY_DURATION_MAX_MS / 60000u)

static dry_presets_t s_presets;
static unsigned      s_selected = 0;      /* index 0 (PLA) on every boot */
static lv_obj_t     *s_rows[DRY_PRESET_COUNT];
static lv_obj_t     *s_temps[DRY_PRESET_COUNT];
static lv_obj_t     *s_holds[DRY_PRESET_COUNT];
static lv_obj_t     *s_start_btn;
static lv_obj_t     *s_hint;

static void temp_text(unsigned i, char *buf, size_t n) {
    snprintf(buf, n, "%u °C", (unsigned)s_presets.e[i].temp_c);
}

static void hold_text(unsigned i, char *buf, size_t n) {
    unsigned h = s_presets.e[i].hold_min / 60u;
    unsigned m = s_presets.e[i].hold_min % 60u;
    if (m) snprintf(buf, n, "%uh%02u", h, m);
    else   snprintf(buf, n, "%u h",   h);
}

static void refresh_rows(void) {
    char buf[16];
    for (unsigned i = 0; i < DRY_PRESET_COUNT; i++) {
        temp_text(i, buf, sizeof buf);
        lv_label_set_text(s_temps[i], buf);
        hold_text(i, buf, sizeof buf);
        lv_label_set_text(s_holds[i], buf);
        lv_obj_set_style_border_color(s_rows[i],
            (i == s_selected) ? THEME_ACCENT : THEME_BORDER, 0);
        lv_obj_set_style_border_width(s_rows[i], (i == s_selected) ? 2 : 1, 0);
        lv_obj_set_style_border_opa(s_rows[i], LV_OPA_COVER, 0);
    }
}

static void persist(void) {
    /* Never write a set the loader would then reject: a rejected record reverts
     * EVERY preset to defaults on the next boot, so one bad edit would silently
     * discard the other five. The keypad clamps to the same bounds, so this is
     * a backstop, not the primary gate. */
    if (!dry_presets_validate(&s_presets)) {
        dry_presets_defaults(&s_presets);
    }
    nvs_store_save_dry_presets(&s_presets);
    refresh_rows();
}

static void hold_done_cb(float value, void *ud) {
    unsigned i = (unsigned)(intptr_t)ud;
    /* Entered in whole minutes, stored in whole minutes — no unit conversion,
     * so the value round-trips through the keypad unchanged. */
    s_presets.e[i].hold_min = (uint16_t)(value + 0.5f);
    persist();
}

static void temp_done_cb(float value, void *ud) {
    unsigned i = (unsigned)(intptr_t)ud;
    s_presets.e[i].temp_c = (uint16_t)(value + 0.5f);
    persist();
}

static void select_row(unsigned i) {
    if (i == s_selected) return;
    s_selected = i;
    refresh_rows();
}

static void open_temp_keypad(unsigned i) {
    keypad_config_t k = {
        .field_label = "Drying temperature", .unit = "°C",
        .min = DRY_TEMP_MIN_C, .max = DRY_TEMP_MAX_C,
        .initial = (float)s_presets.e[i].temp_c,
        .allow_decimal = false,
        .on_ok = temp_done_cb, .user_data = (void *)(intptr_t)i,
    };
    screen_keypad_open(&k);
}

/* Entry is in whole minutes, not decimal hours: a 10-minute hold typed as
 * "0.16" is both awkward and lossy. The row still DISPLAYS hours ("6 h",
 * "1h30") because that reads better at a glance; the keypad header spells the
 * entry unit out as "Drying time - min - 1 to 960". Matches how
 * screen_pid_tuning enters its step-test duration. */
static void open_hold_keypad(unsigned i) {
    keypad_config_t k = {
        .field_label = "Drying time", .unit = "min",
        .min = (float)DRY_HOLD_MIN_MINUTES,
        .max = (float)DRY_HOLD_MAX_MINUTES,
        .initial = (float)s_presets.e[i].hold_min,
        .allow_decimal = false,
        .on_ok = hold_done_cb, .user_data = (void *)(intptr_t)i,
    };
    screen_keypad_open(&k);
}

/* LVGL hands a click to the innermost clickable object only: lv_indev_search_obj
 * returns the first child that hits and LV_OBJ_FLAG_EVENT_BUBBLE is off by
 * default, so the row's own handler never runs for a tap on a value zone. Each
 * value handler therefore selects its row itself — the row you touched must be
 * the row START DRY goes on to use. */
static void temp_clicked_cb(lv_event_t *e) {
    unsigned i = (unsigned)(intptr_t)lv_event_get_user_data(e);
    select_row(i);
    open_temp_keypad(i);
}

static void hold_clicked_cb(lv_event_t *e) {
    unsigned i = (unsigned)(intptr_t)lv_event_get_user_data(e);
    select_row(i);
    open_hold_keypad(i);
}

/* Name area and the gaps between value zones: select only. */
static void row_clicked_cb(lv_event_t *e) {
    select_row((unsigned)(intptr_t)lv_event_get_user_data(e));
}

static void start_clicked_cb(lv_event_t *e) {
    (void)e;
    cmd_msg_t m = { .type = CMD_START_DRY };
    m.payload.dry.target_c = (float)s_presets.e[s_selected].temp_c;
    m.payload.dry.hold_ms  = (uint32_t)s_presets.e[s_selected].hold_min * 60000u;
    xQueueSend(g_cmd_queue, &m, 0);
}

/* A tappable value zone. Deliberately a label rather than a button: the LVGL
 * pool is 64 KB and six more buttons would each add their own background,
 * border and child label. The label keeps its natural height so the text stays
 * vertically centred in the row, and ext_click_area grows only the hit box to
 * the full 34 px — that costs nothing, because add_event_cb already allocates
 * the spec_attr the pad lives in. */
static lv_obj_t *make_value_zone(lv_obj_t *row, unsigned i, lv_align_t align,
                                 lv_text_align_t text_align, lv_event_cb_t cb) {
    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_width(lbl, DRY_VAL_W);
    lv_obj_set_style_text_align(lbl, text_align, 0);
    lv_obj_set_style_text_color(lbl, THEME_FG_FAINT, 0);
    lv_obj_align(lbl, align, 0, 0);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(lbl, DRY_VAL_EXT_HIT);
    lv_obj_add_event_cb(lbl, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    return lbl;
}

void screen_dry_create(lv_obj_t *parent) {
    lv_obj_set_style_bg_color(parent, THEME_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);

    nvs_store_load_dry_presets(&s_presets);

    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(list, THEME_BG, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    for (unsigned i = 0; i < DRY_PRESET_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(list);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 34);
        lv_obj_set_style_bg_color(btn, THEME_PANEL_BG, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, THEME_BORDER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_left(btn, 10, 0);
        lv_obj_set_style_pad_right(btn, 10, 0);
        lv_obj_add_event_cb(btn, row_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        s_rows[i] = btn;

        lv_obj_t *name = lv_label_create(btn);
        lv_label_set_text(name, dry_preset_name(i));
        lv_obj_set_style_text_color(name, THEME_FG, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        s_temps[i] = make_value_zone(btn, i, LV_ALIGN_CENTER,
                                     LV_TEXT_ALIGN_CENTER, temp_clicked_cb);
        s_holds[i] = make_value_zone(btn, i, LV_ALIGN_RIGHT_MID,
                                     LV_TEXT_ALIGN_RIGHT, hold_clicked_cb);
    }

    s_start_btn = lv_button_create(parent);
    lv_obj_set_size(s_start_btn, LV_HOR_RES - 24, 40);
    lv_obj_align(s_start_btn, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(s_start_btn, THEME_GO, 0);
    lv_obj_set_style_opa(s_start_btn, THEME_OPA_DISABLED, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_start_btn, start_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(s_start_btn);
    lv_label_set_text(lbl, "START DRY");
    lv_obj_set_style_text_color(lbl, THEME_FG, 0);
    lv_obj_center(lbl);

    /* Why START is unavailable. A disabled button with no explanation is the
     * same dead end the HOME "too hot" line exists to avoid.
     *
     * Sized and anchored so the text cannot clip. lv_obj_align_to() computes a
     * ONE-SHOT position from the label's size at call time and then pins it
     * with lv_obj_set_pos (it even resets the style align to TOP_LEFT), so a
     * label created empty was centred at zero width and the real text grew
     * rightward off the 320 px panel. lv_obj_align() instead stores a
     * PERSISTENT style align that the layout re-evaluates on every resize.
     * Anchoring the BOTTOM edge means a wrapped second line grows upward into
     * the empty space above, never down into START — so screen_dry_update can
     * set any string without re-aligning. */
    s_hint = lv_label_create(parent);
    lv_obj_set_width(s_hint, LV_HOR_RES - 24);      /* same margin as START */
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_hint, THEME_FG_FAINT, 0);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -DRY_HINT_BOTTOM_OFS);
    lv_label_set_text(s_hint, "");

    refresh_rows();
}

void screen_dry_update(const oven_state_t *snap) {
    /* Mirror sm_start_dry's own gates so the button reflects the decision the
     * control task would actually make; it stays the authority (defence in
     * depth, same split as HOME's START). */
    const float target_c = (float)s_presets.e[s_selected].temp_c;
    const bool  idle     = (snap->mode == MODE_IDLE);
    const bool  too_hot  = snap->temp_c > target_c + DRY_START_MARGIN_C;
    const bool  can      = idle && !too_hot;

    if (can) lv_obj_remove_state(s_start_btn, LV_STATE_DISABLED);
    else     lv_obj_add_state(s_start_btn, LV_STATE_DISABLED);

    if (can) {
        lv_label_set_text(s_hint, "");
    } else if (!idle) {
        lv_label_set_text(s_hint, "Oven busy - stop the current run first");
    } else {
        lv_label_set_text_fmt(s_hint, "Too hot (%d °C) - cool below %d °C",
                              (int)snap->temp_c, (int)(target_c + DRY_START_MARGIN_C));
    }
}

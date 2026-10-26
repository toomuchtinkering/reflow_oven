#include "ui/screen_pid_tuning.h"
#include "ui/screen_settings.h"
#include "ui/screen_keypad.h"
#include "ui/theme.h"
#include "oven_state.h"
#include "config.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <stdint.h>

extern QueueHandle_t g_cmd_queue;

/* Editable step-test parameters (touchscreen-entered). */
static uint8_t  s_duty     = STEP_TEST_DUTY_DEFAULT;
static float    s_ceiling  = STEP_TEST_CEILING_DEFAULT_C;
static uint32_t s_max_ms   = STEP_TEST_MAX_DURATION_MS;

static lv_obj_t *s_duty_lbl, *s_ceiling_lbl, *s_maxmin_lbl;
static lv_obj_t *s_start_btn, *s_stop_btn, *s_live_lbl;
static bool      s_created;

/* Browser-suggested gain confirm prompt. s_confirm_open mirrors "is our modal
 * currently up"; s_confirm_gains is the UI's own copy carried through OK. */
static bool        s_confirm_open;
static pid_gains_t s_confirm_gains;

static void apply_confirmed(void) {
    cmd_msg_t m = { .type = CMD_APPLY_PID, .payload.pid = s_confirm_gains };
    xQueueSend(g_cmd_queue, &m, 0);
    s_confirm_open = false;
}
static void suggestion_dismissed(void) {
    cmd_msg_t m = { .type = CMD_DISMISS_PID };
    xQueueSend(g_cmd_queue, &m, 0);
    s_confirm_open = false;
}

static void refresh_param_labels(void) {
    char b[24];
    snprintf(b, sizeof b, "%u %%", (unsigned)s_duty);
    lv_label_set_text(s_duty_lbl, b);
    snprintf(b, sizeof b, "%.0f \xC2\xB0""C", s_ceiling);
    lv_label_set_text(s_ceiling_lbl, b);
    snprintf(b, sizeof b, "%lu min", (unsigned long)(s_max_ms / 60000u));
    lv_label_set_text(s_maxmin_lbl, b);
}

static void duty_done(float v, void *u) { (void)u; s_duty = (uint8_t)v; refresh_param_labels(); }
static void ceiling_done(float v, void *u) { (void)u; s_ceiling = v; refresh_param_labels(); }
static void maxmin_done(float v, void *u) { (void)u; s_max_ms = (uint32_t)(v * 60000.0f); refresh_param_labels(); }

static void duty_clicked(lv_event_t *e) {
    (void)e;
    keypad_config_t c = { .field_label="Step duty", .unit="%", .min=STEP_TEST_DUTY_MIN,
        .max=STEP_TEST_DUTY_MAX, .initial=s_duty, .allow_decimal=false, .on_ok=duty_done };
    screen_keypad_open(&c);
}
static void ceiling_clicked(lv_event_t *e) {
    (void)e;
    keypad_config_t c = { .field_label="Ceiling", .unit="\xC2\xB0""C", .min=STEP_TEST_CEILING_MIN_C,
        .max=STEP_TEST_CEILING_MAX_C, .initial=s_ceiling, .allow_decimal=false, .on_ok=ceiling_done };
    screen_keypad_open(&c);
}
static void maxmin_clicked(lv_event_t *e) {
    (void)e;
    /* Entered in minutes (1..15); STEP_TEST_MAX_DURATION_MS is the default 10 min. */
    keypad_config_t c = { .field_label="Max duration", .unit="min", .min=1,
        .max=15, .initial=(float)(s_max_ms / 60000u), .allow_decimal=false, .on_ok=maxmin_done };
    screen_keypad_open(&c);
}

static void start_clicked(lv_event_t *e) {
    (void)e;
    cmd_msg_t m = { .type = CMD_START_STEP_TEST };
    m.payload.steptest.duty = s_duty;
    m.payload.steptest.ceiling_c = s_ceiling;
    m.payload.steptest.max_ms = s_max_ms;
    xQueueSend(g_cmd_queue, &m, 0);
}
static void stop_clicked(lv_event_t *e) {
    (void)e;
    cmd_msg_t m = { .type = CMD_STOP };
    xQueueSend(g_cmd_queue, &m, 0);
}

/* A tappable param row: <label> [value]. */
static lv_obj_t *make_param_row(lv_obj_t *parent, int y, const char *label,
                                lv_event_cb_t cb) {
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, LV_HOR_RES - 16, 40);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_bg_color(row, THEME_PANEL_BG, 0);
    lv_obj_set_style_border_color(row, THEME_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, THEME_FG_DIM, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *v = lv_label_create(row);
    lv_obj_set_style_text_color(v, THEME_ACCENT, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -4, 0);
    return v;
}

lv_obj_t *screen_pid_tuning_create(void) {
    lv_obj_t *sub = settings_make_subscreen("PID Tuning");
    int y = 40;
    s_duty_lbl    = make_param_row(sub, y, "Step duty",    duty_clicked);    y += 48;
    s_ceiling_lbl = make_param_row(sub, y, "Ceiling",      ceiling_clicked); y += 48;
    s_maxmin_lbl  = make_param_row(sub, y, "Max duration", maxmin_clicked);  y += 56;

    s_start_btn = lv_button_create(sub);
    lv_obj_set_size(s_start_btn, LV_HOR_RES - 16, 48);
    lv_obj_set_pos(s_start_btn, 0, y);
    lv_obj_set_style_bg_color(s_start_btn, THEME_ACCENT, 0);
    lv_obj_add_event_cb(s_start_btn, start_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(s_start_btn);
    lv_label_set_text(sl, "START step test");
    lv_obj_center(sl);

    s_stop_btn = lv_button_create(sub);
    lv_obj_set_size(s_stop_btn, LV_HOR_RES - 16, 48);
    lv_obj_set_pos(s_stop_btn, 0, y);
    lv_obj_set_style_bg_color(s_stop_btn, THEME_STOP, 0);
    lv_obj_add_event_cb(s_stop_btn, stop_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *stl = lv_label_create(s_stop_btn);
    lv_label_set_text(stl, "STOP");
    lv_obj_center(stl);
    lv_obj_add_flag(s_stop_btn, LV_OBJ_FLAG_HIDDEN);

    s_live_lbl = lv_label_create(sub);
    lv_label_set_text(s_live_lbl, "");
    lv_obj_set_style_text_color(s_live_lbl, THEME_FG, 0);
    lv_obj_set_pos(s_live_lbl, 4, y + 56);

    refresh_param_labels();
    s_created = true;
    return sub;
}

void screen_pid_tuning_update(const oven_state_t *snap) {
    if (!s_created) return;
    bool running = (snap->mode == MODE_STEP_TEST);
    if (running) {
        lv_obj_add_flag(s_start_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_stop_btn, LV_OBJ_FLAG_HIDDEN);
        char b[48];
        snprintf(b, sizeof b, "%.1f \xC2\xB0""C   duty %u%%   %lus",
                 snap->temp_c, (unsigned)snap->step_duty,
                 (unsigned long)(snap->elapsed_ms / 1000));
        lv_label_set_text(s_live_lbl, b);
    } else {
        lv_obj_clear_flag(s_start_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_stop_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_live_lbl, "");
    }

    /* Surface a browser-suggested gain set for confirmation — idle only.
     * OK applies (CMD_APPLY_PID); Cancel OR Back-nav dismisses (CMD_DISMISS_PID,
     * via the settings_confirm on_cancel wired into confirm_close, finding F1)
     * so the controller's pending flag clears and the prompt does not re-pop.
     *
     * Reconciliation: s_confirm_open mirrors "is our modal currently up". Both
     * outcomes (apply_confirmed / suggestion_dismissed) set it false AND cause
     * the controller to clear pending_pid_valid within a tick or two; the line
     * below converges s_confirm_open to false once that clear is observed, so a
     * Back-nav that fired the dismiss can't leave us wedged. */
    if (snap->pending_pid_valid && !s_confirm_open && snap->mode == MODE_IDLE) {
        s_confirm_open = true;
        s_confirm_gains = snap->pending_pid;
        char msg[128];
        snprintf(msg, sizeof msg,
                 "Apply Kp %.2f  Ki %.4f  Kd %.1f\nK.hold %.2f  K.rate %.0f ?",
                 s_confirm_gains.kp, s_confirm_gains.ki, s_confirm_gains.kd,
                 s_confirm_gains.k_hold, s_confirm_gains.k_rate);
        settings_confirm("Suggested gains", msg, apply_confirmed, suggestion_dismissed);
    }
    if (!snap->pending_pid_valid) s_confirm_open = false;   /* controller cleared it */
}

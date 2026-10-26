#include "ui/screen_settings.h"
#include "ui/theme.h"
#include <stddef.h>

/* Sub-screen creators (resolved in later tasks). */
extern lv_obj_t *screen_network_create(void);          /* Task 18 */
extern lv_obj_t *screen_audio_create(void);            /* Task 15 */
extern lv_obj_t *screen_pid_subscreen_create(void);    /* Task 14 */
extern lv_obj_t *screen_pid_tuning_create(void);       /* PID Tuning step-test */
extern lv_obj_t *screen_firmware_update_create(void);  /* Task 28 */
extern lv_obj_t *screen_about_create(void);            /* Task 27 */

static lv_obj_t *s_parent = NULL;
static lv_obj_t *s_list   = NULL;

/* Navigation stack. The WiFi flow nests (Network → Choose network → password),
 * so a single-level "current sub" sent "back" to the settings root instead of
 * the previous screen (#29). Each entry carries an optional on_pop hook so a
 * screen can clean up (e.g. the password screen's WiFi unsubscribe) whether it
 * leaves via Back or programmatically (#30). */
#define SUB_STACK_MAX 4
typedef struct { lv_obj_t *obj; void (*on_pop)(void); } sub_entry_t;
static sub_entry_t s_stack[SUB_STACK_MAX];
static int         s_depth = 0;
static void      (*s_next_pop_hook)(void) = NULL;

static void confirm_close(void);   /* defined with the confirm-dialog helpers below */

typedef struct { const char *icon; const char *title; lv_obj_t *(*creator)(void); } row_t;
static const row_t s_rows[] = {
    { LV_SYMBOL_WIFI,     "Network",         &screen_network_create         },
    { LV_SYMBOL_AUDIO,    "Audio",           &screen_audio_create           },
    { LV_SYMBOL_SETTINGS, "PID",             &screen_pid_subscreen_create   },
    { LV_SYMBOL_CHARGE,   "PID Tuning",      &screen_pid_tuning_create      },
    { LV_SYMBOL_UPLOAD,   "Firmware update", &screen_firmware_update_create },
    { LV_SYMBOL_LIST,     "About",           &screen_about_create           },
};

static lv_obj_t *s_subscreens[sizeof s_rows / sizeof s_rows[0]] = {0};

/* Proper lv_event_cb_t signature wrapper (avoids UB cast). */
static void back_clicked_cb(lv_event_t *e) { (void)e; settings_pop_subscreen(); }

static void row_clicked_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_subscreens[idx]) s_subscreens[idx] = s_rows[idx].creator();
    settings_push_subscreen(s_subscreens[idx]);
}

void screen_settings_create(lv_obj_t *parent) {
    s_parent = parent;
    lv_obj_set_style_bg_color(parent, THEME_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);

    /* Card-row menu. Replaces the default lv_list (which shipped the light
     * LVGL theme look) with a black container of panel cards — same visual
     * language as the Profile / PID stage cards. */
    s_list = lv_obj_create(parent);
    lv_obj_set_size(s_list, LV_HOR_RES, LV_VER_RES - 24 - 36);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_list, THEME_BG, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_list, 8, 0);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);

    const size_t n = sizeof s_rows / sizeof s_rows[0];
    for (size_t i = 0; i < n; i++) {
        lv_obj_t *btn = lv_button_create(s_list);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 44);
        lv_obj_set_style_bg_color(btn, THEME_PANEL_BG, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, THEME_BORDER, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, THEME_BORDER, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_left(btn, 10, 0);
        lv_obj_set_style_pad_right(btn, 10, 0);
        lv_obj_add_event_cb(btn, row_clicked_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, s_rows[i].icon);
        lv_obj_set_style_text_color(icon, THEME_ACCENT, 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *title = lv_label_create(btn);
        lv_label_set_text(title, s_rows[i].title);
        lv_obj_set_style_text_color(title, THEME_FG, 0);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 28, 0);

        lv_obj_t *chev = lv_label_create(btn);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chev, THEME_FG_FAINT, 0);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

lv_obj_t *settings_make_subscreen(const char *title) {
    lv_obj_t *sub = lv_obj_create(s_parent);
    lv_obj_set_size(sub, LV_HOR_RES, LV_VER_RES - 24 - 36);
    lv_obj_set_pos(sub, 0, 0);
    lv_obj_set_style_bg_color(sub, THEME_BG, 0);
    lv_obj_set_style_bg_opa(sub, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(sub, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(sub, 4, 0);
    /* Sub-screens are fixed-layout; never let a few px of content overflow
     * raise scrollbars (the PID card column was tripping both axes). */
    lv_obj_clear_flag(sub, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(sub);
    lv_obj_set_size(back, 48, 28);
    lv_obj_set_pos(back, 0, 0);
    lv_obj_set_style_bg_color(back, THEME_PANEL_BG, 0);
    lv_obj_add_event_cb(back, back_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);

    lv_obj_t *t = lv_label_create(sub);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, THEME_FG, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 56, 4);

    lv_obj_add_flag(sub, LV_OBJ_FLAG_HIDDEN);
    return sub;
}

static void reveal_top(void) {
    lv_obj_t *top = s_depth ? s_stack[s_depth - 1].obj : s_list;
    lv_obj_clear_flag(top, LV_OBJ_FLAG_HIDDEN);
}

/* Hide + pop the top entry and run its on_pop hook. Does NOT reveal the new
 * top (callers do, so a multi-level pop doesn't flicker intermediates). */
static void pop_one(void) {
    if (!s_depth) return;
    sub_entry_t e = s_stack[--s_depth];
    lv_obj_add_flag(e.obj, LV_OBJ_FLAG_HIDDEN);
    if (e.on_pop) e.on_pop();
}

/* The next pushed sub-screen will run `hook` when it is later popped. */
void settings_set_next_pop_hook(void (*hook)(void)) { s_next_pop_hook = hook; }

void settings_push_subscreen(lv_obj_t *sub) {
    lv_obj_t *cur = s_depth ? s_stack[s_depth - 1].obj : s_list;
    lv_obj_add_flag(cur, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(sub, LV_OBJ_FLAG_HIDDEN);
    if (s_depth < SUB_STACK_MAX) s_stack[s_depth++] = (sub_entry_t){ sub, s_next_pop_hook };
    s_next_pop_hook = NULL;
}

void settings_pop_subscreen(void) {
    confirm_close();   /* a modal lives on the top layer; don't leave it floating after a pop */
    pop_one();
    reveal_top();
}

/* Pop everything except the first sub-screen (e.g. back to Network status after
 * a successful connect). */
void settings_pop_to_root_sub(void) {
    confirm_close();
    while (s_depth > 1) pop_one();
    reveal_top();
}

/* ── Modal confirm dialog (shared by Forget WiFi / Regenerate token / PID
 *    Tuning suggested gains) ─────────────────────────────────────────────── */
static void (*s_confirm_cb)(void) = NULL;
static void (*s_cancel_cb)(void)  = NULL;   /* runs when the modal closes without OK */
static lv_obj_t *s_confirm_box = NULL;

/* Closing the modal for ANY reason other than OK fires the cancel callback
 * exactly once. confirm_close() is also called by settings_pop_subscreen() and
 * settings_pop_to_root_sub(), so Back-navigating away from an open dialog still
 * runs on_cancel (so a pending PID suggestion can't get stuck — review F1). */
static void confirm_close(void) {
    void (*cancel)(void) = s_cancel_cb;   /* capture before clearing */
    s_confirm_cb = NULL;
    s_cancel_cb  = NULL;
    if (s_confirm_box) { lv_msgbox_close(s_confirm_box); s_confirm_box = NULL; }
    if (cancel) cancel();
}
static void confirm_ok_cb(lv_event_t *e) {
    (void)e;
    void (*cb)(void) = s_confirm_cb;   /* capture before confirm_close() clears it */
    s_cancel_cb = NULL;                /* OK path: suppress the cancel callback */
    confirm_close();
    if (cb) cb();
}
static void confirm_cancel_cb(lv_event_t *e) {
    (void)e;
    confirm_close();   /* fires s_cancel_cb internally */
}

void settings_info(const char *title, const char *msg) {
    confirm_close();                 /* never stack two dialogs (fires any prior cancel cb) */
    s_confirm_box = lv_msgbox_create(NULL);   /* NULL parent → modal */
    lv_msgbox_add_title(s_confirm_box, title);
    lv_msgbox_add_text(s_confirm_box, msg);
    lv_obj_t *ok = lv_msgbox_add_footer_button(s_confirm_box, "OK");
    lv_obj_add_event_cb(ok, confirm_cancel_cb, LV_EVENT_CLICKED, NULL);  /* just closes */
}

void settings_confirm(const char *title, const char *msg,
                      void (*on_confirm)(void), void (*on_cancel)(void)) {
    confirm_close();                 /* never stack two dialogs (fires any prior cancel cb) */
    s_confirm_cb = on_confirm;
    s_cancel_cb  = on_cancel;
    s_confirm_box = lv_msgbox_create(NULL);   /* NULL parent → modal */
    lv_msgbox_add_title(s_confirm_box, title);
    lv_msgbox_add_text(s_confirm_box, msg);
    lv_obj_t *cancel = lv_msgbox_add_footer_button(s_confirm_box, "Cancel");
    lv_obj_add_event_cb(cancel, confirm_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ok = lv_msgbox_add_footer_button(s_confirm_box, "OK");
    lv_obj_add_event_cb(ok, confirm_ok_cb, LV_EVENT_CLICKED, NULL);
}


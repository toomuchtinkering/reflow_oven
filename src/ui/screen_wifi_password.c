#include "ui/screen_wifi_password.h"
#include "ui/screen_settings.h"
#include "ui/theme.h"
#include "drivers/wifi.h"
#include "esp_lvgl_port.h"
#include <string.h>

static char s_ssid_buf[33] = {0};
static bool s_manual_mode = false;
static lv_obj_t *s_ssid_ta = NULL;
static lv_obj_t *s_pw_ta   = NULL;
static lv_obj_t *s_kbd     = NULL;
static lv_obj_t *s_status  = NULL;
static lv_obj_t *s_spinner = NULL;
static lv_obj_t *s_eye_btn = NULL;
static lv_obj_t *s_password_sub = NULL;  /* tracked so we can lv_obj_del on cleanup */

/* Session token: bumped every time the screen is torn down or recreated. A
 * wifi status event is marshalled to the LVGL task via lv_async_call, which
 * can still be in flight when a *newer* password screen has replaced this
 * one. The async carries the session it was scheduled under; if it no longer
 * matches, it is stale and must not touch (or delete) the new screen
 * instance (finding #28). */
/* volatile: written on the LVGL task (cleanup_screen), read on the WiFi event
 * task (on_wifi_status). Only the low 24 bits survive PW_PACK, which is far
 * more provisioning attempts than a session will ever see. */
static volatile uint32_t s_session = 0;

/* Forward declaration — definition below; referenced by cleanup_screen. */
static void on_wifi_status(wifi_cb_event_t ev);

static void cleanup_screen(void) {
    /* Tear down the screen tree, freeing all LVGL children (keyboard, labels,
     * spinner, etc.). Prevents a memory leak across repeated provisionings. */
    s_session++;   /* invalidate any in-flight async from the previous instance */
    wifi_unsubscribe_status(on_wifi_status);
    if (s_password_sub) { lv_obj_del(s_password_sub); s_password_sub = NULL; }
    s_status = s_spinner = s_pw_ta = s_kbd = s_eye_btn = s_ssid_ta = NULL;
}

/* Pack (session, event) into the single lv_async_call void* arg — session in
 * the high bits, the small event enum in the low byte — so no heap alloc is
 * needed for the marshal. */
#define PW_PACK(session, ev)   ((void *)(((uintptr_t)(session) << 8) | ((uintptr_t)(ev) & 0xFF)))
#define PW_SESSION(arg)        ((uint32_t)((uintptr_t)(arg) >> 8))
#define PW_EVENT(arg)          ((wifi_cb_event_t)((uintptr_t)(arg) & 0xFF))

/* Marshal wifi status events from event-loop task back to LVGL UI task. */
static void on_wifi_status_main(void *arg) {
    if (PW_SESSION(arg) != (s_session & 0xFFFFFFu)) return;  /* stale: a newer screen replaced this one (#28) */
    wifi_cb_event_t ev = PW_EVENT(arg);
    if (ev == WIFI_CB_CONNECTED) {
        /* Back to Network status (pop password + scan). The password's pop hook
         * runs cleanup_screen, so the screen tree is freed and the WiFi
         * subscription released. */
        settings_pop_to_root_sub();
    } else if (ev == WIFI_CB_CONNECT_FAILED) {
        if (s_status) lv_label_set_text(s_status, "Failed - check password.");
        if (s_spinner) lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}
static void on_wifi_status(wifi_cb_event_t ev) {
    /* Runs on the WiFi event task. lv_async_call touches the global LVGL timer
     * list, so it must hold the LVGL lock. publish() now invokes status cbs
     * WITHOUT the wifi lock held, so taking the LVGL lock here can't deadlock
     * against the UI tick's LVGL->wifi order. */
    lvgl_port_lock(0);
    lv_async_call(on_wifi_status_main, PW_PACK(s_session, ev));
    lvgl_port_unlock();
}

static void connect_cb(lv_event_t *e) {
    (void)e;
    if (s_manual_mode) {
        const char *ssid = lv_textarea_get_text(s_ssid_ta);
        strncpy(s_ssid_buf, ssid, 32); s_ssid_buf[32] = 0;
    }
    const char *pw = lv_textarea_get_text(s_pw_ta);
    lv_label_set_text(s_status, "Connecting...");
    lv_obj_clear_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    wifi_subscribe_status(on_wifi_status);  /* additive — does not displace OTA-server cb */
    wifi_connect_async(s_ssid_buf, pw);
}

static void eye_clicked(lv_event_t *e) {
    (void)e;
    bool hidden = lv_textarea_get_password_mode(s_pw_ta);
    lv_textarea_set_password_mode(s_pw_ta, !hidden);
    lv_obj_t *icon = lv_obj_get_child(s_eye_btn, 0);
    lv_label_set_text(icon, hidden ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

lv_obj_t *screen_wifi_password_create(const char *ssid) {
    /* If a previous password screen is still allocated, tear it down first. */
    cleanup_screen();
    s_manual_mode = (ssid[0] == 0);
    strncpy(s_ssid_buf, ssid, 32); s_ssid_buf[32] = 0;
    lv_obj_t *sub = settings_make_subscreen(s_manual_mode ? "Manual entry" : ssid);
    s_password_sub = sub;

    int y = 44;
    if (s_manual_mode) {
        s_ssid_ta = lv_textarea_create(sub);
        lv_obj_set_size(s_ssid_ta, LV_HOR_RES - 20, 32);
        lv_obj_set_pos(s_ssid_ta, 8, y);
        lv_textarea_set_placeholder_text(s_ssid_ta, "SSID");
        lv_textarea_set_one_line(s_ssid_ta, true);
        y += 40;
    }

    s_pw_ta = lv_textarea_create(sub);
    lv_obj_set_size(s_pw_ta, LV_HOR_RES - 60, 32);
    lv_obj_set_pos(s_pw_ta, 8, y);
    lv_textarea_set_password_mode(s_pw_ta, true);
    lv_textarea_set_placeholder_text(s_pw_ta, "password");
    lv_textarea_set_one_line(s_pw_ta, true);

    s_eye_btn = lv_button_create(sub);
    lv_obj_set_size(s_eye_btn, 40, 32);
    lv_obj_set_pos(s_eye_btn, LV_HOR_RES - 48, y);
    lv_obj_add_event_cb(s_eye_btn, eye_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *eye_icon = lv_label_create(s_eye_btn);
    lv_label_set_text(eye_icon, LV_SYMBOL_EYE_CLOSE);
    lv_obj_center(eye_icon);
    y += 40;

    s_status = lv_label_create(sub);
    lv_obj_set_pos(s_status, 8, y);
    lv_label_set_text(s_status, "");
    lv_obj_set_style_text_color(s_status, THEME_FG, 0);

    s_spinner = lv_spinner_create(sub);
    lv_obj_set_size(s_spinner, 24, 24);
    lv_obj_set_pos(s_spinner, LV_HOR_RES - 32, y);
    lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    y += 28;

    s_kbd = lv_keyboard_create(sub);
    /* Larger keys: there was ~60 px of dead space above the keyboard; grow it
     * to 220 px so the keys are easier to hit. -40 leaves room for the Connect
     * button below. */
    lv_obj_set_size(s_kbd, LV_HOR_RES, 220);
    lv_obj_align(s_kbd, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_keyboard_set_textarea(s_kbd, s_pw_ta);

    lv_obj_t *btn = lv_button_create(sub);
    lv_obj_set_size(btn, 96, 32);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_event_cb(btn, connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Connect");
    lv_obj_center(bl);

    /* Run cleanup_screen when this screen is popped (Back or success) so the
     * WiFi status subscription is always released, not just on connect/fail
     * (#30). The caller pushes `sub` right after this returns. */
    settings_set_next_pop_hook(cleanup_screen);
    return sub;
}

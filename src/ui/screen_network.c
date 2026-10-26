#include "ui/screen_network.h"
#include "ui/screen_settings.h"
#include "ui/screen_wifi_scan.h"
#include "ui/theme.h"
#include "drivers/wifi.h"
#include <stdio.h>
#include <limits.h>

static lv_obj_t *s_sub = NULL;
static lv_obj_t *s_status_l, *s_ssid_l, *s_ip_l, *s_host_l, *s_rssi_l;
static lv_obj_t *s_btn_change, *s_btn_forget, *s_btn_setup;

static void change_clicked(lv_event_t *e) { (void)e; settings_push_subscreen(screen_wifi_scan_create()); }

static void do_forget(void) { wifi_forget(); /* stays on this screen; refresh flips to Set up WiFi */ }
static void forget_clicked(lv_event_t *e) {
    (void)e;
    /* Spec §7.2 mandates a confirmation before erasing creds (#33). */
    settings_confirm("Forget WiFi?", "Disconnect and erase the saved network?", do_forget, NULL);
}

static void refresh(void) {
    wifi_state_t st = wifi_get_state();
    const char *sname = st == WIFI_STATE_CONNECTED ? "Connected" :
                        st == WIFI_STATE_CONNECTING ? "Connecting..." :
                        st == WIFI_STATE_SCANNING   ? "Scanning..." :
                                                       "Not configured";
    lv_label_set_text_fmt(s_status_l, "Status: %s", sname);
    char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    wifi_get_ssid(ssid);
    lv_label_set_text_fmt(s_ssid_l, "SSID:   %s", ssid[0] ? ssid : "(none)");
    char ip[16] = {0};
    wifi_get_ip_str(ip);
    lv_label_set_text_fmt(s_ip_l, "IP:     %s", ip[0] ? ip : "(none)");
    lv_label_set_text   (s_host_l, "Host:   reflow.local");
    /* Show signal only when connected and the reading is valid; INT_MIN means
     * "unavailable" and must not be printed as a number (#21). */
    int rssi = wifi_get_rssi();
    if (st == WIFI_STATE_CONNECTED && rssi != INT_MIN)
        lv_label_set_text_fmt(s_rssi_l, "Signal: %d dBm", rssi);
    else
        lv_label_set_text(s_rssi_l, "Signal: --");

    /* Provisioned → Change / Forget; unprovisioned → Set up WiFi (spec §4.3, #32). */
    bool provisioned = wifi_is_provisioned();
    if (provisioned) {
        lv_obj_remove_flag(s_btn_change, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_btn_forget, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_btn_setup, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_btn_change, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_btn_forget, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_btn_setup, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *screen_network_create(void) {
    s_sub = settings_make_subscreen("Network");
    s_status_l = lv_label_create(s_sub); lv_obj_set_pos(s_status_l, 8, 44);
    s_ssid_l   = lv_label_create(s_sub); lv_obj_set_pos(s_ssid_l,   8, 64);
    s_ip_l     = lv_label_create(s_sub); lv_obj_set_pos(s_ip_l,     8, 84);
    s_host_l   = lv_label_create(s_sub); lv_obj_set_pos(s_host_l,   8, 104);
    s_rssi_l   = lv_label_create(s_sub); lv_obj_set_pos(s_rssi_l,   8, 124);
    lv_obj_set_style_text_color(s_status_l, THEME_FG, 0);
    lv_obj_set_style_text_color(s_ssid_l,   THEME_FG, 0);
    lv_obj_set_style_text_color(s_ip_l,     THEME_FG, 0);
    lv_obj_set_style_text_color(s_host_l,   THEME_FG, 0);
    lv_obj_set_style_text_color(s_rssi_l,   THEME_FG, 0);

    s_btn_change = lv_button_create(s_sub);
    lv_obj_set_size(s_btn_change, 140, 32); lv_obj_set_pos(s_btn_change, 8, 170);
    lv_obj_add_event_cb(s_btn_change, change_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(s_btn_change); lv_label_set_text(cl, "Change network"); lv_obj_center(cl);

    s_btn_forget = lv_button_create(s_sub);
    lv_obj_set_size(s_btn_forget, 140, 32); lv_obj_set_pos(s_btn_forget, 8, 210);
    lv_obj_set_style_bg_color(s_btn_forget, THEME_STOP, 0);
    lv_obj_add_event_cb(s_btn_forget, forget_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fl = lv_label_create(s_btn_forget); lv_label_set_text(fl, "Forget WiFi"); lv_obj_center(fl);

    /* Shown instead of Change/Forget when no creds are saved (spec §4.3, #32).
     * Same target as Change — opens the scan list. */
    s_btn_setup = lv_button_create(s_sub);
    lv_obj_set_size(s_btn_setup, 140, 32); lv_obj_set_pos(s_btn_setup, 8, 170);
    lv_obj_add_event_cb(s_btn_setup, change_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(s_btn_setup); lv_label_set_text(sl, "Set up WiFi"); lv_obj_center(sl);

    refresh();
    return s_sub;
}

void screen_network_update(void) {
    /* Re-run refresh() from the UI tick while this sub-screen is visible, so
     * Status / IP / Signal track reality after a connect (#31). Cheap no-op
     * when the screen was never created or is hidden behind another. */
    if (s_sub && !lv_obj_has_flag(s_sub, LV_OBJ_FLAG_HIDDEN)) refresh();
}

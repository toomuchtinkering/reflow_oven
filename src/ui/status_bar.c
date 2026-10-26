#include "ui/status_bar.h"
#include "ui/theme.h"
#include "drivers/wifi.h"
#include "audio/audio_task.h"
#include <stdio.h>
#include <time.h>

static lv_obj_t *s_bar        = NULL;
static lv_obj_t *s_wifi_icon  = NULL;   /* the WiFi glyph label */
static lv_obj_t *s_wifi_slash = NULL;   /* red diagonal shown when disconnected */
static lv_obj_t *s_snd_icon   = NULL;   /* the speaker glyph label (always shown) */
static lv_obj_t *s_snd_slash  = NULL;   /* red diagonal shown when muted */
static lv_obj_t *s_clock_lbl  = NULL;

#define STATUS_BAR_H 24
#define ICON_W       20
#define ICON_H       16

/* Slash geometry, relative to an icon container's content area. Shared by
 * both icons (same size). lv_line stores the pointer, so it must be static. */
static lv_point_precise_t s_slash_pts[2] = {{2, 13}, {17, 2}};

/* Create a fixed-size transparent container holding a centred glyph plus a
 * hidden red diagonal slash on top. Mirrors the mvp-implementation mute
 * indicator: always show the glyph, convey the "off" state with the slash. */
static lv_obj_t *make_slashed_icon(lv_obj_t *parent, const char *sym, int x,
                                   lv_obj_t **out_slash) {
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, ICON_W, ICON_H);
    lv_obj_align(cont, LV_ALIGN_LEFT_MID, x, 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(cont);
    lv_label_set_text(lbl, sym);
    lv_obj_set_style_text_color(lbl, THEME_FG, 0);
    lv_obj_center(lbl);

    lv_obj_t *slash = lv_line_create(cont);
    lv_line_set_points(slash, s_slash_pts, 2);
    lv_obj_set_style_line_color(slash, THEME_STOP, 0);
    lv_obj_set_style_line_width(slash, 3, 0);
    lv_obj_set_style_line_rounded(slash, true, 0);
    lv_obj_add_flag(slash, LV_OBJ_FLAG_HIDDEN);

    *out_slash = slash;
    return lbl;
}

lv_obj_t *status_bar_create(lv_obj_t *parent) {
    s_bar = lv_obj_create(parent);
    lv_obj_set_size(s_bar, LV_HOR_RES, STATUS_BAR_H);
    lv_obj_set_pos(s_bar, 0, 0);
    lv_obj_set_style_bg_color(s_bar, THEME_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(s_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_bar, 4, 0);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_icon = make_slashed_icon(s_bar, LV_SYMBOL_WIFI,        0, &s_wifi_slash);
    s_snd_icon  = make_slashed_icon(s_bar, LV_SYMBOL_VOLUME_MAX, 24, &s_snd_slash);

    s_clock_lbl = lv_label_create(s_bar);
    lv_label_set_text(s_clock_lbl, "--:--");
    lv_obj_set_style_text_color(s_clock_lbl, THEME_FG, 0);
    lv_obj_align(s_clock_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    return s_bar;
}

/* RSSI is conveyed by the icon's OPACITY, not a wedge count — LVGL's built-in
 * LV_SYMBOL_WIFI is a single glyph (spec §3.2). Thresholds TUNE during bring-up. */
static lv_opa_t wifi_opa_for_rssi(int rssi) {
    if (rssi >= -55) return LV_OPA_COVER;
    if (rssi >= -65) return LV_OPA_80;
    if (rssi >= -75) return LV_OPA_60;
    return LV_OPA_40;
}

void status_bar_update(void) {
    wifi_state_t st = wifi_get_state();
    switch (st) {
        case WIFI_STATE_CONNECTED: {
            int r = wifi_get_rssi();
            lv_obj_set_style_text_color(s_wifi_icon, THEME_FG, 0);
            lv_obj_set_style_text_opa(s_wifi_icon, wifi_opa_for_rssi(r), 0);
            lv_obj_add_flag(s_wifi_slash, LV_OBJ_FLAG_HIDDEN);
            break;
        }
        case WIFI_STATE_CONNECTING:
        case WIFI_STATE_SCANNING:
            lv_obj_set_style_text_color(s_wifi_icon, THEME_FG_FAINT, 0);
            lv_obj_set_style_text_opa(s_wifi_icon, LV_OPA_60, 0);
            lv_obj_add_flag(s_wifi_slash, LV_OBJ_FLAG_HIDDEN);
            break;
        default:
            /* Not provisioned / disconnected — dim glyph with a red slash. */
            lv_obj_set_style_text_color(s_wifi_icon, THEME_FG_FAINT, 0);
            lv_obj_set_style_text_opa(s_wifi_icon, LV_OPA_COVER, 0);
            lv_obj_clear_flag(s_wifi_slash, LV_OBJ_FLAG_HIDDEN);
            break;
    }

    if (audio_is_muted()) lv_obj_clear_flag(s_snd_slash, LV_OBJ_FLAG_HIDDEN);
    else                  lv_obj_add_flag(s_snd_slash, LV_OBJ_FLAG_HIDDEN);

    char buf[8];
    if (wifi_time_synced()) {
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        snprintf(buf, sizeof buf, "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    } else {
        snprintf(buf, sizeof buf, "--:--");
    }
    lv_label_set_text(s_clock_lbl, buf);
}

#include "ui/screen_wifi_scan.h"
#include "ui/screen_settings.h"
#include "ui/screen_wifi_password.h"
#include "ui/theme.h"
#include "drivers/wifi.h"
#include "esp_lvgl_port.h"
#include <string.h>
#include <stdlib.h>

#define MAX_RESULTS 16

static lv_obj_t *s_sub  = NULL;
static lv_obj_t *s_list = NULL;
static wifi_scan_chosen_cb_t s_chosen_cb = NULL;

/* Results that live on the LVGL task (accessed only from LVGL task). */
static wifi_scan_result_t s_results[MAX_RESULTS];
static size_t s_n = 0;

/* Each scan's results are carried to the LVGL task in their own heap payload
 * via the lv_async_call arg, so a later scan completing can't overwrite an
 * in-flight buffer (the old shared s_pending_results race) and stale results
 * can't linger between scans (#42/#43). */
typedef struct { size_t n; wifi_scan_result_t r[MAX_RESULTS]; } scan_payload_t;

/* Scan lifecycle flags. The scan list and the password screen's keyboard can't
 * both fit in the 64 KB LVGL pool, so the list is freed before the password
 * screen opens and re-scanned when the user returns to it. */
static bool s_scan_inflight = false;   /* scan issued, awaiting SCAN_DONE */
static bool s_have_results  = false;   /* results applied for the current view */

static void on_scan_results(const wifi_scan_result_t *results, size_t n);  /* fwd */

static void start_scan(void) {
    if (s_scan_inflight) return;
    s_scan_inflight = true;
    wifi_scan_start(on_scan_results);
}

void screen_wifi_scan_set_chosen_cb(wifi_scan_chosen_cb_t cb) { s_chosen_cb = cb; }

/* Free the populated scan list before stacking the password screen on top.
 * The list (up to 16 rows ~= 11 KB) plus the lv_keyboard exceed the pool; the
 * list is hidden behind the password screen anyway. screen_wifi_scan_update()
 * re-scans when the user navigates back to the (now empty) list. */
static void free_list_for_password(void) {
    lv_obj_clean(s_list);
    s_n = 0;
    s_have_results = false;
}

static void on_ssid_chosen(const char *ssid, bool locked) {
    (void)locked;
    char ssid_copy[33];
    strncpy(ssid_copy, ssid, sizeof ssid_copy - 1);   /* ssid points into s_results; copy before freeing */
    ssid_copy[sizeof ssid_copy - 1] = 0;
    free_list_for_password();
    lv_obj_t *pw = screen_wifi_password_create(ssid_copy);
    settings_push_subscreen(pw);
}

static void row_clicked(lv_event_t *e) {
    size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx >= s_n) return;
    if (s_chosen_cb) s_chosen_cb(s_results[idx].ssid, s_results[idx].locked);
}

static void manual_clicked(lv_event_t *e) {
    (void)e;
    /* Manual entry: open password screen with empty SSID; user types both. */
    free_list_for_password();
    lv_obj_t *pw = screen_wifi_password_create(""); /* empty = manual mode */
    settings_push_subscreen(pw);
}

/* Called on the LVGL task via lv_async_call — safe to touch LVGL objects.
 * Owns the heap payload and frees it. */
static void apply_results_main(void *arg) {
    scan_payload_t *p = arg;
    s_n = p->n;
    memcpy(s_results, p->r, sizeof(wifi_scan_result_t) * s_n);
    free(p);

    /* Sort s_results[0 .. s_n) by RSSI descending. */
    for (size_t i = 1; i < s_n; i++) {
        wifi_scan_result_t cur = s_results[i];
        size_t j = i;
        while (j > 0 && s_results[j-1].rssi < cur.rssi) {
            s_results[j] = s_results[j-1];
            j--;
        }
        s_results[j] = cur;
    }

    lv_obj_clean(s_list);
    for (size_t i = 0; i < s_n; i++) {
        const char *icon = s_results[i].locked
            ? LV_SYMBOL_WIFI " " LV_SYMBOL_EYE_CLOSE
            : LV_SYMBOL_WIFI;
        lv_obj_t *b = lv_list_add_button(s_list, icon, s_results[i].ssid);
        lv_obj_set_style_bg_color(b, THEME_PANEL_BG, 0);
        lv_obj_set_style_bg_color(b, THEME_BORDER, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(b, THEME_FG, 0);
        lv_obj_add_event_cb(b, row_clicked, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    s_scan_inflight = false;
    s_have_results  = true;   /* even n==0 counts as "scanned" so update() doesn't loop */
}

/* Called from the WiFi event-loop task — must NOT touch LVGL directly. */
static void on_scan_results(const wifi_scan_result_t *results, size_t n) {
    scan_payload_t *p = malloc(sizeof *p);
    if (!p) return;
    p->n = n > MAX_RESULTS ? MAX_RESULTS : n;
    for (size_t i = 0; i < p->n; i++) p->r[i] = results[i];
    /* lv_async_call mutates the global LVGL timer list + heap; it MUST run under
     * the LVGL lock or it corrupts that list and the LVGL task spins forever
     * (the Change-network freeze). The wifi lock is already released before this
     * callback runs (SCAN_DONE path), so taking the LVGL lock here can't invert
     * lock order. */
    lvgl_port_lock(0);
    if (lv_async_call(apply_results_main, p) != LV_RESULT_OK) free(p);  /* don't leak if the async queue is full */
    lvgl_port_unlock();
}

lv_obj_t *screen_wifi_scan_create(void) {
    /* Singleton: build once, reuse forever. Re-creating this sub-screen on every
     * "Change network" leaked a multi-KB sub-screen each cycle; after a handful
     * the 64 KB LVGL pool exhausted -> lv_malloc fails -> LV_ASSERT_MALLOC ->
     * while(1) -> frozen UI (§4.2). On reuse just clear the rows and re-scan. */
    if (s_sub) {
        lv_obj_clean(s_list);
        s_n = 0;
        s_have_results = false;
        start_scan();
        return s_sub;
    }

    s_sub = settings_make_subscreen("Choose network");

    s_list = lv_list_create(s_sub);
    /* Height stops short of the Manual-entry button so the two no longer
     * overlap (was -80, which left the list bottom flush with the button top). */
    lv_obj_set_size(s_list, LV_HOR_RES - 8, LV_VER_RES - 24 - 36 - 88);
    lv_obj_set_pos(s_list, 0, 36);
    lv_obj_set_style_bg_color(s_list, THEME_BG, 0);
    lv_obj_set_style_border_opa(s_list, LV_OPA_TRANSP, 0);

    lv_obj_t *manual = lv_button_create(s_sub);
    lv_obj_set_size(manual, LV_HOR_RES - 16, 32);
    lv_obj_align(manual, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_event_cb(manual, manual_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ml = lv_label_create(manual);
    lv_label_set_text(ml, "Manual entry");
    lv_obj_center(ml);

    start_scan();
    screen_wifi_scan_set_chosen_cb(on_ssid_chosen);
    return s_sub;
}

/* Re-scan when the scan screen is revealed with an empty list — e.g. after
 * backing out of the password screen, which freed the list. Called from the UI
 * tick (mirrors screen_network_update); cheap no-op when hidden or already
 * populated/scanning. */
void screen_wifi_scan_update(void) {
    if (!s_sub || lv_obj_has_flag(s_sub, LV_OBJ_FLAG_HIDDEN)) return;
    if (s_have_results || s_scan_inflight) return;
    start_scan();
}

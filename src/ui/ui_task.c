#include "ui/ui_task.h"
#include "diag/crash_log.h"
#include "ui/theme.h"
#include "ui/status_bar.h"
#include "ui/screen_home.h"
#include "ui/screen_profile.h"
#include "ui/screen_dry.h"
#include "ui/screen_pid.h"
#include "ui/screen_pid_tuning.h"
#include "ui/screen_settings.h"
#include "ui/screen_network.h"
#include "ui/screen_about.h"
#include "ui/screen_wifi_scan.h"
#include "ui/screen_keypad.h"
#include "drivers/display.h"
#include "drivers/touch.h"
#include "storage/nvs_store.h"
#include "control/profile_curve.h"
#include "config.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include <string.h>

QueueHandle_t g_cmd_queue;          /* externed by the screen modules */
static oven_state_t *s_state;

/* UI-owned snapshot + curve copy. The refresh timer holds the state
 * mutex only long enough to memcpy these out of the publisher's
 * buffers; LVGL rendering then runs lock-free against the copy. This
 * keeps the control_task critical section bounded to a deterministic
 * memcpy regardless of how heavy the graph render becomes (spec
 * §3.3 curve-pointer ownership). */
/* The tabview, file-static so the refresh timer can switch to HOME when a dry
 * starts. */
static lv_obj_t *s_tv = NULL;

static oven_state_t s_snap;
static float        s_curve_copy[PROFILE_CURVE_MAX_SAMPLES];

/* Read by control_task's UI supervisor (see UI_STALL_TIMEOUT_MS). */
static volatile uint32_t s_heartbeat;

uint32_t ui_task_heartbeat(void) { return s_heartbeat; }

static void ui_refresh_timer(lv_timer_t *t) {
    (void)t;

    /* UI liveness heartbeat. esp_lvgl_port owns the LVGL task and neither
     * subscribes it to the task watchdog nor exposes a hook to feed one, so a
     * stalled counter here is the only externally visible symptom of a wedged
     * UI. Bumped first thing, before any work that could itself block, so the
     * beat means "the LVGL timer is still being scheduled" rather than "the
     * whole refresh completed". A naturally aligned 32-bit store is atomic on
     * this core, so the cross-task hand-off needs no lock. */
    s_heartbeat++;

    /* Rolling LVGL-pool sample into RTC memory. Taken here because this runs
     * inside the LVGL task with the lock already held — the crash path cannot
     * get this number when the UI is wedged holding that lock, which is
     * precisely the case we need it for. Self-rate-limited to 1 Hz. */
    crash_log_sample_pool((uint32_t)(esp_timer_get_time() / 1000));

    status_bar_update();

    /* ---- critical section: memcpy snapshot + curve ---- */
    xSemaphoreTake(s_state->mutex, portMAX_DELAY);
    s_snap = *s_state;
    uint16_t n = s_state->curve_len;
    if (n > PROFILE_CURVE_MAX_SAMPLES) n = PROFILE_CURVE_MAX_SAMPLES;
    if (s_state->curve && n) {
        memcpy(s_curve_copy, s_state->curve, n * sizeof(float));
    }
    xSemaphoreGive(s_state->mutex);
    /* ---- end critical section ---- */

    /* Re-anchor the snapshot's curve pointer to the UI-owned copy. */
    s_snap.curve = s_curve_copy;
    s_snap.curve_len = n;

    /* Drive the planned-curve line + transition overlays only when the curve
     * actually changes — set_curve recomputes hundreds of points and is
     * wasteful at 4 Hz (#37). The applied-profile seq covers user edits; the
     * sample-count check catches the initial publish (seq stays 0 at boot). */
    /* Flip the HOME graph between reflow and drying scaling before the curve
     * render below, so a dry that starts mid-session repaints the chrome in
     * the same tick. Idempotent, so an unconditional call costs nothing. */
    screen_home_set_dry_view(s_snap.dry_active ? s_snap.dry_target_c : 0.0f);

    static uint16_t s_last_curve_seq = 0xFFFF;
    static uint16_t s_last_curve_n   = 0xFFFF;
    if (s_snap.profile_applied_seq != s_last_curve_seq || n != s_last_curve_n) {
        s_last_curve_seq = s_snap.profile_applied_seq;
        s_last_curve_n   = n;
        screen_home_set_curve(s_curve_copy, n, s_snap.phase_end_ms);
    }

    /* A dry just started: bring the user to HOME, where the run renders. The
     * DRY tab has no live display of its own, so leaving them on it would look
     * like the START tap did nothing. */
    static bool s_prev_dry = false;
    if (s_snap.dry_active && !s_prev_dry && s_tv) {
        lv_tabview_set_active(s_tv, 0, LV_ANIM_ON);
    }
    s_prev_dry = s_snap.dry_active;

    screen_home_update(&s_snap);
    screen_profile_update(&s_snap);
    screen_dry_update(&s_snap);
    screen_pid_update(&s_snap);
    screen_pid_tuning_update(&s_snap);
    screen_network_update();   /* refreshes only while the Network sub-screen is visible */
    screen_wifi_scan_update();  /* re-scans when the scan list is shown empty */
    screen_about_update(&s_snap);  /* live TC-board temp, only while About is visible */
}

void ui_init(QueueHandle_t cmd_queue, oven_state_t *state) {
    g_cmd_queue = cmd_queue;
    s_state = state;

    display_handles_t d = display_init();
    esp_lcd_touch_handle_t tp = touch_init();

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));


    /* Single draw buffer (was double): on this PSRAM-less board each 320x48x2 B
     * buffer is ~30 KB of DMA-capable internal RAM — the same pool the system
     * heap draws from. Double-buffering cost ~60 KB and left only ~10 KB free,
     * which starved the WiFi/lwip stack on any large TCP transfer (serving the
     * SPA / receiving an OTA), wedging the device. Single buffer reclaims ~30 KB.
     * At the 4 Hz UI_REFRESH_MS tick the lost render/flush pipelining is
     * imperceptible. See the heap-starvation root cause.
     *
     * Band halved again, 48 -> 24 lines (~30 KB -> ~15 KB), to fund raising
     * LV_MEM_SIZE to 80 KB in sdkconfig.defaults: both come out of internal
     * RAM, so the swap is close to net-neutral and leaves OTA heap headroom
     * intact. It also cuts the render band itself, and LVGL allocates a draw
     * task per drawing operation per band — fewer objects intersect a 24-line
     * band, so the transient task queue that exhausted the pool in
     * lv_draw_add_task shrinks with it. Cost is twice as many flush chunks per
     * refresh, which the 4 Hz tick absorbs for the same reason as above. */
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = d.io, .panel_handle = d.panel,
        .buffer_size = 320 * 24, .double_buffer = false,
        .hres = 320, .vres = 480,
        .rotation = {.swap_xy = false, .mirror_x = true, .mirror_y = false},
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    lvgl_port_touch_cfg_t touch_cfg = {.disp = disp, .handle = tp};
    lvgl_port_add_touch(&touch_cfg);

    /* Build tabview */
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();
    theme_apply_to_screen(scr);
    status_bar_create(scr);
    lv_obj_t *tv = lv_tabview_create(scr);
    s_tv = tv;
    lv_tabview_set_tab_bar_position(tv, LV_DIR_BOTTOM);
    lv_tabview_set_tab_bar_size(tv, 36);
    lv_obj_set_pos(tv, 0, 24);
    lv_obj_set_size(tv, LV_HOR_RES, LV_VER_RES - 24);
    /* Defensive: paint the tabview body + its content container dark
     * so no LVGL-theme default (which can ship a light primary tint)
     * leaks through underneath the tab page. */
    lv_obj_set_style_bg_color(tv, THEME_BG, 0);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, 0);
    lv_obj_t *tv_content = lv_tabview_get_content(tv);
    if (tv_content) {
        lv_obj_set_style_bg_color(tv_content, THEME_BG, 0);
        lv_obj_set_style_bg_opa(tv_content, LV_OPA_COVER, 0);
        lv_obj_set_style_border_opa(tv_content, LV_OPA_TRANSP, 0);
    }

    lv_obj_t *tab_home    = lv_tabview_add_tab(tv, "HOME");
    lv_obj_t *tab_profile = lv_tabview_add_tab(tv, "PROFILE");
    lv_obj_t *tab_dry     = lv_tabview_add_tab(tv, "DRY");
    lv_obj_t *tab_settings = lv_tabview_add_tab(tv, LV_SYMBOL_SETTINGS);

    /* Bottom nav (tab bar): paint it black to match the rest of the UI.
     * The LVGL 9 default theme tints the bar + the active tab with a light
     * primary colour; override the bar and every tab button. The active tab
     * is marked by a THEME_ACCENT line above its label (top border, since the
     * bar sits at the bottom of the screen). */
    lv_obj_t *tab_bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(tab_bar, THEME_BG, 0);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(tab_bar, LV_OPA_TRANSP, 0);
    uint32_t tab_cnt = lv_obj_get_child_count(tab_bar);
    for (uint32_t i = 0; i < tab_cnt; i++) {
        lv_obj_t *btn = lv_obj_get_child(tab_bar, i);
        lv_obj_set_style_bg_color(btn, THEME_BG, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, THEME_PANEL_BG, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(btn, THEME_FG_DIM, 0);
        lv_obj_set_style_text_color(btn, THEME_FG, LV_STATE_CHECKED);
        lv_obj_set_style_border_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(btn, THEME_ACCENT, LV_STATE_CHECKED);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_TOP, LV_STATE_CHECKED);
        lv_obj_set_style_border_width(btn, 2, LV_STATE_CHECKED);
    }

    screen_home_create(tab_home);
    screen_profile_create(tab_profile);
    screen_dry_create(tab_dry);
    screen_settings_create(tab_settings);

    /* Load persisted values into the editors. */
    profile_t p; nvs_store_load_profile(&p);
    pid_gains_t g; nvs_store_load_pid(&g);
    screen_profile_set_initial(&p);
    screen_pid_set_initial(&g);

    lv_timer_create(ui_refresh_timer, UI_REFRESH_MS, NULL);
    lvgl_port_unlock();
}

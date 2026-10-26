#include "drivers/wifi.h"

wifi_state_t wifi_fsm_next(wifi_state_t cur, wifi_drv_event_t ev) {
    switch (cur) {
        case WIFI_STATE_UNINIT:
            if (ev == WIFI_EV_INIT_DONE)     return WIFI_STATE_IDLE;
            break;
        case WIFI_STATE_IDLE:
            if (ev == WIFI_EV_SCAN_START)    return WIFI_STATE_SCANNING;
            if (ev == WIFI_EV_CONNECT_START) return WIFI_STATE_CONNECTING;
            break;
        case WIFI_STATE_SCANNING:
            if (ev == WIFI_EV_SCAN_DONE)     return WIFI_STATE_IDLE;
            break;
        case WIFI_STATE_CONNECTING:
            if (ev == WIFI_EV_GOT_IP)        return WIFI_STATE_CONNECTED;
            if (ev == WIFI_EV_DISCONNECTED)  return WIFI_STATE_CONNECTING;  /* retry */
            if (ev == WIFI_EV_FORGET)        return WIFI_STATE_IDLE;
            break;
        case WIFI_STATE_CONNECTED:
            if (ev == WIFI_EV_DISCONNECTED)  return WIFI_STATE_CONNECTING;
            if (ev == WIFI_EV_FORGET)        return WIFI_STATE_IDLE;
            break;
    }
    return cur;
}

#ifdef ESP_PLATFORM
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <limits.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "mdns.h"

#include "config.h"
#include "storage/nvs_store.h"

static const char *TAG = "wifi";

/* ── Mutex (recursive so callbacks may call wifi_* without deadlock) ─────── */
static SemaphoreHandle_t s_lock = NULL;
#define LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(s_lock)

/* ── Shared state (all guarded by s_lock) ────────────────────────────────── */
static wifi_state_t   s_state        = WIFI_STATE_UNINIT;
static esp_ip4_addr_t s_ip           = {0};
static char           s_cur_ssid[WIFI_SSID_MAX_LEN + 1] = {0};
static char           s_cur_pass[WIFI_PASS_MAX_LEN + 1] = {0};
static int            s_retry_count  = 0;
static const int      s_backoffs_ms[] = WIFI_RETRY_BACKOFFS_MS;
_Static_assert(sizeof(s_backoffs_ms) / sizeof(s_backoffs_ms[0]) == WIFI_CONNECT_RETRY_MAX,
               "WIFI_RETRY_BACKOFFS_MS must have WIFI_CONNECT_RETRY_MAX entries");
static TimerHandle_t  s_retry_timer  = NULL;
static bool           s_wifi_started = false;  /* gates esp_wifi_connect before STA_START */
static bool           s_pending_creds = false; /* connect requested before STA_START */
static bool           s_creds_persisted = false; /* the *current* creds are saved in NVS (retry-class signal) */
static bool           s_provisioned     = false; /* NVS holds some creds; only cleared by forget (UI signal) */
static bool           s_forgotten    = false;  /* set by wifi_forget; cleared on new connect */

#define WIFI_STATUS_CB_MAX 4
static wifi_status_cb_t s_status_cbs[WIFI_STATUS_CB_MAX] = {0};
static wifi_scan_cb_t   s_scan_cb   = NULL;

/* Scan scratch buffers. on_event runs on the single esp_event task, so these
 * file-statics are safe and keep ~1.5 KB of arrays off that task's stack (#25). */
static wifi_ap_record_t    s_scan_recs[16];
static wifi_scan_result_t  s_scan_out[16];

/* Set true by the SNTP sync-notification callback once the clock has actually
 * been set, so the status-bar clock can show real time vs "--:--" rather than
 * guessing from a tm_year sentinel (#38). */
static volatile bool s_time_synced = false;
static void on_time_synced(struct timeval *tv) { (void)tv; s_time_synced = true; }
bool wifi_time_synced(void) { return s_time_synced; }

/* One-time discovery + time services. mDNS and SNTP are started once and then
 * persist across reconnects — mDNS re-announces automatically when the netif
 * gets a new IP, and SNTP keeps polling — so they are NOT re-initialised on a
 * forget + new-SSID connect (re-calling mdns_init() would error) (#19/#20).
 * Called from the GOT_IP path while holding the wifi lock (no lock drop, so no
 * TOCTOU window), and run on the single event task so the guards need no extra
 * sync. Each guard is set only on success, so a failed first attempt retries on
 * the next GOT_IP. */
static void start_discovery_services_once(void) {
    static bool mdns_up = false;
    static bool sntp_up = false;
    if (!mdns_up) {
        esp_err_t e = mdns_init();
        if (e == ESP_OK) {
            esp_err_t h = mdns_hostname_set(WIFI_HOSTNAME);
            mdns_instance_name_set("Reflow Oven");
            mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
            if (h != ESP_OK)
                ESP_LOGW(TAG, "mdns_hostname_set failed: %s (reflow.local may be taken)",
                         esp_err_to_name(h));
            mdns_up = true;
        } else {
            ESP_LOGW(TAG, "mdns_init failed: %s — will retry on next connect", esp_err_to_name(e));
        }
    }
    if (!sntp_up) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        sntp_set_time_sync_notification_cb(on_time_synced);
        esp_sntp_init();
        sntp_up = true;
    }
}

/*
 * publish() — MUST be called with the wifi lock NOT held. Status callbacks
 * marshal to the LVGL task via lv_async_call under the LVGL lock; the UI tick
 * takes the LVGL lock then the wifi lock, so invoking a cb that takes the LVGL
 * lock while holding the wifi lock would invert that order and deadlock (the
 * Change-network connect freeze). We snapshot the registry under a short lock,
 * then invoke the callbacks lock-free.
 */
static void publish(wifi_cb_event_t ev) {
    wifi_status_cb_t cbs[WIFI_STATUS_CB_MAX];
    LOCK();
    memcpy(cbs, s_status_cbs, sizeof cbs);
    UNLOCK();
    for (int i = 0; i < WIFI_STATUS_CB_MAX; i++)
        if (cbs[i]) cbs[i](ev);
}

bool wifi_subscribe_status(wifi_status_cb_t cb) {
    if (!cb) return false;
    LOCK();
    for (int i = 0; i < WIFI_STATUS_CB_MAX; i++) {
        if (s_status_cbs[i] == cb) { UNLOCK(); return true; }  /* idempotent */
        if (!s_status_cbs[i]) { s_status_cbs[i] = cb; UNLOCK(); return true; }
    }
    ESP_LOGW(TAG, "subscribe_status: registry full (max %d)", WIFI_STATUS_CB_MAX);
    UNLOCK();
    return false;
}

void wifi_unsubscribe_status(wifi_status_cb_t cb) {
    LOCK();
    for (int i = 0; i < WIFI_STATUS_CB_MAX; i++)
        if (s_status_cbs[i] == cb) s_status_cbs[i] = NULL;
    UNLOCK();
}

static void retry_timer_fire(TimerHandle_t t) {
    (void)t;
    LOCK();
    if (s_forgotten) {
        UNLOCK();
        return;  /* user has called wifi_forget; abandon retry */
    }
    ESP_LOGI(TAG, "retry attempt %d", s_retry_count + 1);
    UNLOCK();
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    /* publish() must run without the wifi lock (see its comment); defer the
     * notifications to after the critical section below. */
    bool pub_connected = false, pub_failed = false;
    LOCK();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_wifi_started = true;
        if (s_pending_creds) { s_pending_creds = false; UNLOCK(); esp_wifi_connect(); return; }
        else if (s_state == WIFI_STATE_CONNECTING) { UNLOCK(); esp_wifi_connect(); return; }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_state = wifi_fsm_next(s_state, WIFI_EV_DISCONNECTED);
        if (s_forgotten) {
            /* User forgot the network: do not retry. State is already IDLE
             * (set by wifi_forget's FORGET transition). (#17) */
        } else if (s_creds_persisted || s_retry_count < WIFI_CONNECT_RETRY_MAX) {
            /* Known-good (persisted) creds → retry forever while they're known,
             * so a long AP outage self-heals instead of trapping us in IDLE
             * (#16). A fresh trial connect (creds not yet persisted) only gets
             * WIFI_CONNECT_RETRY_MAX attempts so a bad password surfaces to the
             * UI. Past the backoff table, hold at the longest backoff. */
            int idx = s_retry_count < WIFI_CONNECT_RETRY_MAX
                    ? s_retry_count : WIFI_CONNECT_RETRY_MAX - 1;
            int backoff = s_backoffs_ms[idx];
            if (s_retry_count < WIFI_CONNECT_RETRY_MAX) s_retry_count++;
            ESP_LOGW(TAG, "disconnect; retry in %d ms (attempt %d%s)",
                     backoff, s_retry_count, s_creds_persisted ? ", known creds" : "");
            xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(backoff), 0);
            xTimerStart(s_retry_timer, 0);
        } else {
            ESP_LOGE(TAG, "connect failed after %d attempts — entering IDLE",
                     WIFI_CONNECT_RETRY_MAX);
            s_state = WIFI_STATE_IDLE;  /* exit CONNECTING; caller can wifi_connect_async to retry */
            pub_failed = true;
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);
        if (count > 16) count = 16;
        esp_wifi_scan_get_ap_records(&count, s_scan_recs);
        for (uint16_t i = 0; i < count; i++) {
            strncpy(s_scan_out[i].ssid, (const char*)s_scan_recs[i].ssid, 32);
            s_scan_out[i].ssid[32] = 0;
            s_scan_out[i].rssi   = s_scan_recs[i].rssi;
            s_scan_out[i].locked = (s_scan_recs[i].authmode != WIFI_AUTH_OPEN);
        }
        s_state = wifi_fsm_next(s_state, WIFI_EV_SCAN_DONE);
        wifi_scan_cb_t cb = s_scan_cb;
        UNLOCK();
        if (cb) cb(s_scan_out, count);
        return;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_ip = e->ip_info.ip;
        s_state = wifi_fsm_next(s_state, WIFI_EV_GOT_IP);
        s_retry_count = 0;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&s_ip));
        /* Persist creds ONLY here, in the GOT_IP path — i.e. only after a
         * connection has actually succeeded — so a mistyped password is never
         * written and retried on every boot (#22). Write once per credential
         * set to protect flash wear cycles. (Stored plaintext; NVS encryption
         * is out of scope per spec §1.2 — accepted residual risk.) */
        if (!s_creds_persisted) {
            if (nvs_store_save_wifi_creds(s_cur_ssid, s_cur_pass)) {
                s_creds_persisted = true;
                s_provisioned     = true;  /* NVS now holds these creds */
            }
        }
        start_discovery_services_once();  /* mDNS + SNTP, one-time; see helper (#19/#20/#23) */
        pub_connected = true;
    }
    UNLOCK();
    /* Notify subscribers outside the lock (publish() takes the LVGL lock). */
    if (pub_connected) publish(WIFI_CB_CONNECTED);
    if (pub_failed)    publish(WIFI_CB_CONNECT_FAILED);
}

void wifi_init(void) {
    if (s_state != WIFI_STATE_UNINIT) return;
    /* TZ set ONCE at init — independent of WiFi connect; localtime_r is safe
     * even before SNTP syncs (status_bar uses tm_year > 70 sentinel). */
    setenv("TZ", LOCAL_TZ, 1); tzset();

    /* Create recursive mutex BEFORE registering event handlers */
    s_lock = xSemaphoreCreateRecursiveMutex();
    configASSERT(s_lock);

    /* esp_netif_init / esp_event_loop_create_default are idempotent-safe:
     * other drivers may already have called them; ignore "already initialised". */
    (void)esp_netif_init();
    (void)esp_event_loop_create_default();
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();

    /* Name ourselves to the router. mDNS only ever named us on the .local
     * side; the DHCP client sends option 12 from the netif hostname, which
     * defaults to "espressif" — so the oven was indistinguishable from any
     * other ESP32 in the DHCP lease list. Set before esp_wifi_start(), i.e.
     * before the DHCP client can send its first DISCOVER, so the name is in
     * the very first lease rather than only after a renew. Non-fatal: a
     * misnamed oven still works, and this must not stop WiFi coming up. */
    if (sta) {
        esp_err_t hn = esp_netif_set_hostname(sta, WIFI_HOSTNAME);
        if (hn != ESP_OK)
            ESP_LOGW(TAG, "esp_netif_set_hostname failed: %s (router will show the default)",
                     esp_err_to_name(hn));
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());  /* fires WIFI_EVENT_STA_START asynchronously */

    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(5000), pdFALSE, NULL, retry_timer_fire);

    LOCK();
    s_state = wifi_fsm_next(s_state, WIFI_EV_INIT_DONE);
    UNLOCK();

    char ssid[WIFI_SSID_MAX_LEN + 1], pass[WIFI_PASS_MAX_LEN + 1];
    if (nvs_store_get_wifi_creds(ssid, pass)) {
        ESP_LOGI(TAG, "loading saved creds for SSID '%s'", ssid);
        wifi_connect_async(ssid, pass);
        /* Mark creds as already persisted — they came from NVS — and the
         * device as provisioned. */
        LOCK();
        s_creds_persisted = true;
        s_provisioned     = true;
        UNLOCK();
    } else {
        ESP_LOGI(TAG, "no creds in NVS — staying idle");
    }
}

wifi_state_t wifi_get_state(void) {
    LOCK();
    wifi_state_t st = s_state;
    UNLOCK();
    return st;
}

bool wifi_connect_async(const char *ssid, const char *pass) {
    LOCK();
    /* Detect new credentials — invalidate persisted flag so they get saved */
    if (strncmp(s_cur_ssid, ssid, WIFI_SSID_MAX_LEN) != 0 ||
        strncmp(s_cur_pass, pass, WIFI_PASS_MAX_LEN) != 0) {
        s_creds_persisted = false;
    }
    strncpy(s_cur_ssid, ssid, WIFI_SSID_MAX_LEN); s_cur_ssid[WIFI_SSID_MAX_LEN] = 0;
    strncpy(s_cur_pass, pass, WIFI_PASS_MAX_LEN); s_cur_pass[WIFI_PASS_MAX_LEN] = 0;

    wifi_config_t wc = {0};
    strncpy((char*)wc.sta.ssid,     s_cur_ssid, sizeof wc.sta.ssid - 1);
    strncpy((char*)wc.sta.password, s_cur_pass, sizeof wc.sta.password - 1);

    s_retry_count = 0;
    s_forgotten   = false;  /* re-enable retries on new connect */
    s_state = wifi_fsm_next(s_state, WIFI_EV_CONNECT_START);

    /* esp_wifi_connect requires that STA has started. If STA_START hasn't
     * fired yet (first wifi_connect_async after wifi_init), defer the call
     * until on_event sees STA_START. */
    bool deferred = !s_wifi_started;
    if (deferred) {
        /* STA_START hasn't fired yet; defer the connect to that event. */
        s_pending_creds = true;
        UNLOCK();
        return esp_wifi_set_config(WIFI_IF_STA, &wc) == ESP_OK;
    }

    /* Keep the lock held across set_config + disconnect + connect so the
     * DISCONNECTED event from the self-induced disconnect can't interleave
     * between the two calls and queue a stray retry / double connect (#18).
     * The event task simply blocks on the lock until this completes. */
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err == ESP_OK) {
        esp_wifi_disconnect();   /* in case we were already connecting */
        err = esp_wifi_connect();
    }
    UNLOCK();
    return err == ESP_OK;
}

bool wifi_forget(void) {
    nvs_store_forget_wifi();
    /* Mark forgotten FIRST, under the lock, so the disconnect event that
     * esp_wifi_disconnect() triggers sees s_forgotten=true and does not queue
     * a fresh retry that this path is trying to cancel (#17). */
    LOCK();
    s_forgotten       = true;
    s_creds_persisted = false;
    s_provisioned     = false;   /* NVS creds erased — UI flips to "Set up WiFi" */
    s_pending_creds   = false;
    s_state = wifi_fsm_next(s_state, WIFI_EV_FORGET);
    UNLOCK();
    if (s_retry_timer) xTimerStop(s_retry_timer, 0);  /* cancel any already-scheduled retry */
    esp_wifi_disconnect();
    publish(WIFI_CB_DISCONNECTED);   /* publish() takes its own short lock; must not hold it here */
    return true;
}

int wifi_get_rssi(void) {
    /* esp_wifi_sta_get_ap_info is thread-safe per ESP-IDF; no mutex needed.
     * Return INT_MIN (not 0) on error so callers can tell "no signal" apart
     * from a genuine 0 dBm reading and avoid painting full bars (#21). */
    wifi_ap_record_t info;
    return (esp_wifi_sta_get_ap_info(&info) == ESP_OK) ? info.rssi : INT_MIN;
}

bool wifi_is_provisioned(void) {
    /* "Provisioned" == NVS holds WiFi creds. Tracked by s_provisioned (set at
     * boot if NVS has creds and on a successful save, cleared only by forget)
     * rather than s_creds_persisted — the latter is cleared by a *trial*
     * connect with new creds, which would wrongly hide Forget for a network
     * still saved in NVS (#32 follow-up). Cached, so no NVS read per UI tick. */
    LOCK();
    bool p = s_provisioned;
    UNLOCK();
    return p;
}

bool wifi_get_ip_str(char buf[16]) {
    LOCK();
    if (s_state != WIFI_STATE_CONNECTED) { buf[0] = 0; UNLOCK(); return false; }
    snprintf(buf, 16, IPSTR, IP2STR(&s_ip));
    UNLOCK();
    return true;
}

bool wifi_get_ssid(char ssid[WIFI_SSID_MAX_LEN + 1]) {
    LOCK();
    if (s_state != WIFI_STATE_CONNECTED) { ssid[0] = 0; UNLOCK(); return false; }
    strncpy(ssid, s_cur_ssid, WIFI_SSID_MAX_LEN); ssid[WIFI_SSID_MAX_LEN] = 0;
    UNLOCK();
    return true;
}

bool wifi_scan_start(wifi_scan_cb_t cb) {
    LOCK();
    s_scan_cb = cb;
    /* Issue the scan FIRST; only advance the FSM to SCANNING if it actually
     * started. A failed start would otherwise leave us stuck in SCANNING with
     * no SCAN_DONE ever arriving, wedging all future scans (#24). */
    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err == ESP_OK) s_state = wifi_fsm_next(s_state, WIFI_EV_SCAN_START);
    UNLOCK();
    return err == ESP_OK;
}

#endif /* ESP_PLATFORM */

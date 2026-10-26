#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "config.h"

typedef enum {
    WIFI_STATE_UNINIT,
    WIFI_STATE_IDLE,
    WIFI_STATE_SCANNING,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
} wifi_state_t;

/* Renamed wifi_drv_event_t (not wifi_event_t) to avoid clash with ESP-IDF's
 * own wifi_event_t defined in esp_wifi_types_generic.h. */
typedef enum {
    WIFI_EV_INIT_DONE,
    WIFI_EV_SCAN_START,
    WIFI_EV_SCAN_DONE,
    WIFI_EV_CONNECT_START,
    WIFI_EV_GOT_IP,
    WIFI_EV_DISCONNECTED,
    WIFI_EV_FORGET,
} wifi_drv_event_t;

/* Pure FSM — exported for host-side unit tests. */
wifi_state_t wifi_fsm_next(wifi_state_t cur, wifi_drv_event_t ev);

/* Runtime (ESP32-only) ---------------------------------------------------*/
void wifi_init(void);                              /* sets TZ, starts STA, auto-connects if creds exist */
wifi_state_t wifi_get_state(void);
bool wifi_connect_async(const char *ssid, const char *pass); /* returns immediately; result via callback */
bool wifi_forget(void);
bool wifi_is_provisioned(void);                    /* true if WiFi creds are saved in NVS */
bool wifi_time_synced(void);                       /* true once SNTP has set the clock */
int  wifi_get_rssi(void);                          /* RSSI in dBm, or INT_MIN if unavailable */
bool wifi_get_ip_str(char buf[16]);
bool wifi_get_ssid(char ssid[WIFI_SSID_MAX_LEN + 1]);

/* Status callbacks for UI / server / etc. — registry-style so multiple
 * consumers (e.g. OTA server bringup + password-screen UI) can subscribe
 * simultaneously. Callbacks fire on the WiFi event-loop task; UI consumers
 * must marshal to the LVGL task via lv_async_call.
 *
 * Capacity is small (4 slots). Returns true if registration succeeded,
 * false if the table is full or cb is NULL. */
typedef enum { WIFI_CB_CONNECTED, WIFI_CB_CONNECT_FAILED, WIFI_CB_DISCONNECTED } wifi_cb_event_t;
typedef void (*wifi_status_cb_t)(wifi_cb_event_t ev);
bool wifi_subscribe_status(wifi_status_cb_t cb);
void wifi_unsubscribe_status(wifi_status_cb_t cb);

/* Scan */
typedef struct { char ssid[33]; int rssi; bool locked; } wifi_scan_result_t;
typedef void (*wifi_scan_cb_t)(const wifi_scan_result_t *results, size_t n);
bool wifi_scan_start(wifi_scan_cb_t cb);

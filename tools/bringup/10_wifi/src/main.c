/* Bring-up 10: WiFi STA + mDNS + DAC fault-tone coexistence smoke.
 * Set WIFI_SSID / WIFI_PASS below before flashing. */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "driver/dac_cosine.h"

#define WIFI_SSID "YOUR_SSID_HERE"
#define WIFI_PASS "YOUR_PASSWORD_HERE"

static const char *TAG = "bringup10";
static EventGroupHandle_t s_eg;
#define BIT_CONNECTED BIT0

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == WIFI_EVENT_STA_START)        esp_wifi_connect();
    if (id == WIFI_EVENT_STA_DISCONNECTED) { ESP_LOGW(TAG, "disconnect; retry"); esp_wifi_connect(); }
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_eg, BIT_CONNECTED);
    }
}

static void play_test_tone(void) {
    /* 4 kHz DAC cosine for ~400 ms — same DAC2 channel as the fault melody.
     * If WiFi RF activity glitches the DAC clock, we expect to hear it during
     * a connection attempt. Test passes if the tone is steady. */
    dac_cosine_handle_t h;
    dac_cosine_config_t cfg = {
        .chan_id = DAC_CHAN_1,  /* IO26 = DAC2 on ESP32-WROOM */
        .freq_hz = 4000,
        .clk_src = DAC_COSINE_CLK_SRC_DEFAULT,
        /* -6 dB attenuation (half VDD swing). DAC_COSINE_ATTEN_DEFAULT is
         * 0 dB / full-scale, which is dangerously loud through the FM8002E
         * power amp on this board. -6 dB is loud enough to clearly hear
         * glitches but won't blow out the speaker or the user's ears. */
        .atten   = DAC_COSINE_ATTEN_DB_6,
        .phase   = DAC_COSINE_PHASE_0,
        .offset  = 0,
        .flags = { .force_set_freq = false },
    };
    if (dac_cosine_new_channel(&cfg, &h) != ESP_OK) { ESP_LOGW(TAG, "DAC init failed"); return; }
    dac_cosine_start(h);
    vTaskDelay(pdMS_TO_TICKS(400));
    dac_cosine_stop(h);
    dac_cosine_del_channel(h);
}

void app_main(void) {
    s_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &on_event, NULL));

    wifi_config_t wc = {0};
    strncpy((char*)wc.sta.ssid,     WIFI_SSID, sizeof wc.sta.ssid - 1);
    strncpy((char*)wc.sta.password, WIFI_PASS, sizeof wc.sta.password - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* While WiFi is bringing up, play a tone and listen for glitches. */
    ESP_LOGI(TAG, "DAC coexistence smoke — playing 4kHz tone during connect");
    play_test_tone();

    xEventGroupWaitBits(s_eg, BIT_CONNECTED, false, true, portMAX_DELAY);

    /* Play again after connect to catch RF-active glitches. */
    ESP_LOGI(TAG, "DAC coexistence smoke — playing 4kHz tone post-connect");
    play_test_tone();

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("reflow"));
    ESP_ERROR_CHECK(mdns_instance_name_set("Reflow Oven (bring-up)"));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
    ESP_LOGI(TAG, "mDNS advertised: reflow.local");

    while (1) {
        wifi_ap_record_t info;
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
            ESP_LOGI(TAG, "RSSI: %d dBm, AP: %s", info.rssi, info.ssid);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

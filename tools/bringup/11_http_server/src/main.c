#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "mdns.h"

#define WIFI_SSID "YOUR_SSID_HERE"
#define WIFI_PASS "YOUR_PASSWORD_HERE"

static const char *TAG = "bringup11";
static EventGroupHandle_t s_eg;
#define BIT_CONNECTED BIT0

static esp_err_t hello_handler(httpd_req_t *req) {
    const char *html = "<!doctype html><h1>reflow oven bringup 11</h1>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == WIFI_EVENT_STA_START)        esp_wifi_connect();
    if (id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_eg, BIT_CONNECTED);
    }
}

void app_main(void) {
    s_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &on_event, NULL);
    wifi_config_t wc = {0};
    strncpy((char*)wc.sta.ssid,     WIFI_SSID, sizeof wc.sta.ssid - 1);
    strncpy((char*)wc.sta.password, WIFI_PASS, sizeof wc.sta.password - 1);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();

    xEventGroupWaitBits(s_eg, BIT_CONNECTED, false, true, portMAX_DELAY);

    mdns_init();
    mdns_hostname_set("reflow");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server;
    ESP_ERROR_CHECK(httpd_start(&server, &hcfg));
    httpd_uri_t hello = { .uri="/", .method=HTTP_GET, .handler=hello_handler };
    httpd_register_uri_handler(server, &hello);
    ESP_LOGI(TAG, "HTTP server up; visit http://reflow.local/");

    while (1) vTaskDelay(pdMS_TO_TICKS(60000));
}

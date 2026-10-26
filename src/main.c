/* src/main.c — boots NVS, drivers, control_task, and the LVGL UI task. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_system.h"

#include "drivers/audio.h"
#include "drivers/thermocouple.h"
#include "drivers/ssr.h"
#include "drivers/wifi.h"
#include "storage/nvs_store.h"
#include "ota/ota_server.h"
#include "ota/ota_validator.h"
#include "ota/boot_status.h"
#include "control/control_task.h"
#include "audio/audio_task.h"
#include "ui/ui_task.h"
#include "diag/crash_log.h"
#include "oven_state.h"
#include "config.h"

static const char *TAG = "main";

static void on_wifi_status_for_server(wifi_cb_event_t ev) {
    if (ev == WIFI_CB_CONNECTED && !ota_server_is_running()) ota_server_start();
}

static QueueHandle_t s_cmd_queue;
static oven_state_t  s_state;

static void shutdown_handler(void) {
    /* Graceful restart: drive SSR low. Panic / WDT-reset path relies on
     * the external pull-down + WDT timeout (§6.1 items 5-6, §6.2).
     */
    ssr_force_off();
}

void app_main(void) {
    ESP_LOGI(TAG, "reflow_oven boot");

    /* First, before any subsystem can record a fault or read one back: the
     * breadcrumb from a previous boot is only interpretable relative to the
     * counter this advances, and on a cold power-on this is what discards the
     * undefined contents of RTC memory. */
    crash_log_boot();
    ESP_LOGI(TAG, "boot #%u", (unsigned)crash_log_boot_count());

    /* WDT */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = CONTROL_TASK_WDT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&wdt_cfg));

    /* Reserve the control task's ~12 KB curve buffer now, while the heap is
     * still unfragmented. control_task_create runs last (after LVGL + WiFi
     * have carved up the heap), where this contiguous allocation otherwise
     * fails — the boot-time guru-meditation in control_task_create. */
    control_task_prealloc();

    /* Audio first — drives IO4 high (amp off) before anything else can
     * race the pin. DAC is allocated per-tone by the audio task later. */
    audio_init();

    /* Drivers */
    nvs_store_init();
    wifi_init();  /* non-blocking; auto-connects if creds present */
    wifi_subscribe_status(on_wifi_status_for_server);
    thermocouple_init();
    ssr_init();
    /* Register the shutdown handler *after* ssr_init so the GPIO is already
     * an output by the time a graceful restart could fire force_off. */
    esp_register_shutdown_handler(shutdown_handler);

    /* IPC */
    s_cmd_queue = xQueueCreate(8, sizeof(cmd_msg_t));
    s_state.mutex = xSemaphoreCreateMutex();

    /* Tasks */
    audio_task_start();                 /* before control_task — control posts to its queue */

    /* LVGL / UI must initialise before the OTA validator so that the
     * display+touch alive hooks can inspect the driver state.
     * control_task_create comes AFTER the validator so that the task body
     * reads boot_status_validation() with the correct value and can set
     * MODE_PENDING_VALIDATION as its initial state. */
    ui_init(s_cmd_queue, &s_state);

    if (ota_validator_pending()) {
        if (ota_validator_run_self_tests()) {
            ESP_LOGI("app", "OTA pending validation — awaiting user confirmation");
            boot_status_set_validation(BOOT_VALIDATION_PENDING);
        } else {
            ESP_LOGE("app", "OTA self-tests FAILED (reason: %s) — rollback on next reboot",
                     ota_validator_last_failure());
            boot_status_set_validation(BOOT_VALIDATION_FAILED);
            /* Unattended broken oven should be audible (finding #6). The
             * fault melody is self-bounded (~30 s); play it once. */
            audio_post_event(AUDIO_EVT_FAULT);
        }
    } else {
        /* Normal boot. Anchor the running slot as valid if it was never
         * marked (e.g. the USB-flashed base image, state UNDEFINED) so a
         * future failed OTA always has a valid slot to roll back into and
         * cannot brick the device (finding #1). */
        ota_validator_anchor_running();
    }

    control_task_create(s_cmd_queue, &s_state);
}

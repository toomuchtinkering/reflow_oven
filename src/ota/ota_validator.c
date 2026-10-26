#include "ota/ota_validator.h"
#include "config.h"
#include <stdint.h>
#include <string.h>

/* Pure consensus check for one TC sample window — host-testable. */
bool ota_validator_tc_window_ok(const float t[3], const uint8_t fault_flags[3]) {
    float lo = t[0], hi = t[0];
    for (int i = 0; i < 3; i++) {
        if (fault_flags[i] != 0) return false;
        if (t[i] < -50.0f || t[i] > 350.0f) return false;
        if (t[i] < lo) lo = t[i];
        if (t[i] > hi) hi = t[i];
    }
    return (hi - lo) <= OTA_TC_CONSENSUS_TOLERANCE;
}

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"   /* for vTaskDelay */
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"            /* for lv_display_get_default / lv_indev_get_* */
#include "drivers/thermocouple.h"
#include "drivers/ssr.h"

static const char *TAG = "validator";

bool ota_validator_hook_tc_alive(void) {
    /* Boot-time readings wiggle (WiFi bursts, LCD common-mode pickup — see
     * config.h), so try up to OTA_TC_WINDOW_ATTEMPTS consensus windows; one
     * quiet window proves the sensor is alive and plausible. A transport
     * failure inside a window only fails that window, not the whole test. */
    for (int a = 0; a < OTA_TC_WINDOW_ATTEMPTS; a++) {
        if (a) vTaskDelay(pdMS_TO_TICKS(OTA_TC_RETRY_GAP_MS));
        float t[3]; uint8_t flags[3] = {0xFF, 0xFF, 0xFF}; uint32_t age[3];
        for (int i = 0; i < 3; i++) {
            if (!thermocouple_read(&t[i], NULL, &flags[i], &age[i])) t[i] = -1000.0f;
            vTaskDelay(pdMS_TO_TICKS(OTA_TC_SAMPLE_GAP_MS));
        }
        if (ota_validator_tc_window_ok(t, flags)) return true;
    }
    return false;
}

bool ota_validator_hook_ssr_alive(void) {
    /* Call ssr_force_off; cannot read back GPIO output level meaningfully.
     * Smoke check: no crash + function returns to caller. */
    ssr_force_off();
    return true;
}

bool ota_validator_hook_display_alive(void) {
    /* Display is initialised at boot before validator runs. Aliveness check:
     * a default display handle exists in LVGL. */
    return lv_display_get_default() != NULL;
}

bool ota_validator_hook_touch_alive(void) {
    /* Touch is initialised at boot. Aliveness check: existing indev handle. */
    return lv_indev_get_act() != NULL || lv_indev_get_next(NULL) != NULL;
}

bool ota_validator_pending(void) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    return esp_ota_get_state_partition(run, &st) == ESP_OK
        && st == ESP_OTA_IMG_PENDING_VERIFY;
}
void ota_validator_mark_valid(void) {
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "marked valid");
}

void ota_validator_anchor_running(void) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK
            && st == ESP_OTA_IMG_UNDEFINED) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
            ESP_LOGI(TAG, "anchored running slot '%s' as valid (was UNDEFINED)", run->label);
    }
}
#endif

#ifndef ESP_PLATFORM
/* Weak default implementations for host builds. Tests override these
 * by providing non-weak definitions in their own .c file. */
__attribute__((weak)) bool ota_validator_hook_tc_alive(void)      { return true; }
__attribute__((weak)) bool ota_validator_hook_ssr_alive(void)     { return true; }
__attribute__((weak)) bool ota_validator_hook_display_alive(void) { return true; }
__attribute__((weak)) bool ota_validator_hook_touch_alive(void)   { return true; }
#endif

static const char *s_last_failure = "";
const char *ota_validator_last_failure(void) { return s_last_failure; }

static bool run_with_timeout(bool (*hook)(void), const char *name, int64_t budget_us) {
#ifdef ESP_PLATFORM
    int64_t start = esp_timer_get_time();
    bool ok = hook();
    int64_t elapsed = esp_timer_get_time() - start;
    if (!ok) { s_last_failure = name; return false; }
    if (elapsed > budget_us) { s_last_failure = name; return false; }  /* slow ≈ broken */
    return true;
#else
    (void)budget_us;
    bool ok = hook();
    if (!ok) s_last_failure = name;
    return ok;
#endif
}

bool ota_validator_run_self_tests(void) {
    s_last_failure = "";
    int64_t per_test_us  = (int64_t)(OTA_SELF_TEST_TIMEOUT_MS) * 1000;
    int64_t total_budget = (int64_t)(OTA_SELF_TEST_TOTAL_MS)  * 1000;
#ifdef ESP_PLATFORM
    int64_t start = esp_timer_get_time();
#endif
    if (!run_with_timeout(ota_validator_hook_tc_alive,      "tc",      per_test_us)) return false;
    if (!run_with_timeout(ota_validator_hook_ssr_alive,     "ssr",     per_test_us)) return false;
    if (!run_with_timeout(ota_validator_hook_display_alive, "display", per_test_us)) return false;
    if (!run_with_timeout(ota_validator_hook_touch_alive,   "touch",   per_test_us)) return false;
#ifdef ESP_PLATFORM
    if (esp_timer_get_time() - start > total_budget) {
        s_last_failure = "total-budget";
        return false;
    }
#else
    (void)total_budget;
#endif
    return true;
}

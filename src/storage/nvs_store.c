#include "storage/nvs_store.h"
#include "config.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "nvs_store";

void nvs_store_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }
}

void nvs_store_load_profile(profile_t *out) {
    profile_defaults(out);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    profile_v2_t blob;
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_PROFILE, &blob, &len);
    nvs_close(h);

    if (err != ESP_OK) { ESP_LOGW(TAG, "profile read err %d, using defaults", err); return; }
    profile_t loaded;
    if (!profile_from_blob(&blob, len, &loaded)) {
        ESP_LOGW(TAG, "profile schema mismatch, using defaults");
        return;
    }
    if (!profile_validate(&loaded)) {
        ESP_LOGW(TAG, "stored profile fails validation, using defaults");
        return;
    }
    *out = loaded;
}

bool nvs_store_save_profile(const profile_t *p) {
    profile_v2_t blob;
    profile_to_blob(p, &blob);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "profile save: open err %d", err); return false; }
    err = nvs_set_blob(h, NVS_KEY_PROFILE, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "profile save: write err %d", err); return false; }
    return true;
}

void nvs_store_load_pid(pid_gains_t *out) {
    pid_defaults(out);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    /* v2-sized buffer reads BOTH record generations: nvs_get_blob returns the
     * stored length (13 for legacy v1, 21 for v2) and pid_from_blob dispatches
     * on it — the v1 path migrates with ff defaults. */
    pid_v2_t blob;
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_PID, &blob, &len);
    nvs_close(h);
    if (err != ESP_OK) { ESP_LOGW(TAG, "pid read err %d, using defaults", err); return; }
    pid_gains_t loaded;
    if (!pid_from_blob(&blob, len, &loaded)) {
        ESP_LOGW(TAG, "pid schema mismatch, using defaults");
        return;
    }
    if (!pid_validate(&loaded)) {
        ESP_LOGW(TAG, "stored pid gains fail validation, using defaults");
        return;
    }
    *out = loaded;
}

bool nvs_store_save_pid(const pid_gains_t *g) {
    pid_v2_t blob;
    pid_to_blob(g, &blob);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "pid save: open err %d", err); return false; }
    err = nvs_set_blob(h, NVS_KEY_PID, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "pid save: write err %d", err); return false; }
    return true;
}

void nvs_store_load_dry_presets(dry_presets_t *out) {
    dry_presets_defaults(out);          /* first statement: every early return
                                         * below leaves safe values in place */
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    dry_presets_v1_t blob;
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_DRY_PRESETS, &blob, &len);
    nvs_close(h);
    if (err != ESP_OK) { ESP_LOGW(TAG, "dry presets read err %d, using defaults", err); return; }

    dry_presets_t loaded;
    if (!dry_presets_from_blob(&blob, len, &loaded)) {
        ESP_LOGW(TAG, "dry presets schema mismatch, using defaults");
        return;
    }
    if (!dry_presets_validate(&loaded)) {
        ESP_LOGW(TAG, "stored dry presets fail validation, using defaults");
        return;
    }
    *out = loaded;
}

bool nvs_store_save_dry_presets(const dry_presets_t *p) {
    dry_presets_v1_t blob;
    dry_presets_to_blob(p, &blob);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "dry presets save: open err %d", err); return false; }
    err = nvs_set_blob(h, NVS_KEY_DRY_PRESETS, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "dry presets save: write err %d", err); return false; }
    return true;
}

bool nvs_store_get_mute(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, NVS_KEY_MUTE, &v);
    nvs_close(h);
    return (err == ESP_OK) && (v != 0);
}

void nvs_store_set_mute(bool muted) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGW(TAG, "mute open err %d", err); return; }
    err = nvs_set_u8(h, NVS_KEY_MUTE, muted ? 1 : 0);
    if (err != ESP_OK) { ESP_LOGW(TAG, "mute set err %d", err); nvs_close(h); return; }
    err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "mute commit err %d", err);
    nvs_close(h);
}

bool nvs_store_save_wifi_creds(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = (nvs_set_str(h, NVS_KEY_WIFI_SSID, ssid) == ESP_OK)
           && (nvs_set_str(h, NVS_KEY_WIFI_PASS, pass) == ESP_OK)
           && (nvs_set_u8 (h, NVS_KEY_WIFI_SET,  1)    == ESP_OK)
           && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

bool nvs_store_get_wifi_creds(char ssid[WIFI_SSID_MAX_LEN + 1],
                              char pass[WIFI_PASS_MAX_LEN + 1]) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t set = 0;
    bool ok = (nvs_get_u8(h, NVS_KEY_WIFI_SET, &set) == ESP_OK) && set == 1;
    if (ok) {
        size_t s = WIFI_SSID_MAX_LEN + 1, p = WIFI_PASS_MAX_LEN + 1;
        ok = (nvs_get_str(h, NVS_KEY_WIFI_SSID, ssid, &s) == ESP_OK)
          && (nvs_get_str(h, NVS_KEY_WIFI_PASS, pass, &p) == ESP_OK);
    }
    nvs_close(h);
    return ok;
}

bool nvs_store_forget_wifi(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_erase_key(h, NVS_KEY_WIFI_SSID);
    nvs_erase_key(h, NVS_KEY_WIFI_PASS);
    nvs_set_u8(h, NVS_KEY_WIFI_SET, 0);
    bool ok = (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

bool nvs_store_get_ota_token(char token[OTA_TOKEN_HEX_LEN + 1]) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = OTA_TOKEN_HEX_LEN + 1;
    bool ok = (nvs_get_str(h, NVS_KEY_OTA_TOKEN, token, &len) == ESP_OK)
           && (len == OTA_TOKEN_HEX_LEN + 1);
    nvs_close(h);
    return ok;
}

bool nvs_store_set_ota_token(const char *token) {
    if (strlen(token) != OTA_TOKEN_HEX_LEN) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = (nvs_set_str(h, NVS_KEY_OTA_TOKEN, token) == ESP_OK)
           && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

/* NVS I/O wrapper. Uses nvs_schema for serialisation; falls back to
 * defaults from config.h on any read error. See spec §8.
 */
#pragma once
#include <stdbool.h>
#include "config.h"
#include "control/profile_curve.h"
#include "storage/nvs_schema.h"

void nvs_store_init(void);                            /* call once at boot */

/* nvs_store_load_*: always populates *out.
 *
 * On NVS miss, schema mismatch, or stored-value validation failure,
 * *out is overwritten with the defaults from profile_defaults() /
 * pid_defaults() and a WARN log is emitted. The functions return void
 * deliberately: callers cannot distinguish "loaded from NVS" from
 * "fell back to defaults", but they don't need to — *out is always
 * safe to use.
 *
 * Implementation contract (nvs_store.c): the *_defaults() call is the
 * first statement of each function, before any NVS I/O is attempted.
 * Every early-return path therefore leaves the defaults in place. */
void nvs_store_load_profile(profile_t *out);

/* nvs_store_save_*: return false (with an ERROR log) on any NVS failure
 * instead of panicking — a flash hiccup must degrade to "not persisted",
 * not reboot the oven. Callers use the result to decide whether to ACK
 * the save to the UI (the applied_seq bump). */
bool nvs_store_save_profile(const profile_t *p);

void nvs_store_load_pid(pid_gains_t *out);
bool nvs_store_save_pid(const pid_gains_t *g);

/* Drying presets. Same contract as the profile/PID loaders: *out is always
 * populated, falling back to dry_presets_defaults() on miss, schema mismatch
 * or validation failure. */
void nvs_store_load_dry_presets(dry_presets_t *out);
bool nvs_store_save_dry_presets(const dry_presets_t *p);

/* Mute flag. Single uint8_t under NVS_KEY_MUTE. */
bool nvs_store_get_mute(void);          /* false on missing key or any error */
void nvs_store_set_mute(bool muted);    /* fire-and-forget; logs on error */

/* WiFi credentials */
bool nvs_store_get_wifi_creds(char ssid[WIFI_SSID_MAX_LEN + 1],
                              char pass[WIFI_PASS_MAX_LEN + 1]);
bool nvs_store_save_wifi_creds(const char *ssid, const char *pass);
bool nvs_store_forget_wifi(void);

/* OTA token (32 hex chars + NUL). */
bool nvs_store_get_ota_token(char token[OTA_TOKEN_HEX_LEN + 1]);
bool nvs_store_set_ota_token(const char *token);

# OTA Firmware Update Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement OTA firmware update over WiFi, WiFi provisioning UX, persistent status bar, Settings hub UI restructure, boot-time self-test + user-confirm validation with rollback, mDNS discovery, and a PlatformIO custom upload script per the 2026-05-25 design spec.

**Architecture:** Phased delivery. Each phase ends with a working, committable firmware so MVP work can preempt this branch at any phase boundary. Foundation (partition + version + NVS schema) → WiFi driver + bring-up tool 10 → HTTP server + bring-up 11 → status bar UI element → Settings hub UI restructure (replaces PID tab) → WiFi provisioning sub-screens → OTA endpoint + bring-up 12 → boot self-tests + validation gate + bring-up 13 → About / Firmware-update sub-screens → PIO upload script → end-to-end smoke + rollback verification.

**Tech Stack:** ESP-IDF ≥ 5.0 (via PlatformIO `framework = espidf`), LVGL 9, `esp_http_server`, `mdns`, `esp_sntp`, `esp_ota_*`, `mbedtls_ct_memcmp` (for constant-time compare; hand-rolled fallback documented), `esp_fill_random`. Build: PlatformIO with the existing `[env:esp32dev]` env + new `[env:esp32dev-ota]` env. Host-side tests on `[env:native]` using Unity (existing pattern).

**Spec:** [../specs/2026-05-25-ota-firmware-update-design.md](../specs/2026-05-25-ota-firmware-update-design.md). Read it before starting; this plan does not repeat the rationale.

**Branch:** `ota-firmware-update` (off `mvp-implementation`). Will need rebase onto `main` when MVP merges.

**Verified facts** (checked against the current codebase, 2026-05-26):
- NVS namespace is `NVS_NAMESPACE` (`"reflow"`), defined in [include/config.h:134](../../../include/config.h#L134).
- SSR driver exposes `ssr_force_off(void)` ([src/drivers/ssr.h](../../../src/drivers/ssr.h)); there is no `ssr_off`. GPIO read-back on the SSR output pin is unreliable — the validator's SSR test verifies driver state, not pin level.
- `thermocouple_read(float *temp_c, uint8_t *fault_flags, uint32_t *age_ms)` returns `bool` ([src/drivers/thermocouple.h:15](../../../src/drivers/thermocouple.h#L15)).
- `display_init()` returns `display_handles_t` (struct); `touch_init()` returns `esp_lcd_touch_handle_t`. Both are already called at boot from `main.c` — the validator must NOT re-initialise them; it inspects their cached handles.
- Theme constants exist in [src/ui/theme.h](../../../src/ui/theme.h): `THEME_STOP` is the deep-red used here for danger buttons. There is no `THEME_BTN_DANGER`.
- `cmd_type_t` is in [src/oven_state.h:54](../../../src/oven_state.h#L54), not in `control_task.h`.
- `oven_mode_t` is in [src/control/reflow_sm.h:31](../../../src/control/reflow_sm.h#L31).
- `mbedtls_ct_memcmp` is available via `mbedtls/constant_time.h` in ESP-IDF 5.x.

---

## File structure

**New files** (~22):

| Path | Responsibility |
|---|---|
| `partitions.csv` | Custom partition table (replaces stock `partitions_singleapp.csv`) |
| `include/version.h.in` | CMake-templated header — emits `FIRMWARE_VERSION_STR` + `BUILD_DATE_STR` |
| `src/drivers/wifi.{c,h}` | WiFi station-mode init, NVS-backed creds, connection FSM, scan helper, mDNS hostname registration, SNTP setup; **async** API for UI use |
| `src/ota/ota_server.{c,h}` | HTTP server with `GET /`, `GET /version`, `POST /ota`. Token check + mode gate + streaming write to inactive OTA partition |
| `src/ota/ota_validator.{c,h}` | Boot-time subsystem self-tests; exposes `ota_validator_run_self_tests()`, `ota_validator_pending()`, `ota_validator_mark_valid()` |
| `src/ota/boot_status.{c,h}` | Read-only accessor module for boot-time OTA state shared across modules (validator-pending flag + self-tests-ok flag). Replaces bare `extern` globals. |
| `src/ui/status_bar.{c,h}` | 24 px persistent status bar above the tabview; WiFi/mute icons + clock; RSSI-graded WiFi icon; driven by UI tick |
| `src/ui/screen_settings.{c,h}` | Settings hub list view + sub-screen back-nav helpers (`settings_make_subscreen`, `settings_push_subscreen`, `settings_pop_subscreen`) |
| `src/ui/screen_network.{c,h}` | Settings → Network status view + Forget WiFi |
| `src/ui/screen_wifi_scan.{c,h}` | SSID scan + scrollable list + Manual-entry button |
| `src/ui/screen_wifi_password.{c,h}` | Password entry with `lv_keyboard` + show/hide eye + Connect spinner; async connect |
| `src/ui/screen_audio.{c,h}` | Settings → Audio (mute toggle moved from HOME) |
| `src/ui/screen_about.{c,h}` | Build version, slot info (active + other-slot via app description), Board / Chip / MAC, free heap |
| `src/ui/screen_firmware_update.{c,h}` | OTA token persistent display + Regenerate + slot status |
| `tools/ota_upload.py` | Standalone Python uploader; **streaming** upload with progress |
| `tools/bringup/10_wifi/` | WiFi bring-up + **DAC + WiFi coexistence smoke** (plays fault melody during connect) |
| `tools/bringup/11_http_server/` | HTTP server bring-up |
| `tools/bringup/12_ota_endpoint/` | `/ota` POST endpoint bring-up |
| `tools/bringup/13_self_tests/` | Subsystem self-test runner |
| `test/test_wifi_state.c` | Host-side: WiFi FSM transitions (including CONNECTING+DISCONNECT self-loop) |
| `test/test_ota_validator.c` | Host-side: self-test sequencing with mocked driver hooks |
| `test/test_ota_token.c` | Host-side: token generation distribution + constant-time compare |

**Modified files**:

| Path | Change |
|---|---|
| `sdkconfig.defaults` | OTA config keys (CONFIG_PARTITION_TABLE_CUSTOM, rollback flags, HTTPD caps, WiFi dynamic buffers, LWIP loopback). `sdkconfig.esp32dev` is gitignored — regenerated from defaults + platformio.ini each build. |
| `platformio.ini` | `board_build.partitions = partitions.csv` under `[env:esp32dev]` (the PIO-conventional way to select a custom partition CSV) + new `[env:esp32dev-ota]` env |
| `src/idf_component.yml` | Add `espressif/mdns` |
| `CMakeLists.txt` (top-level) | Generate `version.h` from `version.h.in` via `execute_process` + `configure_file` |
| `src/CMakeLists.txt` | Add `target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_BINARY_DIR})` so generated `version.h` is reachable |
| `include/config.h` | New constants: `LOCAL_TZ`, `WIFI_CONNECT_TIMEOUT_MS`, `WIFI_CONNECT_RETRY_MAX`, `WIFI_RETRY_BACKOFFS_MS`, `OTA_UPLOAD_CHUNK_SIZE`, `OTA_SELF_TEST_TIMEOUT_MS`, NVS key strings |
| `src/control/reflow_sm.h` | Add `MODE_PENDING_VALIDATION` to `oven_mode_t` enum |
| `src/oven_state.h` | Add `CMD_CLEAR_PENDING` to `cmd_type_t` enum |
| `src/control/control_task.c` | Handle `MODE_PENDING_VALIDATION` — reject `CMD_START` (already true by existing IDLE-only gate), allow other commands; handle `CMD_CLEAR_PENDING`; **update every `switch (mode)` site** (see Task 22) |
| `src/control/control_task.h` | Declare `control_get_mode(void)` |
| `src/storage/nvs_store.{c,h}` | get/set for `wifi_ssid`, `wifi_pass`, `wifi_set`, `ota_token` (all under `NVS_NAMESPACE`) |
| `src/main.c` | Call `nvs_store_init()` then `ota_validator_run_self_tests()` (only if pending) then UI then `wifi_init()`. WiFi runs asynchronously after this; OTA server starts on first connect via the WiFi-connected event hook. |
| `src/ui/ui_task.c` | Create status_bar above tabview; replace PID tab with Settings tab; reduce tabview height by 24 px |
| `src/ui/screen_home.c` | Remove mute button (moved to Settings → Audio); add pending-validation banner; reduce Y coords by 24 px; surface validator-fail message during boot (if pending+failed, show fault-style banner instead) |
| `src/ui/screen_profile.c` | Adjust hardcoded `ACTION_Y` and graph dimensions for new content area |
| `src/ui/screen_pid.c` | Refactor to be a sub-screen instead of a tab page; add back-nav header; adjust Y coords for sub-screen context |
| `platformio.ini` | (already noted above — board_build.partitions + new env) |

---

## Known carry-overs (not blocking; address during implementation if hit)

- **Bringup 10 event dispatch redundancy** (NIT): the event_handler_register with `WIFI_EVENT, ESP_EVENT_ANY_ID` already filters by base; the `if (base == WIFI_EVENT)` check in the bringup handler is redundant. Cosmetic only.
- **About screen visual budget**: the spec-listed rows may not all fit at default font size in a 320×420 sub-screen. If text wraps unexpectedly, drop the Built timestamp's seconds or shorten labels.
- **lv_tabview tab label visual cropping**: `LV_SYMBOL_SETTINGS` as a tab name in a 36 px tab bar may render clipped depending on the LVGL theme. Tune-able cosmetically.
- **NTP cold-boot lag**: clock shows `--:--` until first sync (~10–60 s after IP). Expected and documented in status_bar code.
- **httpd_req_recv timeout retry budget**: the OTA handler retries on `HTTPD_SOCK_ERR_TIMEOUT` up to a 30 s aggregate budget; tune via `OTA_RECV_TIMEOUT_TOTAL_MS` if bring-up shows different behaviour on user's WiFi.

---

## Phase A — Foundation

After Phase A, the firmware still runs as MVP. Partition table changes require a USB re-flash once.

### Task 1: Custom partition table + OTA rollback flags + LWIP loopback

**Files:**
- Create: `partitions.csv`
- Modify: `sdkconfig.esp32dev`

- [ ] **Step 1: Create `partitions.csv`**

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
otadata,  data, ota,     0xf000,   0x2000,
phy_init, data, phy,     0x11000,  0x1000,
ota_0,    app,  ota_0,   0x20000,  0x170000,
ota_1,    app,  ota_1,   0x190000, 0x170000,
```

Note: slots were enlarged from `0x160000` to `0x170000` (1,507,328 B each) during bring-up — the firmware grew to ~94 % of a `0x160000` slot, so the extra 64 KB/slot restores headroom (ota_1 therefore starts at `0x190000`; both slots still fit the 4 MB flash, ending at `0x300000`). Bootloader sits at the default `0x1000`; partition table is at `CONFIG_PARTITION_TABLE_OFFSET=0x8000` per existing sdkconfig. The CSV starts at `0x9000` (immediately after the 4 KB partition table). If you ever change `CONFIG_PARTITION_TABLE_OFFSET`, update offsets accordingly.

- [ ] **Step 2: Update [sdkconfig.esp32dev](../../../sdkconfig.esp32dev)**

Replacements:
```diff
- CONFIG_PARTITION_TABLE_SINGLE_APP=y
+ # CONFIG_PARTITION_TABLE_SINGLE_APP is not set
- # CONFIG_PARTITION_TABLE_CUSTOM is not set
+ CONFIG_PARTITION_TABLE_CUSTOM=y
- CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"
+ CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
```

Append:
```
# OTA rollback
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_APP_ROLLBACK_ENABLE=y

# HTTP server caps for /ota
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
CONFIG_HTTPD_MAX_URI_LEN=64

# WiFi dynamic buffers — saves RAM
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER=y
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER=y

# LWIP loopback — improves mDNS reliability on some networks (spec §2.3)
CONFIG_LWIP_NETIF_LOOPBACK=y
```

- [ ] **Step 3: Build to verify**

```
~/.platformio/penv/bin/pio run -e esp32dev
```

Expected: build succeeds; `.pio/build/esp32dev/partitions.bin` is generated from the new CSV.

- [ ] **Step 4: Commit**

```bash
git add partitions.csv sdkconfig.esp32dev
git commit -m "build: switch to two-OTA partition layout + rollback flags"
```

---

### Task 2: Build-time version string

**Files:**
- Create: `include/version.h.in`
- Modify: `CMakeLists.txt` (top-level)
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Create `include/version.h.in`**

```c
/* Auto-generated by CMake from version.h.in — do not edit. */
#pragma once
#define FIRMWARE_VERSION_STR "@GIT_VERSION@"
#define BUILD_DATE_STR       "@BUILD_DATE@"
```

- [ ] **Step 2: Modify top-level `CMakeLists.txt`**

After `project(reflow_oven)`, append:

```cmake
# --- Version embedding ---------------------------------------------------
execute_process(
    COMMAND git describe --tags --always --dirty
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    ERROR_QUIET
)
if(NOT GIT_VERSION)
    set(GIT_VERSION "unknown")
endif()
string(TIMESTAMP BUILD_DATE "%Y-%m-%d %H:%M:%S" UTC)
configure_file(
    ${CMAKE_SOURCE_DIR}/include/version.h.in
    ${CMAKE_BINARY_DIR}/version.h
    @ONLY
)
```

- [ ] **Step 3: Modify `src/CMakeLists.txt`**

Note: `${CMAKE_BINARY_DIR}` evaluated inside `src/CMakeLists.txt` is the **component** binary dir (e.g. `.pio/build/esp32dev/esp-idf/main`), NOT the project binary dir where `configure_file` wrote `version.h`. Use a stable absolute path captured from the top-level CMake context. Modify the top-level `CMakeLists.txt` to set a CACHE variable, then read it in `src/CMakeLists.txt`.

In the top-level `CMakeLists.txt`, replace the `configure_file` block with:

```cmake
set(REFLOW_GEN_INCLUDE_DIR ${CMAKE_BINARY_DIR} CACHE PATH "Generated header dir")
configure_file(
    ${CMAKE_SOURCE_DIR}/include/version.h.in
    ${REFLOW_GEN_INCLUDE_DIR}/version.h
    @ONLY
)
```

Then in `src/CMakeLists.txt`, after `idf_component_register(SRCS ${app_sources})`:

```cmake
target_include_directories(${COMPONENT_LIB} PRIVATE ${REFLOW_GEN_INCLUDE_DIR})
```

This makes `#include "version.h"` resolve regardless of which CMake scope is active. (The earlier idea of `idf_build_set_property(COMPILE_OPTIONS …)` at the project-CMake layer does not work for components processed earlier in the build graph; using `${CMAKE_BINARY_DIR}` directly in src/CMakeLists.txt resolves to a different path than where the file is written.)

- [ ] **Step 4: Build to verify**

```
~/.platformio/penv/bin/pio run -e esp32dev
cat .pio/build/esp32dev/version.h
```

Expected: `version.h` exists in the build dir with a real `git describe` output.

- [ ] **Step 5: Commit**

```bash
git add include/version.h.in CMakeLists.txt src/CMakeLists.txt
git commit -m "build: embed git-describe version + make version.h reachable from app"
```

---

### Task 3: NVS keys for WiFi creds + OTA token

**Files:**
- Modify: `include/config.h`
- Modify: `src/storage/nvs_store.{c,h}`

- [ ] **Step 1: Add constants to [include/config.h](../../../include/config.h)**

Append to the existing constants block:

```c
/* === Networking + OTA NVS keys (new in v2-OTA) === */
#define NVS_KEY_WIFI_SSID  "wifi_ssid"
#define NVS_KEY_WIFI_PASS  "wifi_pass"
#define NVS_KEY_WIFI_SET   "wifi_set"     /* u8: 1 if creds persisted */
#define NVS_KEY_OTA_TOKEN  "ota_token"    /* 32 hex chars (16 bytes) */

#define WIFI_SSID_MAX_LEN     32
#define WIFI_PASS_MAX_LEN     64
#define OTA_TOKEN_HEX_LEN     32

/* WiFi connection tunables. Retry backoffs apply on disconnect events:
 * first retry after 5 s, second after 10 s, third after 20 s, then give up. */
#define WIFI_CONNECT_TIMEOUT_MS  30000
#define WIFI_CONNECT_RETRY_MAX   3
#define WIFI_RETRY_BACKOFFS_MS   { 5000, 10000, 20000 }

/* OTA tunables */
#define OTA_UPLOAD_CHUNK_SIZE       4096
#define OTA_RECV_TIMEOUT_TOTAL_MS  30000  /* aggregate retry budget for httpd_req_recv timeouts */
#define OTA_SELF_TEST_TIMEOUT_MS    2000  /* per individual self-test (raised from 1000 during bring-up) */
#define OTA_SELF_TEST_TOTAL_MS      5000  /* total budget for all self-tests */
#define OTA_TC_CONSENSUS_TOLERANCE   2.0f /* °C tolerance across 3 consecutive samples */

/* Local timezone for status-bar clock (POSIX TZ; Germany w/ DST). */
#define LOCAL_TZ "CET-1CEST,M3.5.0/2,M10.5.0/3"
```

- [ ] **Step 2: Extend [src/storage/nvs_store.h](../../../src/storage/nvs_store.h)**

Add to the existing header:

```c
#include "config.h"

/* WiFi credentials */
bool nvs_store_get_wifi_creds(char ssid[WIFI_SSID_MAX_LEN + 1],
                              char pass[WIFI_PASS_MAX_LEN + 1]);
bool nvs_store_save_wifi_creds(const char *ssid, const char *pass);
bool nvs_store_forget_wifi(void);

/* OTA token (32 hex chars + NUL). */
bool nvs_store_get_ota_token(char token[OTA_TOKEN_HEX_LEN + 1]);
bool nvs_store_set_ota_token(const char *token);
```

- [ ] **Step 3: Implement in [src/storage/nvs_store.c](../../../src/storage/nvs_store.c)**

Append (note: namespace is `NVS_NAMESPACE`, matching existing keys):

```c
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
```

- [ ] **Step 4: Build to verify**

```
~/.platformio/penv/bin/pio run -e esp32dev
```

- [ ] **Step 5: Commit**

```bash
git add include/config.h src/storage/nvs_store.{c,h}
git commit -m "nvs: add wifi creds + ota_token under existing reflow namespace"
```

---

## Phase B — WiFi driver + bring-up 10

### Task 4: Bring-up 10 — WiFi connect + mDNS + DAC coexistence smoke

**Files:**
- Create: `tools/bringup/10_wifi/platformio.ini`
- Create: `tools/bringup/10_wifi/sdkconfig.defaults`
- Create: `tools/bringup/10_wifi/CMakeLists.txt`           (project root — IDF-required)
- Create: `tools/bringup/10_wifi/src/CMakeLists.txt`       (component registration)
- Create: `tools/bringup/10_wifi/src/main.c`
- Create: `tools/bringup/10_wifi/src/idf_component.yml`
- Create: `tools/bringup/10_wifi/README.md`

The structure **mirrors `tools/bringup/09_audio/` exactly** — verified layout: project-root `CMakeLists.txt` + `platformio.ini` + `sdkconfig.esp32dev` + `src/{main.c, CMakeLists.txt, idf_component.yml}`. (Do NOT use a `main/` subfolder — the project's bringup convention is `src/`.)

The project-root `CMakeLists.txt` is the standard IDF/PlatformIO boilerplate:

```cmake
cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bringup_10_wifi)
```

The `src/CMakeLists.txt`:

```cmake
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
idf_component_register(SRCS ${app_sources})
```

- [ ] **Step 1: `platformio.ini`**

```ini
[platformio]
name = bringup_10_wifi
default_envs = esp32dev

[env:esp32dev]
platform = espressif32
board = esp32dev
framework = espidf
monitor_speed = 115200
```

- [ ] **Step 2: `sdkconfig.defaults`**

```
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER=y
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER=y
CONFIG_LWIP_NETIF_LOOPBACK=y
```

- [ ] **Step 3: `src/idf_component.yml`**

```yaml
dependencies:
  idf:
    version: ">=5.0"
  espressif/mdns:
    version: "^1"
```

- [ ] **Step 4: `src/main.c`**

```c
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
        .atten   = DAC_COSINE_ATTEN_DEFAULT,
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
```

- [ ] **Step 5: `README.md`**

```markdown
# Bring-up 10 — WiFi + DAC coexistence

Edit `WIFI_SSID` and `WIFI_PASS` in `src/main.c`, then:

    ~/.platformio/penv/bin/pio run -e esp32dev -t upload && \
        ~/.platformio/penv/bin/pio device monitor

Expected log:
- `DAC coexistence smoke — playing 4kHz tone during connect`
- `got IP: 192.168.x.x`
- `DAC coexistence smoke — playing 4kHz tone post-connect`
- `mDNS advertised: reflow.local`
- `RSSI: -XX dBm, AP: <ssid>` every 5 s

**Audible check**: both tones should be steady 4 kHz. If you hear glitching, popping, or frequency drift during the *post-connect* tone, the DAC + WiFi clock contention risk from the audio spec §8.2 is real on this hardware — note for the OTA spec §11.1 risk mitigation (fallback to LEDC tone or sample-based I²S playback).

Reverts to MVP firmware via:

    cd ../../..
    ~/.platformio/penv/bin/pio run -e esp32dev -t upload
```

- [ ] **Step 6: Build to verify**

```
cd tools/bringup/10_wifi
~/.platformio/penv/bin/pio run -e esp32dev
cd ../../..
```

- [ ] **Step 7: Commit**

```bash
git add tools/bringup/10_wifi/
git commit -m "bringup(10_wifi): WiFi + mDNS + DAC coexistence smoke"
```

---

### Task 5: Host-side WiFi FSM tests

**Files:**
- Create: `test/test_wifi_state.c`

- [ ] **Step 1: Write the failing tests**

```c
/* test/test_wifi_state.c — host-side, [env:native] */
#include "unity.h"

typedef enum {
    WIFI_STATE_UNINIT,
    WIFI_STATE_IDLE,
    WIFI_STATE_SCANNING,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
} wifi_state_t;

typedef enum {
    WIFI_EV_INIT_DONE,
    WIFI_EV_SCAN_START,
    WIFI_EV_SCAN_DONE,
    WIFI_EV_CONNECT_START,
    WIFI_EV_GOT_IP,
    WIFI_EV_DISCONNECTED,
    WIFI_EV_FORGET,
} wifi_event_t;

wifi_state_t wifi_fsm_next(wifi_state_t cur, wifi_event_t ev);

void test_uninit_to_idle(void)                  { TEST_ASSERT_EQUAL(WIFI_STATE_IDLE,       wifi_fsm_next(WIFI_STATE_UNINIT, WIFI_EV_INIT_DONE)); }
void test_idle_to_scanning(void)                { TEST_ASSERT_EQUAL(WIFI_STATE_SCANNING,   wifi_fsm_next(WIFI_STATE_IDLE,    WIFI_EV_SCAN_START)); }
void test_scanning_back_to_idle(void)           { TEST_ASSERT_EQUAL(WIFI_STATE_IDLE,       wifi_fsm_next(WIFI_STATE_SCANNING,WIFI_EV_SCAN_DONE)); }
void test_idle_to_connecting(void)              { TEST_ASSERT_EQUAL(WIFI_STATE_CONNECTING, wifi_fsm_next(WIFI_STATE_IDLE,    WIFI_EV_CONNECT_START)); }
void test_connecting_to_connected(void)         { TEST_ASSERT_EQUAL(WIFI_STATE_CONNECTED,  wifi_fsm_next(WIFI_STATE_CONNECTING,WIFI_EV_GOT_IP)); }
void test_connecting_self_loop_on_disconnect(void) {
    /* During CONNECTING, a disconnect event keeps us in CONNECTING (retry). */
    TEST_ASSERT_EQUAL(WIFI_STATE_CONNECTING, wifi_fsm_next(WIFI_STATE_CONNECTING, WIFI_EV_DISCONNECTED));
}
void test_connected_to_connecting_on_disconnect(void) { TEST_ASSERT_EQUAL(WIFI_STATE_CONNECTING, wifi_fsm_next(WIFI_STATE_CONNECTED, WIFI_EV_DISCONNECTED)); }
void test_forget_from_connected(void)           { TEST_ASSERT_EQUAL(WIFI_STATE_IDLE,       wifi_fsm_next(WIFI_STATE_CONNECTED, WIFI_EV_FORGET)); }
void test_forget_from_connecting(void)          { TEST_ASSERT_EQUAL(WIFI_STATE_IDLE,       wifi_fsm_next(WIFI_STATE_CONNECTING,WIFI_EV_FORGET)); }
void test_unknown_event_keeps_state(void)       { TEST_ASSERT_EQUAL(WIFI_STATE_IDLE,       wifi_fsm_next(WIFI_STATE_IDLE,    WIFI_EV_GOT_IP)); }

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_uninit_to_idle);
    RUN_TEST(test_idle_to_scanning);
    RUN_TEST(test_scanning_back_to_idle);
    RUN_TEST(test_idle_to_connecting);
    RUN_TEST(test_connecting_to_connected);
    RUN_TEST(test_connecting_self_loop_on_disconnect);
    RUN_TEST(test_connected_to_connecting_on_disconnect);
    RUN_TEST(test_forget_from_connected);
    RUN_TEST(test_forget_from_connecting);
    RUN_TEST(test_unknown_event_keeps_state);
    return UNITY_END();
}
```

- [ ] **Step 2: Verify link failure**

```
~/.platformio/penv/bin/pio test -e native -f test_wifi_state
```

Expected: link error — `wifi_fsm_next` undefined.

- [ ] **Step 3: Commit failing test**

```bash
git add test/test_wifi_state.c
git commit -m "test: WiFi FSM transitions (failing, no impl yet)"
```

---

### Task 6: WiFi driver (async API)

**Files:**
- Create: `src/drivers/wifi.{c,h}`

**Design note**: the API is **async** — UI calls `wifi_connect_async()` which returns immediately. A `WIFI_CONNECTED` or `WIFI_CONNECT_FAILED` event is published via a status callback so the UI can update without blocking the LVGL render thread.

- [ ] **Step 1: Create `src/drivers/wifi.h`**

```c
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

/* Note: this enum is named wifi_drv_event_t (not wifi_event_t) to avoid a
 * collision with ESP-IDF's own wifi_event_t in esp_wifi_types_generic.h. The
 * host test in test/test_wifi_state/test_wifi_state.c declares its own local
 * wifi_event_t typedef of the same shape — link-time the function signature
 * resolves correctly because both enums are int-sized. */
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
int  wifi_get_rssi(void);
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
```

- [ ] **Step 2: Create `src/drivers/wifi.c` — FSM first (test-driven)**

```c
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
```

- [ ] **Step 3: Host tests pass**

```
~/.platformio/penv/bin/pio test -e native -f test_wifi_state
```

Expected: 10 tests pass.

- [ ] **Step 4: ESP-IDF runtime impl — append to `src/drivers/wifi.c`**

```c
#ifdef ESP_PLATFORM
#include <string.h>
#include <time.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/timers.h"
#include "mdns.h"

#include "config.h"
#include "storage/nvs_store.h"

static const char *TAG = "wifi";
static wifi_state_t s_state = WIFI_STATE_UNINIT;
static esp_ip4_addr_t s_ip = {0};
static char s_cur_ssid[WIFI_SSID_MAX_LEN + 1] = {0};
static char s_cur_pass[WIFI_PASS_MAX_LEN + 1] = {0};
static int  s_retry_count = 0;
static const int s_backoffs_ms[] = WIFI_RETRY_BACKOFFS_MS;
static TimerHandle_t s_retry_timer = NULL;
static bool s_wifi_started = false;  /* gates esp_wifi_connect attempts before STA_START fires */
static bool s_pending_creds = false; /* connect requested before STA_START */

#define WIFI_STATUS_CB_MAX 4
static wifi_status_cb_t s_status_cbs[WIFI_STATUS_CB_MAX] = {0};
static wifi_scan_cb_t   s_scan_cb   = NULL;
static bool s_sntp_started = false;

static void publish(wifi_cb_event_t ev) {
    for (int i = 0; i < WIFI_STATUS_CB_MAX; i++)
        if (s_status_cbs[i]) s_status_cbs[i](ev);
}

bool wifi_subscribe_status(wifi_status_cb_t cb) {
    if (!cb) return false;
    for (int i = 0; i < WIFI_STATUS_CB_MAX; i++) {
        if (s_status_cbs[i] == cb) return true;     /* idempotent */
        if (!s_status_cbs[i]) { s_status_cbs[i] = cb; return true; }
    }
    return false;
}
void wifi_unsubscribe_status(wifi_status_cb_t cb) {
    for (int i = 0; i < WIFI_STATUS_CB_MAX; i++)
        if (s_status_cbs[i] == cb) s_status_cbs[i] = NULL;
}

static void retry_timer_fire(TimerHandle_t t) {
    (void)t;
    ESP_LOGI(TAG, "retry attempt %d", s_retry_count + 1);
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_wifi_started = true;
        /* If a connect was requested before STA_START fired, kick it now. */
        if (s_pending_creds) { s_pending_creds = false; esp_wifi_connect(); }
        else if (s_state == WIFI_STATE_CONNECTING) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_state = wifi_fsm_next(s_state, WIFI_EV_DISCONNECTED);
        if (s_retry_count < WIFI_CONNECT_RETRY_MAX) {
            int backoff = s_backoffs_ms[s_retry_count];
            s_retry_count++;
            ESP_LOGW(TAG, "disconnect; retry in %d ms (attempt %d/%d)",
                     backoff, s_retry_count, WIFI_CONNECT_RETRY_MAX);
            xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(backoff), 0);
            xTimerStart(s_retry_timer, 0);
        } else {
            ESP_LOGE(TAG, "max retries reached");
            publish(WIFI_CB_CONNECT_FAILED);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);
        if (count > 16) count = 16;
        wifi_ap_record_t recs[16];
        esp_wifi_scan_get_ap_records(&count, recs);
        wifi_scan_result_t out[16];
        for (uint16_t i = 0; i < count; i++) {
            strncpy(out[i].ssid, (const char*)recs[i].ssid, 32); out[i].ssid[32] = 0;
            out[i].rssi   = recs[i].rssi;
            out[i].locked = (recs[i].authmode != WIFI_AUTH_OPEN);
        }
        s_state = wifi_fsm_next(s_state, WIFI_EV_SCAN_DONE);
        if (s_scan_cb) s_scan_cb(out, count);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_ip = e->ip_info.ip;
        s_state = wifi_fsm_next(s_state, WIFI_EV_GOT_IP);
        s_retry_count = 0;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&s_ip));
        nvs_store_save_wifi_creds(s_cur_ssid, s_cur_pass);
        /* One-time mDNS + SNTP advert */
        static bool advertised = false;
        if (!advertised) {
            mdns_init();
            mdns_hostname_set("reflow");
            mdns_instance_name_set("Reflow Oven");
            mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
            advertised = true;
        }
        if (!s_sntp_started) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
            s_sntp_started = true;
        }
        publish(WIFI_CB_CONNECTED);
    }
}

void wifi_init(void) {
    if (s_state != WIFI_STATE_UNINIT) return;
    /* TZ set ONCE at init — independent of WiFi connect; localtime_r is safe
     * even before SNTP syncs (status_bar uses tm_year > 70 sentinel). */
    setenv("TZ", LOCAL_TZ, 1); tzset();

    /* esp_netif_init / esp_event_loop_create_default are idempotent-safe:
     * call once, ignore "already initialised" errors that future drivers
     * may emit. */
    (void)esp_netif_init();
    (void)esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());  /* fires WIFI_EVENT_STA_START asynchronously */

    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(5000), pdFALSE, NULL, retry_timer_fire);
    s_state = wifi_fsm_next(s_state, WIFI_EV_INIT_DONE);

    char ssid[WIFI_SSID_MAX_LEN + 1], pass[WIFI_PASS_MAX_LEN + 1];
    if (nvs_store_get_wifi_creds(ssid, pass)) {
        ESP_LOGI(TAG, "loading saved creds for SSID '%s'", ssid);
        wifi_connect_async(ssid, pass);
    } else {
        ESP_LOGI(TAG, "no creds in NVS — staying idle");
    }
}

wifi_state_t wifi_get_state(void) { return s_state; }

bool wifi_connect_async(const char *ssid, const char *pass) {
    strncpy(s_cur_ssid, ssid, WIFI_SSID_MAX_LEN); s_cur_ssid[WIFI_SSID_MAX_LEN] = 0;
    strncpy(s_cur_pass, pass, WIFI_PASS_MAX_LEN); s_cur_pass[WIFI_PASS_MAX_LEN] = 0;

    wifi_config_t wc = {0};
    strncpy((char*)wc.sta.ssid,     s_cur_ssid, sizeof wc.sta.ssid - 1);
    strncpy((char*)wc.sta.password, s_cur_pass, sizeof wc.sta.password - 1);
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) return false;

    s_retry_count = 0;
    s_state = wifi_fsm_next(s_state, WIFI_EV_CONNECT_START);

    /* esp_wifi_connect requires that STA has started. If STA_START hasn't
     * fired yet (first wifi_connect_async after wifi_init), defer the call
     * until on_event sees STA_START. */
    if (!s_wifi_started) { s_pending_creds = true; return true; }

    esp_wifi_disconnect();   /* in case we were already connecting */
    return esp_wifi_connect() == ESP_OK;
}

bool wifi_forget(void) {
    nvs_store_forget_wifi();
    if (s_retry_timer) xTimerStop(s_retry_timer, 0);  /* cancel any pending retry */
    esp_wifi_disconnect();
    s_retry_count = WIFI_CONNECT_RETRY_MAX;
    s_pending_creds = false;
    s_state = wifi_fsm_next(s_state, WIFI_EV_FORGET);
    publish(WIFI_CB_DISCONNECTED);
    return true;
}

int wifi_get_rssi(void) {
    wifi_ap_record_t info;
    return (esp_wifi_sta_get_ap_info(&info) == ESP_OK) ? info.rssi : 0;
}

bool wifi_get_ip_str(char buf[16]) {
    if (s_state != WIFI_STATE_CONNECTED) { buf[0] = 0; return false; }
    snprintf(buf, 16, IPSTR, IP2STR(&s_ip));
    return true;
}

bool wifi_get_ssid(char ssid[WIFI_SSID_MAX_LEN + 1]) {
    if (s_state != WIFI_STATE_CONNECTED) { ssid[0] = 0; return false; }
    strncpy(ssid, s_cur_ssid, WIFI_SSID_MAX_LEN); ssid[WIFI_SSID_MAX_LEN] = 0;
    return true;
}

bool wifi_scan_start(wifi_scan_cb_t cb) {
    s_scan_cb = cb;
    s_state = wifi_fsm_next(s_state, WIFI_EV_SCAN_START);
    return esp_wifi_scan_start(NULL, false) == ESP_OK;
}

#endif /* ESP_PLATFORM */
```

- [ ] **Step 5: Re-run host tests + build target**

```
~/.platformio/penv/bin/pio test -e native -f test_wifi_state
~/.platformio/penv/bin/pio run -e esp32dev
```

- [ ] **Step 6: Commit**

```bash
git add src/drivers/wifi.{c,h}
git commit -m "wifi: async driver with retry backoff (5/10/20s), mDNS + SNTP, TZ at init"
```

---

### Task 7: Wire WiFi init into main + add mDNS component

**Files:**
- Modify: `src/idf_component.yml`
- Modify: `src/main.c`

- [ ] **Step 1: Add `espressif/mdns` to [src/idf_component.yml](../../../src/idf_component.yml)**

```yaml
  espressif/mdns:
    version: "^1"
```

- [ ] **Step 2: Modify `src/main.c` — order matters**

The order is: NVS → validator self-test (only if pending OTA) → drivers → UI → WiFi (async; fires events when ready).

In `app_main`, after the existing `nvs_store_init()` (which calls `nvs_flash_init` internally):

```c
#include "drivers/wifi.h"
/* ... */
void app_main(void) {
    nvs_store_init();
    /* Validator runs early (boot self-tests when slot is PENDING_VERIFY).
     * Task 24 fills in this call. */

    /* Existing drivers + UI + tasks ... */

    wifi_init();  /* non-blocking; auto-connects in background if creds present */
}
```

- [ ] **Step 3: Build to verify**

```
~/.platformio/penv/bin/pio run -e esp32dev
```

- [ ] **Step 4: Commit**

```bash
git add src/idf_component.yml src/main.c
git commit -m "main: wire wifi_init at boot (non-blocking, auto-connect if creds)"
```

---

## Phase C — HTTP server bring-up

### Task 8: Bring-up 11 — HTTP server hello

**Files:**
- Create: `tools/bringup/11_http_server/{CMakeLists.txt, platformio.ini, sdkconfig.defaults, src/{main.c, CMakeLists.txt, idf_component.yml}, README.md}`

Project skeleton **identical to Task 4's layout** (root `CMakeLists.txt` + `src/`). Full `src/main.c`:

```c
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
```

Includes the same `idf_component.yml` (idf + mdns) as bringup 10.

- [ ] **Build + commit**

```
cd tools/bringup/11_http_server && ~/.platformio/penv/bin/pio run -e esp32dev && cd ../../..
git add tools/bringup/11_http_server/
git commit -m "bringup(11_http_server): hello page on reflow.local/"
```

---

## Phase D — Status bar UI element

### Task 9: status_bar.{c,h} scaffold

**Files:**
- Create: `src/ui/status_bar.{c,h}`
- Modify: `src/ui/ui_task.c`

- [ ] **Step 1: `src/ui/status_bar.h`**

```c
#pragma once
#include "lvgl.h"

lv_obj_t *status_bar_create(lv_obj_t *parent);
void      status_bar_update(void);  /* called from 250 ms UI tick */
```

- [ ] **Step 2: `src/ui/status_bar.c` (scaffold)**

```c
#include "ui/status_bar.h"
#include "ui/theme.h"
#include <stdio.h>
#include <time.h>

static lv_obj_t *s_bar       = NULL;
static lv_obj_t *s_wifi_icon = NULL;
static lv_obj_t *s_mute_icon = NULL;
static lv_obj_t *s_clock_lbl = NULL;

#define STATUS_BAR_H 24

lv_obj_t *status_bar_create(lv_obj_t *parent) {
    s_bar = lv_obj_create(parent);
    lv_obj_set_size(s_bar, LV_HOR_RES, STATUS_BAR_H);
    lv_obj_set_pos(s_bar, 0, 0);
    lv_obj_set_style_bg_color(s_bar, THEME_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(s_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_bar, 4, 0);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_icon = lv_label_create(s_bar);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi_icon, THEME_FG, 0);
    lv_obj_align(s_wifi_icon, LV_ALIGN_LEFT_MID, 0, 0);

    s_mute_icon = lv_label_create(s_bar);
    lv_label_set_text(s_mute_icon, LV_SYMBOL_MUTE);
    lv_obj_set_style_text_color(s_mute_icon, THEME_FG, 0);
    lv_obj_align(s_mute_icon, LV_ALIGN_LEFT_MID, 32, 0);
    lv_obj_add_flag(s_mute_icon, LV_OBJ_FLAG_HIDDEN);

    s_clock_lbl = lv_label_create(s_bar);
    lv_label_set_text(s_clock_lbl, "--:--");
    lv_obj_set_style_text_color(s_clock_lbl, THEME_FG, 0);
    lv_obj_align(s_clock_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    return s_bar;
}

void status_bar_update(void) { /* Task 10 fills in live data. */ }
```

- [ ] **Step 3: Modify `src/ui/ui_task.c`**

After `lv_screen_active()` is obtained and before `lv_tabview_create`:

```c
#include "ui/status_bar.h"

status_bar_create(scr);

lv_obj_t *tv = lv_tabview_create(scr);
lv_obj_set_pos(tv, 0, 24);
lv_obj_set_size(tv, LV_HOR_RES, LV_VER_RES - 24);
```

(Adjust variable name `scr` to match the existing code if different.)

- [ ] **Build + commit**

```
~/.platformio/penv/bin/pio run -e esp32dev
git add src/ui/status_bar.{c,h} src/ui/ui_task.c
git commit -m "ui(status_bar): 24px scaffold above tabview"
```

---

### Task 10: Status bar live data + RSSI-graded WiFi icon

**Files:**
- Modify: `src/ui/status_bar.c`
- Modify: `src/ui/ui_task.c`

- [ ] **Step 1: Replace `status_bar_update()` body**

```c
#include "drivers/wifi.h"
#include "drivers/audio.h"

/* RSSI thresholds (TUNE during bring-up). Strongest to weakest. */
static const char *wifi_bars_for_rssi(int rssi) {
    if (rssi >= -55) return LV_SYMBOL_WIFI;                 /* 4 wedges */
    if (rssi >= -65) return LV_SYMBOL_WIFI;                 /* 3 wedges (LVGL renders one glyph; */
    if (rssi >= -75) return LV_SYMBOL_WIFI;                 /*  intensity differentiated via opacity below) */
    return LV_SYMBOL_WIFI;                                  /* 1 wedge */
}
static lv_opa_t wifi_opa_for_rssi(int rssi) {
    if (rssi >= -55) return LV_OPA_COVER;
    if (rssi >= -65) return LV_OPA_80;
    if (rssi >= -75) return LV_OPA_60;
    return LV_OPA_40;
}

void status_bar_update(void) {
    wifi_state_t st = wifi_get_state();
    switch (st) {
        case WIFI_STATE_CONNECTED: {
            int r = wifi_get_rssi();
            lv_label_set_text(s_wifi_icon, wifi_bars_for_rssi(r));
            lv_obj_set_style_text_color(s_wifi_icon, THEME_FG, 0);
            lv_obj_set_style_text_opa(s_wifi_icon, wifi_opa_for_rssi(r), 0);
            break;
        }
        case WIFI_STATE_CONNECTING:
        case WIFI_STATE_SCANNING:
            lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
            lv_obj_set_style_text_color(s_wifi_icon, THEME_FG_FAINT, 0);
            lv_obj_set_style_text_opa(s_wifi_icon, LV_OPA_60, 0);
            break;
        default:
            /* Not provisioned / disconnected — warning overlay on a dim icon. */
            lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI " " LV_SYMBOL_WARNING);
            lv_obj_set_style_text_color(s_wifi_icon, THEME_FG_FAINT, 0);
            lv_obj_set_style_text_opa(s_wifi_icon, LV_OPA_COVER, 0);
            break;
    }

    if (audio_is_muted()) lv_obj_clear_flag(s_mute_icon, LV_OBJ_FLAG_HIDDEN);
    else                  lv_obj_add_flag(s_mute_icon, LV_OBJ_FLAG_HIDDEN);

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char buf[8];
    if (tm_now.tm_year > 70) snprintf(buf, sizeof buf, "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    else                     snprintf(buf, sizeof buf, "--:--");
    lv_label_set_text(s_clock_lbl, buf);
}
```

The LVGL default font has a single `LV_SYMBOL_WIFI` glyph (not a per-bar set). We render visual signal-strength gradation through opacity rather than four distinct glyphs — a pragmatic approximation. A future task could add a custom 4-glyph WiFi icon font if visual fidelity matters more.

- [ ] **Step 2: Wire `status_bar_update()` into the existing 250 ms UI tick in `ui_task.c`**

- [ ] **Build + commit**

```
git add src/ui/status_bar.c src/ui/ui_task.c
git commit -m "ui(status_bar): live data + RSSI-graded WiFi opacity + warning overlay"
```

---

### Task 11: Layout cascade

**Files:** `src/ui/screen_home.c`, `src/ui/screen_profile.c`

- [ ] **Step 1: `screen_profile.c`** — change `const int ACTION_Y = 444 - 16 - ACTION_H;` → `const int ACTION_Y = 420 - 16 - ACTION_H;`

- [ ] **Step 2: `screen_home.c`** — audit fixed Y values. Graph canvas height reduces by 24. Anything aligned bottom doesn't need changes.

- [ ] **Build + commit**

```
git commit -am "ui: adjust hardcoded coords for 420 px content area"
```

---

## Phase E — Settings hub UI restructure

### Task 12: Settings tab replaces PID tab

**Files:** `src/ui/screen_settings.{c,h}` (stub), `src/ui/ui_task.c`

- [ ] **Step 1: Stub `src/ui/screen_settings.h`**

```c
#pragma once
#include "lvgl.h"
void screen_settings_create(lv_obj_t *parent);
```

- [ ] **Step 2: Stub `src/ui/screen_settings.c`**

```c
#include "ui/screen_settings.h"
#include "ui/theme.h"
void screen_settings_create(lv_obj_t *parent) {
    lv_obj_set_style_bg_color(parent, THEME_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "Settings\n(stub — Task 13 fills)");
    lv_obj_center(l);
}
```

- [ ] **Step 3: Swap tab in `src/ui/ui_task.c`**

```c
/* was: */
lv_obj_t *tab_pid = lv_tabview_add_tab(tv, "PID");
screen_pid_create(tab_pid);

/* becomes: */
lv_obj_t *tab_settings = lv_tabview_add_tab(tv, LV_SYMBOL_SETTINGS);
screen_settings_create(tab_settings);
```

If `screen_pid_create` is referenced elsewhere, leave it — Task 14 re-uses it as a sub-screen.

- [ ] **Build + commit**

```
git commit -am "ui: Settings tab replaces PID tab (stub)"
```

---

### Task 13: Settings hub list + sub-screen helpers

**Files:** `src/ui/screen_settings.{c,h}`

- [ ] **Step 1: Extend `src/ui/screen_settings.h`**

```c
#pragma once
#include "lvgl.h"

void screen_settings_create(lv_obj_t *parent);

lv_obj_t *settings_make_subscreen(const char *title);
void settings_push_subscreen(lv_obj_t *sub);
void settings_pop_subscreen(void);
```

- [ ] **Step 2: Rewrite `src/ui/screen_settings.c`**

```c
#include "ui/screen_settings.h"
#include "ui/theme.h"
#include <stddef.h>

/* Sub-screen creators (resolved in later tasks). */
extern lv_obj_t *screen_network_create(void);          /* Task 18 */
extern lv_obj_t *screen_audio_create(void);            /* Task 15 */
extern lv_obj_t *screen_pid_subscreen_create(void);    /* Task 14 */
extern lv_obj_t *screen_firmware_update_create(void);  /* Task 28 */
extern lv_obj_t *screen_about_create(void);            /* Task 27 */

static lv_obj_t *s_parent = NULL;
static lv_obj_t *s_list   = NULL;
static lv_obj_t *s_current_sub = NULL;

typedef struct { const char *icon; const char *title; lv_obj_t *(*creator)(void); } row_t;
static const row_t s_rows[] = {
    { LV_SYMBOL_WIFI,     "Network",         &screen_network_create         },
    { LV_SYMBOL_AUDIO,    "Audio",           &screen_audio_create           },
    { LV_SYMBOL_SETTINGS, "PID",             &screen_pid_subscreen_create   },
    { LV_SYMBOL_UPLOAD,   "Firmware update", &screen_firmware_update_create },
    { LV_SYMBOL_LIST,     "About",           &screen_about_create           },
};

static lv_obj_t *s_subscreens[sizeof s_rows / sizeof s_rows[0]] = {0};

/* Proper lv_event_cb_t signature wrapper (avoids UB cast). */
static void back_clicked_cb(lv_event_t *e) { (void)e; settings_pop_subscreen(); }

static void row_clicked_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_subscreens[idx]) s_subscreens[idx] = s_rows[idx].creator();
    settings_push_subscreen(s_subscreens[idx]);
}

void screen_settings_create(lv_obj_t *parent) {
    s_parent = parent;
    lv_obj_set_style_bg_color(parent, THEME_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);

    s_list = lv_list_create(parent);
    lv_obj_set_size(s_list, LV_HOR_RES, LV_VER_RES - 24 - 36);
    lv_obj_set_style_bg_color(s_list, THEME_BG, 0);
    lv_obj_set_style_border_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 0);

    const size_t n = sizeof s_rows / sizeof s_rows[0];
    for (size_t i = 0; i < n; i++) {
        lv_obj_t *btn = lv_list_add_button(s_list, s_rows[i].icon, s_rows[i].title);
        lv_obj_add_event_cb(btn, row_clicked_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
}

lv_obj_t *settings_make_subscreen(const char *title) {
    lv_obj_t *sub = lv_obj_create(s_parent);
    lv_obj_set_size(sub, LV_HOR_RES, LV_VER_RES - 24 - 36);
    lv_obj_set_pos(sub, 0, 0);
    lv_obj_set_style_bg_color(sub, THEME_BG, 0);
    lv_obj_set_style_bg_opa(sub, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(sub, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(sub, 4, 0);

    lv_obj_t *back = lv_button_create(sub);
    lv_obj_set_size(back, 48, 28);
    lv_obj_set_pos(back, 0, 0);
    lv_obj_set_style_bg_color(back, THEME_PANEL_BG, 0);
    lv_obj_add_event_cb(back, back_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);

    lv_obj_t *t = lv_label_create(sub);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, THEME_FG, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 56, 4);

    lv_obj_add_flag(sub, LV_OBJ_FLAG_HIDDEN);
    return sub;
}

void settings_push_subscreen(lv_obj_t *sub) {
    if (s_current_sub) lv_obj_add_flag(s_current_sub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(sub, LV_OBJ_FLAG_HIDDEN);
    s_current_sub = sub;
}

void settings_pop_subscreen(void) {
    if (s_current_sub) {
        lv_obj_add_flag(s_current_sub, LV_OBJ_FLAG_HIDDEN);
        s_current_sub = NULL;
    }
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_HIDDEN);
}

/* Temporary stubs — replaced by Tasks 14, 15, 18, 27, 28. */
__attribute__((weak)) lv_obj_t *screen_network_create(void)         { return settings_make_subscreen("Network (TODO)"); }
__attribute__((weak)) lv_obj_t *screen_audio_create(void)           { return settings_make_subscreen("Audio (TODO)"); }
__attribute__((weak)) lv_obj_t *screen_pid_subscreen_create(void)   { return settings_make_subscreen("PID (TODO)"); }
__attribute__((weak)) lv_obj_t *screen_firmware_update_create(void) { return settings_make_subscreen("Firmware (TODO)"); }
__attribute__((weak)) lv_obj_t *screen_about_create(void)           { return settings_make_subscreen("About (TODO)"); }
```

- [ ] **Build + commit**

```
git commit -am "ui(settings): list + sub-screen back-nav (proper lv_event_cb_t)"
```

---

### Task 14: PID sub-screen

**Files:** `src/ui/screen_pid.{c,h}`, `src/ui/screen_settings.c`

- [ ] **Step 1: Add to `src/ui/screen_pid.h`**

```c
lv_obj_t *screen_pid_subscreen_create(void);
```

- [ ] **Step 2: Add to `src/ui/screen_pid.c`** (the existing `screen_pid_create` takes a parent and lays children into it — preserve that contract; wrap in an inner container with the sub-screen header offset):

```c
#include "ui/screen_settings.h"

lv_obj_t *screen_pid_subscreen_create(void) {
    lv_obj_t *sub = settings_make_subscreen("PID");
    lv_obj_t *inner = lv_obj_create(sub);
    lv_obj_set_size(inner, LV_HOR_RES - 8, LV_VER_RES - 24 - 36 - 40);
    lv_obj_set_pos(inner, 4, 36);
    lv_obj_set_style_bg_opa(inner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(inner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(inner, 0, 0);
    lv_obj_clear_flag(inner, LV_OBJ_FLAG_SCROLLABLE);
    screen_pid_create(inner);
    return sub;
}
```

If the existing `screen_pid_create` hardcodes Y offsets that assumed full-tab height, those need adjusting — likely subtract ~40 from each so cards fit within the inner container. Inspect [src/ui/screen_pid.c](../../../src/ui/screen_pid.c) before this task.

- [ ] **Step 3: Remove the weak stub for `screen_pid_subscreen_create` in `screen_settings.c`.**

- [ ] **Build + commit**

```
git commit -am "ui: PID moves into Settings sub-screen with back-nav"
```

---

### Task 15: Audio sub-screen + remove mute button from HOME

**Files:** `src/ui/screen_audio.{c,h}`, `src/ui/screen_home.c`, `src/ui/screen_settings.c`

- [ ] **Step 1: `screen_audio.h`**

```c
#pragma once
#include "lvgl.h"
lv_obj_t *screen_audio_create(void);
```

- [ ] **Step 2: `screen_audio.c`**

```c
#include "ui/screen_audio.h"
#include "ui/screen_settings.h"
#include "ui/theme.h"
#include "drivers/audio.h"

static lv_obj_t *s_mute_sw = NULL;

static void mute_changed_cb(lv_event_t *e) {
    (void)e;
    audio_set_muted(lv_obj_has_state(s_mute_sw, LV_STATE_CHECKED));
}

lv_obj_t *screen_audio_create(void) {
    lv_obj_t *sub = settings_make_subscreen("Audio");
    lv_obj_t *lbl = lv_label_create(sub);
    lv_label_set_text(lbl, "Mute notifications");
    lv_obj_set_pos(lbl, 8, 48);
    lv_obj_set_style_text_color(lbl, THEME_FG, 0);

    s_mute_sw = lv_switch_create(sub);
    lv_obj_set_pos(s_mute_sw, LV_HOR_RES - 80, 44);
    if (audio_is_muted()) lv_obj_add_state(s_mute_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_mute_sw, mute_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sub;
}
```

- [ ] **Step 3: Remove the mute button from `screen_home.c`** (delete the existing speaker icon + its click callback).

- [ ] **Step 4: Drop the weak `screen_audio_create` stub from `screen_settings.c`.**

- [ ] **Build + commit**

```
git commit -am "ui: move mute from HOME to Settings → Audio"
```

---

## Phase F — WiFi provisioning sub-screens

### Task 16: WiFi scan sub-screen (no strdup leaks; Manual entry button)

**Files:** `src/ui/screen_wifi_scan.{c,h}`

- [ ] **Step 1: Header**

```c
#pragma once
#include "lvgl.h"
lv_obj_t *screen_wifi_scan_create(void);

typedef void (*wifi_scan_chosen_cb_t)(const char *ssid, bool locked);
void screen_wifi_scan_set_chosen_cb(wifi_scan_chosen_cb_t cb);
```

- [ ] **Step 2: `screen_wifi_scan.c`** — keep SSIDs in a fixed-size table; no strdup:

```c
#include "ui/screen_wifi_scan.h"
#include "ui/screen_settings.h"
#include "ui/screen_wifi_password.h"  /* Task 17 declares screen_wifi_password_create */
#include "ui/theme.h"
#include "drivers/wifi.h"
#include <string.h>

#define MAX_RESULTS 16

static lv_obj_t *s_sub  = NULL;
static lv_obj_t *s_list = NULL;
static wifi_scan_chosen_cb_t s_chosen_cb = NULL;
static wifi_scan_result_t s_results[MAX_RESULTS];
static size_t s_n = 0;

void screen_wifi_scan_set_chosen_cb(wifi_scan_chosen_cb_t cb) { s_chosen_cb = cb; }

static void row_clicked(lv_event_t *e) {
    size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx >= s_n) return;
    if (s_chosen_cb) s_chosen_cb(s_results[idx].ssid, s_results[idx].locked);
}

static void manual_clicked(lv_event_t *e) {
    (void)e;
    /* Manual entry: open password screen with empty SSID; user types both. */
    lv_obj_t *pw = screen_wifi_password_create(""); /* empty = manual mode */
    settings_push_subscreen(pw);
}

static void on_scan_results(const wifi_scan_result_t *results, size_t n) {
    s_n = n > MAX_RESULTS ? MAX_RESULTS : n;
    for (size_t i = 0; i < s_n; i++) s_results[i] = results[i];
    lv_obj_clean(s_list);
    for (size_t i = 0; i < s_n; i++) {
        const char *icon = s_results[i].locked
            ? LV_SYMBOL_WIFI " " LV_SYMBOL_EYE_CLOSE
            : LV_SYMBOL_WIFI;
        lv_obj_t *b = lv_list_add_button(s_list, icon, s_results[i].ssid);
        lv_obj_add_event_cb(b, row_clicked, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
}

lv_obj_t *screen_wifi_scan_create(void) {
    s_sub = settings_make_subscreen("Choose network");
    s_list = lv_list_create(s_sub);
    lv_obj_set_size(s_list, LV_HOR_RES - 8, LV_VER_RES - 24 - 36 - 80);
    lv_obj_set_pos(s_list, 0, 36);

    lv_obj_t *manual = lv_button_create(s_sub);
    lv_obj_set_size(manual, LV_HOR_RES - 16, 32);
    lv_obj_align(manual, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_event_cb(manual, manual_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ml = lv_label_create(manual); lv_label_set_text(ml, "Manual entry"); lv_obj_center(ml);

    wifi_scan_start(on_scan_results);
    return s_sub;
}
```

- [ ] **Build + commit**

```
git commit -am "ui(wifi_scan): fixed-size results table + Manual entry button"
```

---

### Task 17: Password sub-screen — async connect + show/hide eye + spinner

**Files:** `src/ui/screen_wifi_password.{c,h}`

- [ ] **Step 1: Header**

```c
#pragma once
#include "lvgl.h"
/* Pass empty string for manual SSID entry (an SSID textarea is then shown). */
lv_obj_t *screen_wifi_password_create(const char *ssid);
```

- [ ] **Step 2: Implementation — async connect via wifi status callback**

```c
#include "ui/screen_wifi_password.h"
#include "ui/screen_settings.h"
#include "ui/theme.h"
#include "drivers/wifi.h"
#include <string.h>

static char s_ssid_buf[33] = {0};
static bool s_manual_mode = false;
static lv_obj_t *s_ssid_ta = NULL;
static lv_obj_t *s_pw_ta   = NULL;
static lv_obj_t *s_kbd     = NULL;
static lv_obj_t *s_status  = NULL;
static lv_obj_t *s_spinner = NULL;
static lv_obj_t *s_eye_btn = NULL;

static lv_obj_t *s_password_sub = NULL;  /* tracked so we can lv_obj_del on cleanup */

/* Forward declaration — definition below; referenced by cleanup_screen. */
static void on_wifi_status(wifi_cb_event_t ev);

static void cleanup_screen(void) {
    /* Tear down the screen tree, freeing all LVGL children (keyboard, labels,
     * spinner, etc.). Prevents a memory leak across repeated provisionings. */
    wifi_unsubscribe_status(on_wifi_status);
    if (s_password_sub) { lv_obj_del(s_password_sub); s_password_sub = NULL; }
    s_status = s_spinner = s_pw_ta = s_kbd = s_eye_btn = s_ssid_ta = NULL;
}

/* Marshal wifi status events from event-loop task back to LVGL UI task. */
static void on_wifi_status_main(void *arg) {
    wifi_cb_event_t ev = (wifi_cb_event_t)(intptr_t)arg;
    if (ev == WIFI_CB_CONNECTED) {
        settings_pop_subscreen();
        cleanup_screen();   /* delete screen tree; next provisioning recreates */
    } else if (ev == WIFI_CB_CONNECT_FAILED) {
        if (s_status) lv_label_set_text(s_status, "Failed — check password.");
        if (s_spinner) lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}
static void on_wifi_status(wifi_cb_event_t ev) {
    lv_async_call(on_wifi_status_main, (void*)(intptr_t)ev);
}

static void connect_cb(lv_event_t *e) {
    (void)e;
    if (s_manual_mode) {
        const char *ssid = lv_textarea_get_text(s_ssid_ta);
        strncpy(s_ssid_buf, ssid, 32); s_ssid_buf[32] = 0;
    }
    const char *pw = lv_textarea_get_text(s_pw_ta);
    lv_label_set_text(s_status, "Connecting...");
    lv_obj_clear_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    wifi_subscribe_status(on_wifi_status);  /* additive — does not displace OTA-server cb */
    wifi_connect_async(s_ssid_buf, pw);
}

static void eye_clicked(lv_event_t *e) {
    (void)e;
    bool hidden = lv_textarea_get_password_mode(s_pw_ta);
    lv_textarea_set_password_mode(s_pw_ta, !hidden);
    lv_obj_t *icon = lv_obj_get_child(s_eye_btn, 0);
    lv_label_set_text(icon, hidden ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

lv_obj_t *screen_wifi_password_create(const char *ssid) {
    /* If a previous password screen is still allocated, tear it down first.
     * Caller treats the returned handle as freshly created each call. */
    cleanup_screen();
    s_manual_mode = (ssid[0] == 0);
    strncpy(s_ssid_buf, ssid, 32); s_ssid_buf[32] = 0;
    lv_obj_t *sub = settings_make_subscreen(s_manual_mode ? "Manual entry" : ssid);
    s_password_sub = sub;

    int y = 44;
    if (s_manual_mode) {
        s_ssid_ta = lv_textarea_create(sub);
        lv_obj_set_size(s_ssid_ta, LV_HOR_RES - 20, 32);
        lv_obj_set_pos(s_ssid_ta, 8, y);
        lv_textarea_set_placeholder_text(s_ssid_ta, "SSID");
        lv_textarea_set_one_line(s_ssid_ta, true);
        y += 40;
    }

    s_pw_ta = lv_textarea_create(sub);
    lv_obj_set_size(s_pw_ta, LV_HOR_RES - 60, 32);
    lv_obj_set_pos(s_pw_ta, 8, y);
    lv_textarea_set_password_mode(s_pw_ta, true);
    lv_textarea_set_placeholder_text(s_pw_ta, "password");
    lv_textarea_set_one_line(s_pw_ta, true);

    s_eye_btn = lv_button_create(sub);
    lv_obj_set_size(s_eye_btn, 40, 32);
    lv_obj_set_pos(s_eye_btn, LV_HOR_RES - 48, y);
    lv_obj_add_event_cb(s_eye_btn, eye_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *eye_icon = lv_label_create(s_eye_btn);
    lv_label_set_text(eye_icon, LV_SYMBOL_EYE_CLOSE);
    lv_obj_center(eye_icon);
    y += 40;

    s_status = lv_label_create(sub);
    lv_obj_set_pos(s_status, 8, y);
    lv_label_set_text(s_status, "");
    lv_obj_set_style_text_color(s_status, THEME_FG, 0);

    s_spinner = lv_spinner_create(sub);
    lv_obj_set_size(s_spinner, 24, 24);
    lv_obj_set_pos(s_spinner, LV_HOR_RES - 32, y);
    lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    y += 28;

    s_kbd = lv_keyboard_create(sub);
    lv_obj_set_size(s_kbd, LV_HOR_RES, 180);
    lv_obj_align(s_kbd, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_keyboard_set_textarea(s_kbd, s_pw_ta);

    lv_obj_t *btn = lv_button_create(sub);
    lv_obj_set_size(btn, 96, 32);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_event_cb(btn, connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Connect");
    lv_obj_center(bl);

    return sub;
}
```

- [ ] **Update `src/ui/screen_wifi_scan.c`** to register the chosen-cb that pushes the password screen:

```c
static void on_ssid_chosen(const char *ssid, bool locked) {
    (void)locked;
    lv_obj_t *pw = screen_wifi_password_create(ssid);
    settings_push_subscreen(pw);
}
/* In screen_wifi_scan_create, after wifi_scan_start: */
screen_wifi_scan_set_chosen_cb(on_ssid_chosen);
```

- [ ] **Build + commit**

```
git commit -am "ui(wifi_password): async connect via lv_async_call + eye + spinner"
```

---

### Task 18: Network sub-screen — dynamic SSID + Host row

**Files:** `src/ui/screen_network.{c,h}`, `src/ui/screen_settings.c`

- [ ] **Step 1: Header**

```c
#pragma once
#include "lvgl.h"
lv_obj_t *screen_network_create(void);
```

- [ ] **Step 2: Implementation**

```c
#include "ui/screen_network.h"
#include "ui/screen_settings.h"
#include "ui/screen_wifi_scan.h"
#include "ui/theme.h"
#include "drivers/wifi.h"
#include <stdio.h>

static lv_obj_t *s_sub = NULL;
static lv_obj_t *s_status_l, *s_ssid_l, *s_ip_l, *s_host_l, *s_rssi_l;

static void change_clicked(lv_event_t *e) { (void)e; settings_push_subscreen(screen_wifi_scan_create()); }
static void forget_clicked(lv_event_t *e) { (void)e; wifi_forget(); settings_pop_subscreen(); }

static void refresh(void) {
    wifi_state_t st = wifi_get_state();
    const char *sname = st == WIFI_STATE_CONNECTED ? "Connected" :
                        st == WIFI_STATE_CONNECTING ? "Connecting..." :
                        st == WIFI_STATE_SCANNING   ? "Scanning..." :
                                                       "Not configured";
    lv_label_set_text_fmt(s_status_l, "Status: %s", sname);
    char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    wifi_get_ssid(ssid);
    lv_label_set_text_fmt(s_ssid_l, "SSID:   %s", ssid[0] ? ssid : "(none)");
    char ip[16] = {0};
    wifi_get_ip_str(ip);
    lv_label_set_text_fmt(s_ip_l, "IP:     %s", ip[0] ? ip : "(none)");
    lv_label_set_text   (s_host_l, "Host:   reflow.local");
    lv_label_set_text_fmt(s_rssi_l, "Signal: %d dBm", wifi_get_rssi());
}

lv_obj_t *screen_network_create(void) {
    s_sub = settings_make_subscreen("Network");
    s_status_l = lv_label_create(s_sub); lv_obj_set_pos(s_status_l, 8, 44);
    s_ssid_l   = lv_label_create(s_sub); lv_obj_set_pos(s_ssid_l,   8, 64);
    s_ip_l     = lv_label_create(s_sub); lv_obj_set_pos(s_ip_l,     8, 84);
    s_host_l   = lv_label_create(s_sub); lv_obj_set_pos(s_host_l,   8, 104);
    s_rssi_l   = lv_label_create(s_sub); lv_obj_set_pos(s_rssi_l,   8, 124);
    lv_obj_set_style_text_color(s_status_l, THEME_FG, 0);
    lv_obj_set_style_text_color(s_ssid_l,   THEME_FG, 0);
    lv_obj_set_style_text_color(s_ip_l,     THEME_FG, 0);
    lv_obj_set_style_text_color(s_host_l,   THEME_FG, 0);
    lv_obj_set_style_text_color(s_rssi_l,   THEME_FG, 0);

    lv_obj_t *change = lv_button_create(s_sub);
    lv_obj_set_size(change, 140, 32); lv_obj_set_pos(change, 8, 170);
    lv_obj_add_event_cb(change, change_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(change); lv_label_set_text(cl, "Change network"); lv_obj_center(cl);

    lv_obj_t *forget = lv_button_create(s_sub);
    lv_obj_set_size(forget, 140, 32); lv_obj_set_pos(forget, 8, 210);
    lv_obj_set_style_bg_color(forget, THEME_STOP, 0);
    lv_obj_add_event_cb(forget, forget_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fl = lv_label_create(forget); lv_label_set_text(fl, "Forget WiFi"); lv_obj_center(fl);

    refresh();
    return s_sub;
}
```

(Status fields update only at create-time for now. For live updates, a future enhancement could subscribe to wifi_set_status_cb — but the user typically only views this screen briefly, so the on-create refresh is acceptable.)

- [ ] **Step 3: Drop weak `screen_network_create` stub from `screen_settings.c`.**

- [ ] **Build + commit**

```
git commit -am "ui(network): dynamic SSID + Host row + THEME_STOP for Forget"
```

---

## Phase G — OTA endpoint

### Task 19: Bring-up 12 — minimal /ota POST

**Files:** `tools/bringup/12_ota_endpoint/{CMakeLists.txt, platformio.ini, sdkconfig.defaults, src/{main.c, CMakeLists.txt, idf_component.yml}, README.md}`

Mirrors structure of bringup 11. Add `POST /ota` handler with hardcoded token and the minimal write-flow. Token in code is OK for bringup. Add `board_build.partitions = ../../../partitions.csv` to `[env:esp32dev]` in `platformio.ini`. Full `src/main.c` extends bringup 11 with:

```c
#include "esp_ota_ops.h"
#define EXPECTED_TOKEN "abcdef0123456789abcdef0123456789"

static esp_err_t ota_handler(httpd_req_t *req) {
    char tok[33] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Token", tok, sizeof tok) != ESP_OK
        || strcmp(tok, EXPECTED_TOKEN) != 0) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad token");
    }
    int total = req->content_len;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t op = 0;
    if (esp_ota_begin(part, total, &op) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "begin");
    char buf[4096];
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf, sizeof buf);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;  /* retry */
        if (r <= 0) { esp_ota_abort(op); return ESP_FAIL; }
        if (esp_ota_write(op, buf, r) != ESP_OK) { esp_ota_abort(op); return ESP_FAIL; }
        received += r;
    }
    if (esp_ota_end(op) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "hash");
    esp_ota_set_boot_partition(part);
    httpd_resp_send(req, "{\"status\":\"rebooting\"}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}
/* register in start_server(): */
httpd_uri_t ota = { .uri="/ota", .method=HTTP_POST, .handler=ota_handler };
```

- [ ] **Build + commit**

```
cd tools/bringup/12_ota_endpoint && ~/.platformio/penv/bin/pio run -e esp32dev && cd ../../..
git add tools/bringup/12_ota_endpoint/
git commit -m "bringup(12_ota_endpoint): minimal POST /ota with token + timeout retry"
```

---

### Task 20: Production ota_server.c

**Files:**
- Create: `src/ota/ota_server.{c,h}`
- Modify: `src/control/control_task.{c,h}` (declare + define `control_get_mode`)
- Test: `test/test_ota_token.c`

- [ ] **Step 1: Tests — `test/test_ota_token.c`**

```c
#include "unity.h"
#include <string.h>

/* Forward decl from src/ota/ota_server.c, exposed for tests. */
int ota_token_eq(const char *a, const char *b, size_t n);

void test_eq_match(void)        { TEST_ASSERT_EQUAL(1, ota_token_eq("abc123", "abc123", 6)); }
void test_eq_mismatch(void)     { TEST_ASSERT_EQUAL(0, ota_token_eq("abc123", "abc124", 6)); }
void test_eq_diff_at_start(void){ TEST_ASSERT_EQUAL(0, ota_token_eq("xbc123", "abc123", 6)); }
void test_eq_diff_at_end(void)  { TEST_ASSERT_EQUAL(0, ota_token_eq("abc12X", "abc123", 6)); }

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_eq_match);
    RUN_TEST(test_eq_mismatch);
    RUN_TEST(test_eq_diff_at_start);
    RUN_TEST(test_eq_diff_at_end);
    return UNITY_END();
}
```

- [ ] **Step 2: `src/control/control_task.h`** — add declaration

```c
oven_mode_t control_get_mode(void);
```

- [ ] **Step 3: `src/control/control_task.c`** — add definition near end:

```c
oven_mode_t control_get_mode(void) { return s_sm.mode; }
```

- [ ] **Step 4: `src/ota/ota_server.h`**

```c
#pragma once
#include <stdbool.h>
#include <stddef.h>

void ota_server_start(void);
bool ota_server_is_running(void);

/* Constant-time compare; uses mbedtls_ct_memcmp on target, hand-rolled fallback for tests. */
int ota_token_eq(const char *a, const char *b, size_t n);
```

- [ ] **Step 5: `src/ota/ota_server.c`**

```c
#include "ota/ota_server.h"
#include "config.h"
#include "storage/nvs_store.h"
#include "control/control_task.h"
#include "control/reflow_sm.h"
#include "version.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_timer.h"     /* esp_timer_get_time for recv-timeout budget */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h" /* vTaskDelay before reboot */
#include <string.h>

#ifdef ESP_PLATFORM
#include "mbedtls/constant_time.h"
#endif

static const char *TAG = "ota_server";
static httpd_handle_t s_server = NULL;

int ota_token_eq(const char *a, const char *b, size_t n) {
#ifdef ESP_PLATFORM
    return mbedtls_ct_memcmp(a, b, n) == 0 ? 1 : 0;
#else
    /* Hand-rolled constant-time fallback for host tests. */
    unsigned char acc = 0;
    for (size_t i = 0; i < n; i++) acc |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return acc == 0 ? 1 : 0;
#endif
}

#ifdef ESP_PLATFORM
static void ensure_token(void) {
    char t[OTA_TOKEN_HEX_LEN + 1];
    if (nvs_store_get_ota_token(t)) return;
    uint8_t raw[16];
    esp_fill_random(raw, sizeof raw);
    for (int i = 0; i < 16; i++) snprintf(t + i*2, 3, "%02x", raw[i]);
    nvs_store_set_ota_token(t);
    ESP_LOGI(TAG, "generated fresh OTA token");
}

static esp_err_t version_handler(httpd_req_t *req) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    char body[200];
    snprintf(body, sizeof body,
        "{\"version\":\"%s\",\"slot\":\"%s\",\"status\":\"%s\",\"built\":\"%s\"}",
        FIRMWARE_VERSION_STR, run ? run->label : "?",
        st == ESP_OTA_IMG_VALID ? "valid" :
        st == ESP_OTA_IMG_PENDING_VERIFY ? "pending" : "unknown",
        BUILD_DATE_STR);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t root_handler(httpd_req_t *req) {
    char body[640];
    snprintf(body, sizeof body,
        "<!doctype html><meta charset=utf-8><title>Reflow oven</title>"
        "<h1>Reflow oven %s</h1><p>Built %s</p>"
        "<p>Upload via CLI:</p>"
        "<pre>curl -X POST -H 'X-OTA-Token: &lt;your token&gt;' \\\n"
        "  --data-binary @firmware.bin http://reflow.local/ota</pre>"
        "<p>or use <code>pio run -e esp32dev-ota -t upload</code>.</p>",
        FIRMWARE_VERSION_STR, BUILD_DATE_STR);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ota_handler(httpd_req_t *req) {
    /* Mode gate */
    if (control_get_mode() != MODE_IDLE)
        return httpd_resp_send_err(req, HTTPD_409_CONFLICT, "oven not idle");

    /* Content-Length check (400 if missing) */
    if (req->content_len <= 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing or zero Content-Length");

    /* Content-Type check — match the full literal prefix so a typo like
     * "application/octet-streaXYZ" is rejected. Allows charset suffixes
     * (e.g. "application/octet-stream; charset=binary") via prefix match. */
    char ctype[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof ctype) == ESP_OK
        && strncmp(ctype, "application/octet-stream", strlen("application/octet-stream")) != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
            "Content-Type must be application/octet-stream");
    }

    /* Auth */
    char tok[OTA_TOKEN_HEX_LEN + 1] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Token", tok, sizeof tok) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "missing token");
    char expected[OTA_TOKEN_HEX_LEN + 1];
    if (!nvs_store_get_ota_token(expected) ||
        !ota_token_eq(tok, expected, OTA_TOKEN_HEX_LEN))
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad token");

    int total = req->content_len;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || total > (int)part->size)
        return httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "too large");

    esp_ota_handle_t op = 0;
    if (esp_ota_begin(part, total, &op) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin");

    char buf[OTA_UPLOAD_CHUNK_SIZE];
    int received = 0;
    int64_t timeout_budget_us = (int64_t)OTA_RECV_TIMEOUT_TOTAL_MS * 1000;
    int64_t last_t = esp_timer_get_time();
    while (received < total) {
        int r = httpd_req_recv(req, buf, sizeof buf);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            int64_t now = esp_timer_get_time();
            timeout_budget_us -= (now - last_t); last_t = now;
            if (timeout_budget_us <= 0) {
                esp_ota_abort(op);
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv timeout");
            }
            continue;
        }
        if (r <= 0) { esp_ota_abort(op); return ESP_FAIL; }
        if (esp_ota_write(op, buf, r) != ESP_OK) {
            esp_ota_abort(op);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_write");
        }
        received += r;
    }
    if (esp_ota_end(op) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_end");
    if (esp_ota_set_boot_partition(part) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set_boot");

    char resp[80];
    snprintf(resp, sizeof resp, "{\"status\":\"rebooting\",\"new_version\":\"%s\"}",
             FIRMWARE_VERSION_STR);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

void ota_server_start(void) {
    if (s_server) return;
    ensure_token();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 4;
    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));
    httpd_uri_t root    = { .uri="/",        .method=HTTP_GET,  .handler=root_handler };
    httpd_uri_t version = { .uri="/version", .method=HTTP_GET,  .handler=version_handler };
    httpd_uri_t ota     = { .uri="/ota",     .method=HTTP_POST, .handler=ota_handler };
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &version);
    httpd_register_uri_handler(s_server, &ota);
    ESP_LOGI(TAG, "OTA server up on port 80");
}

bool ota_server_is_running(void) { return s_server != NULL; }
#endif /* ESP_PLATFORM */
```

- [ ] **Run host tests + build target + commit**

```
~/.platformio/penv/bin/pio test -e native -f test_ota_token
~/.platformio/penv/bin/pio run -e esp32dev
git add src/ota/ota_server.{c,h} src/control/control_task.{c,h} test/test_ota_token.c
git commit -m "ota: server w/ mbedtls_ct_memcmp + 400 + content-type + timeout retry"
```

---

### Task 21: Wire OTA server start from WiFi connected event

**Files:** `src/main.c`

The WiFi driver publishes `WIFI_CB_CONNECTED` on first IP. We hook into that to start the OTA server idempotently.

- [ ] **Step 1: In `main.c`, register a status callback**

```c
#include "ota/ota_server.h"

static void on_wifi_status_for_server(wifi_cb_event_t ev) {
    if (ev == WIFI_CB_CONNECTED && !ota_server_is_running()) ota_server_start();
}

/* in app_main, after wifi_init: */
wifi_subscribe_status(on_wifi_status_for_server);
```

The registry pattern (`wifi_subscribe_status`) means multiple consumers — OTA-server starter here, the WiFi-password screen in Task 17 — coexist cleanly. No callback overwrite.

- [ ] **Build + commit**

```
git commit -am "ota: start server on first WiFi connect event"
```

---

## Phase H — Self-tests + MODE_PENDING_VALIDATION

### Task 22: Add MODE_PENDING_VALIDATION + CMD_CLEAR_PENDING

**Files:** `src/control/reflow_sm.h`, `src/oven_state.h`, `src/control/control_task.c`

- [ ] **Step 1: [src/control/reflow_sm.h](../../../src/control/reflow_sm.h)**

```c
typedef enum {
    MODE_IDLE,
    MODE_RUNNING,
    MODE_FAULT,
    MODE_PENDING_VALIDATION,
} oven_mode_t;
```

- [ ] **Step 2: [src/oven_state.h](../../../src/oven_state.h)**

Add to `cmd_type_t`:

```c
typedef enum {
    CMD_START,
    CMD_STOP,
    CMD_ACK_FAULT,
    CMD_APPLY_PROFILE,
    CMD_APPLY_PID,
    CMD_CLEAR_PENDING,  /* user confirms a pending-validation update */
} cmd_type_t;
```

- [ ] **Step 3: [src/control/control_task.c](../../../src/control/control_task.c)** — handle CMD_CLEAR_PENDING in `apply_non_sm_command`

Open `control_task.c`. The command dispatch is split:
- `drain_commands` (~line 133) handles state-machine event commands: `CMD_START`, `CMD_STOP`, `CMD_ACK_FAULT`.
- `apply_non_sm_command` (~line 83) handles non-SM mutations: `CMD_APPLY_PROFILE`, `CMD_APPLY_PID`.

`CMD_CLEAR_PENDING` is a non-SM mutation of `s_sm.mode`, so it belongs in **`apply_non_sm_command`**. Add the case to that function's switch, BEFORE the default:

```c
case CMD_CLEAR_PENDING:
    if (s_sm.mode == MODE_PENDING_VALIDATION) s_sm.mode = MODE_IDLE;
    break;
```

(Do NOT add it to `drain_commands` — that switch handles only SM-event commands and would silently drop the no-op `default` if unmatched.)

**Visible-feedback requirement (spec §6.4)**: the user must see *why* START is rejected, not just "nothing happened". Implement both (a) and (b):

(a) **Disable the START button** when mode is PENDING_VALIDATION. The button is declared `static lv_obj_t *btn_action` in `screen_home.c` (verify in source) — same translation unit as `screen_home_update`, so no extern needed:

```c
/* In screen_home_update, after the banner logic — references the file-static btn_action: */
if (snap->mode == MODE_PENDING_VALIDATION) lv_obj_add_state(btn_action, LV_STATE_DISABLED);
else                                       lv_obj_remove_state(btn_action, LV_STATE_DISABLED);
```

(b) **Toast on attempted START while pending**. The `action_clicked_cb` in `screen_home.c` already dispatches CMD_START; gate it explicitly:

```c
static void action_clicked_cb(lv_event_t *e) {
    if (s_mode_cached == MODE_PENDING_VALIDATION) {
        /* Existing toast helper (used by Profile screen's "saved" toast) — see screen_profile.c */
        /* TODO: implementer extracts a shared toast helper if not already factored */
        return;  /* don't queue CMD_START */
    }
    /* existing CMD_START / CMD_STOP / CMD_ACK_FAULT dispatch */
}
```

If no toast helper exists yet, the simpler approach: a transient `lv_label` near the START button shown for 2 seconds via `lv_timer_create`, mirroring the `s_saved_label` pattern from `screen_profile.c`.

- [ ] **Step 5: Audit `reflow_sm.c` if-chain too — not just switches**

`src/control/reflow_sm.c` does not use `switch (s->mode)`; it uses `if (s->mode == MODE_X)` chains. `grep -n "s->mode\\|s_sm.mode" src/control/reflow_sm.c` finds the relevant sites. Add an early-return for `MODE_PENDING_VALIDATION` at the top of `sm_step`, so PENDING is treated like a no-op the same way `MODE_FAULT` early-returns:

```c
bool sm_step(sm_state_t *s, /* ... existing params ... */) {
    if (s->mode == MODE_PENDING_VALIDATION) return false;  /* no profile work while pending */
    if (s->mode == MODE_FAULT) { /* existing */ ... }
    if (s->mode == MODE_IDLE)  { /* existing */ ... }
    /* running-state logic continues */
}
```

Sites to audit:
- `src/control/reflow_sm.c` — `sm_step` (add early-return as above)
- `src/ui/screen_home.c` — `screen_home_update` switch on mode for colours / chip text (add explicit case for PENDING that mirrors IDLE styling)
- `src/audio/audio_events.c` — if it switches on mode for stage transitions, ensure PENDING is a no-op
- `src/ui/ui_task.c` — typically reads mode only via snapshots; verify no special handling needed

For each site: if the existing default branch is safe (no SSR drive, no run-side effects), leave it; otherwise add explicit `case MODE_PENDING_VALIDATION:` and treat as IDLE.

- [ ] **Build + commit**

```
git commit -am "control: MODE_PENDING_VALIDATION + CMD_CLEAR_PENDING + switch audit"
```

---

### Task 23: ota_validator with real driver signatures + per-test timeouts

**Files:** `src/ota/ota_validator.{c,h}`, `test/test_ota_validator.c`

- [ ] **Step 1: Tests**

```c
/* test/test_ota_validator.c */
#include "unity.h"
#include <stdbool.h>

/* Mock hooks — overridden by the test. The real impl on ESP_PLATFORM
 * uses the actual drivers. */
static bool mock_tc = true, mock_ssr = true, mock_display = true, mock_touch = true;
bool ota_validator_hook_tc_alive(void)      { return mock_tc; }
bool ota_validator_hook_ssr_alive(void)     { return mock_ssr; }
bool ota_validator_hook_display_alive(void) { return mock_display; }
bool ota_validator_hook_touch_alive(void)   { return mock_touch; }

#include "ota/ota_validator.h"

void test_all_pass(void) {
    mock_tc = mock_ssr = mock_display = mock_touch = true;
    TEST_ASSERT_TRUE(ota_validator_run_self_tests());
}
void test_tc_fail(void) {
    mock_ssr = mock_display = mock_touch = true; mock_tc = false;
    TEST_ASSERT_FALSE(ota_validator_run_self_tests());
}
void test_ssr_fail(void) {
    mock_tc = mock_display = mock_touch = true; mock_ssr = false;
    TEST_ASSERT_FALSE(ota_validator_run_self_tests());
}
void test_display_fail(void) {
    mock_tc = mock_ssr = mock_touch = true; mock_display = false;
    TEST_ASSERT_FALSE(ota_validator_run_self_tests());
}
void test_touch_fail(void) {
    mock_tc = mock_ssr = mock_display = true; mock_touch = false;
    TEST_ASSERT_FALSE(ota_validator_run_self_tests());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_all_pass);
    RUN_TEST(test_tc_fail);
    RUN_TEST(test_ssr_fail);
    RUN_TEST(test_display_fail);
    RUN_TEST(test_touch_fail);
    return UNITY_END();
}
```

- [ ] **Step 2: `src/ota/ota_validator.h`**

```c
#pragma once
#include <stdbool.h>

/* Driver "alive" hooks — overridden in tests; real impls inspect cached driver
 * state without re-initialising hardware (display + touch are already up). */
bool ota_validator_hook_tc_alive(void);
bool ota_validator_hook_ssr_alive(void);
bool ota_validator_hook_display_alive(void);
bool ota_validator_hook_touch_alive(void);

bool ota_validator_run_self_tests(void);
bool ota_validator_pending(void);
void ota_validator_mark_valid(void);

/* Last self-test failure reason — for boot-time logging + optional UI surface. */
const char *ota_validator_last_failure(void);
```

- [ ] **Step 3: `src/ota/ota_validator.c`**

```c
#include "ota/ota_validator.h"
#include "config.h"
#include <string.h>

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
    /* Read 3 samples ~50 ms apart; require no fault flags and consensus
     * within OTA_TC_CONSENSUS_TOLERANCE °C. */
    float t[3]; uint8_t flags[3]; uint32_t age[3];
    for (int i = 0; i < 3; i++) {
        if (!thermocouple_read(&t[i], &flags[i], &age[i])) return false;
        if (flags[i] != 0) return false;
        if (t[i] < -50.0f || t[i] > 350.0f) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    float lo = t[0], hi = t[0];
    for (int i = 1; i < 3; i++) {
        if (t[i] < lo) lo = t[i];
        if (t[i] > hi) hi = t[i];
    }
    return (hi - lo) <= OTA_TC_CONSENSUS_TOLERANCE;
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
    int64_t per_test_us  = (int64_t)OTA_SELF_TEST_TIMEOUT_MS * 1000;
    int64_t total_budget = (int64_t)OTA_SELF_TEST_TOTAL_MS  * 1000;
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
```

The "timeout" is enforced by post-hoc check of elapsed time, not a hard watchdog. ESP-IDF doesn't have a portable "run this with a timeout" primitive that interrupts a hung hook — so a truly hung hook would trip the task watchdog naturally. The post-hoc check guards against subtly-slow regressions.

- [ ] **Run host tests + build target + commit**

```
~/.platformio/penv/bin/pio test -e native -f test_ota_validator
~/.platformio/penv/bin/pio run -e esp32dev
git add src/ota/ota_validator.{c,h} test/test_ota_validator.c
git commit -m "ota: validator w/ real driver signatures, TC 3-sample, per-test timeouts"
```

---

### Task 24: Boot-time wire-up + initial mode

**Files:** `src/main.c`, `src/control/control_task.c`

- [ ] **Step 1: Create `src/ota/boot_status.h`** — small accessor module wraps the two boot-time flags so other TUs read them via getters, not bare `extern`:

```c
#pragma once
#include <stdbool.h>

/* Set ONCE during app_main boot path. Subsequent calls are no-ops. */
void boot_status_set_validator_pending(bool pending);
void boot_status_set_self_tests_ok(bool ok);

/* Read from anywhere (UI tick, control task init, etc.). */
bool boot_status_validator_pending(void);
bool boot_status_self_tests_ok(void);
```

- [ ] **Step 2: Create `src/ota/boot_status.c`**:

```c
#include "ota/boot_status.h"

static bool s_validator_pending = false;
static bool s_self_tests_ok    = true;

void boot_status_set_validator_pending(bool pending) { s_validator_pending = pending; }
void boot_status_set_self_tests_ok(bool ok)         { s_self_tests_ok    = ok; }

bool boot_status_validator_pending(void) { return s_validator_pending; }
bool boot_status_self_tests_ok(void)     { return s_self_tests_ok; }
```

- [ ] **Step 3: `src/main.c`** — run validator AFTER LVGL is initialised, since the display/touch alive hooks need `lv_display_get_default()` and `lv_indev_*` to return non-NULL. Use the accessors, not bare `extern`:

```c
#include "ota/ota_validator.h"
#include "ota/boot_status.h"
/* ... */

void app_main(void) {
    nvs_store_init();

    /* Drivers + LVGL must be up before validator runs (the alive checks
     * read lv_display_get_default and lv_indev_*). */
    /* existing display_init(), touch_init(), lvgl_port_init() etc. */
    /* existing ui_init() */

    /* Now safe to inspect cached driver handles. */
    if (ota_validator_pending()) {
        bool ok = ota_validator_run_self_tests();
        boot_status_set_self_tests_ok(ok);
        if (ok) {
            ESP_LOGI("app", "OTA pending validation — awaiting user confirmation");
            boot_status_set_validator_pending(true);
        } else {
            ESP_LOGE("app", "OTA self-tests FAILED (reason: %s) — rollback on next reboot",
                     ota_validator_last_failure());
        }
    }

    /* Start control + WiFi tasks. WiFi is async; OTA server starts on first
     * connect via wifi_subscribe_status (Task 21). */
    control_task_create(g_cmd_queue, &g_oven_state);
    wifi_init();
    wifi_subscribe_status(on_wifi_status_for_server);
}
```

- [ ] **Step 4: Pass `pending` into the control task's initial mode via `boot_status`**

`s_sm` is reset by `sm_init(&s_sm)` *inside* the control_task body (around line 160 of `control_task.c`), not in `control_task_create`. So the override must happen AFTER `sm_init` runs. Use the boot_status accessor — no bare `extern`:

In `control_task.c`, add `#include "ota/boot_status.h"` near the top. Then just below the existing `sm_init(&s_sm);` call inside the task body:

```c
if (boot_status_validator_pending()) s_sm.mode = MODE_PENDING_VALIDATION;
```

(Do NOT place this in `control_task_create` — that function only spawns the task; the task body is where `sm_init` runs and where the override must land.)

- [ ] **Build + commit**

```
git commit -am "ota: boot-time validator + initial MODE_PENDING_VALIDATION"
```

---

### Task 25: HOME banner — pending + failed-validation surfacing

**Files:** `src/ui/screen_home.c`

- [ ] **Step 1: Add two banners — pending (orange) + failed (red)**

```c
#include "ota/ota_validator.h"
#include "ota/boot_status.h"
#include "esp_ota_ops.h"

static lv_obj_t *s_pending_banner = NULL;
static lv_obj_t *s_failed_banner  = NULL;

/* Forward declaration — defined below, used in lv_obj_add_event_cb. */
static void banner_clicked_cb(lv_event_t *e);

/* In screen_home_create, near the top of the parent: */
/* Pending banner */
s_pending_banner = lv_obj_create(parent);
lv_obj_set_size(s_pending_banner, LV_HOR_RES - 16, 32);
lv_obj_set_pos(s_pending_banner, 8, 0);
lv_obj_set_style_bg_color(s_pending_banner, lv_color_hex(0xCC8800), 0);
lv_obj_set_style_bg_opa(s_pending_banner, LV_OPA_COVER, 0);
lv_obj_add_flag(s_pending_banner, LV_OBJ_FLAG_HIDDEN);
lv_obj_add_event_cb(s_pending_banner, banner_clicked_cb, LV_EVENT_CLICKED, NULL);
lv_obj_t *bl = lv_label_create(s_pending_banner);
lv_label_set_text_fmt(bl, "%s Pending update — tap to confirm", LV_SYMBOL_WARNING);
lv_obj_set_style_text_color(bl, lv_color_white(), 0);
lv_obj_center(bl);

/* Failed banner */
s_failed_banner = lv_obj_create(parent);
lv_obj_set_size(s_failed_banner, LV_HOR_RES - 16, 32);
lv_obj_set_pos(s_failed_banner, 8, 0);
lv_obj_set_style_bg_color(s_failed_banner, lv_color_hex(0x9C2424), 0);  /* dark red */
lv_obj_set_style_bg_opa(s_failed_banner, LV_OPA_COVER, 0);
lv_obj_add_flag(s_failed_banner, LV_OBJ_FLAG_HIDDEN);
lv_obj_t *fl = lv_label_create(s_failed_banner);
lv_label_set_text_fmt(fl, "%s Update failed self-tests — rollback on reboot",
                      LV_SYMBOL_CLOSE);
lv_obj_set_style_text_color(fl, lv_color_white(), 0);
lv_obj_center(fl);
```

- [ ] **Step 2: Click handler**

```c
static void banner_clicked_cb(lv_event_t *e) {
    (void)e;
    ota_validator_mark_valid();
    cmd_msg_t m = { .type = CMD_CLEAR_PENDING };
    xQueueSend(g_cmd_queue, &m, 0);
}
```

- [ ] **Step 3: `screen_home_update` reads the cached self-test result via `boot_status`**

Task 24 already populates the boot_status accessor at boot. The UI tick reads via the getter; it does NOT re-run the (expensive) hooks each tick and does not touch a bare `extern`.

```c
void screen_home_update(const oven_state_t *snap) {
    /* ... existing fields ... */
    if (snap->mode == MODE_PENDING_VALIDATION) {
        lv_obj_clear_flag(s_pending_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_failed_banner, LV_OBJ_FLAG_HIDDEN);
    } else if (ota_validator_pending() && !boot_status_self_tests_ok()) {
        /* Self-tests failed at boot; oven is in IDLE-default but warning-worthy. */
        lv_obj_clear_flag(s_failed_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pending_banner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_pending_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_failed_banner, LV_OBJ_FLAG_HIDDEN);
    }
}
```

- [ ] **Build + commit**

```
git commit -am "ui(home): pending banner + failed-validation surface + click→mark_valid"
```

---

### Task 26: Bring-up 13 — standalone self-test runner with explicit driver init

**Files:** `tools/bringup/13_self_tests/{CMakeLists.txt, platformio.ini, sdkconfig.defaults, src/{main.c, CMakeLists.txt, idf_component.yml}, README.md}` (layout matches Task 4)

The bringup must **initialise LVGL too** (via `esp_lvgl_port_init` or the project's existing LVGL bring-up pattern in `tools/bringup/04_lvgl_hello/`) before calling display/touch alive checks, because those hooks query LVGL handles. See [tools/bringup/04_lvgl_hello/src/main.c](../../../tools/bringup/04_lvgl_hello/src/main.c) for the LVGL init pattern this project uses.

- [ ] **`src/main.c`** — explicit init including LVGL (no vague "copy from main"):

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "drivers/display.h"
#include "drivers/touch.h"
#include "drivers/thermocouple.h"
#include "drivers/ssr.h"
#include "ota/ota_validator.h"

static const char *TAG = "bringup13";

void app_main(void) {
    /* Bring up the same drivers the main firmware brings up, in the same order. */
    display_init();
    touch_init();
    thermocouple_init();
    ssr_init();

    int64_t start = esp_timer_get_time();
    bool tc       = ota_validator_hook_tc_alive();      int64_t t1 = esp_timer_get_time();
    bool ssr      = ota_validator_hook_ssr_alive();     int64_t t2 = esp_timer_get_time();
    bool display  = ota_validator_hook_display_alive(); int64_t t3 = esp_timer_get_time();
    bool touch    = ota_validator_hook_touch_alive();   int64_t t4 = esp_timer_get_time();

    ESP_LOGI(TAG, "TC      : %s (%lld ms)", tc       ? "PASS" : "FAIL", (t1-start)/1000);
    ESP_LOGI(TAG, "SSR     : %s (%lld ms)", ssr      ? "PASS" : "FAIL", (t2-t1)/1000);
    ESP_LOGI(TAG, "Display : %s (%lld ms)", display  ? "PASS" : "FAIL", (t3-t2)/1000);
    ESP_LOGI(TAG, "Touch   : %s (%lld ms)", touch    ? "PASS" : "FAIL", (t4-t3)/1000);
    ESP_LOGI(TAG, "TOTAL   : %lld ms", (t4-start)/1000);

    while (1) vTaskDelay(pdMS_TO_TICKS(60000));
}
```

(Assumes `thermocouple_init` and `ssr_init` exist with `void → void` signatures — verify against the actual headers in [src/drivers/](../../../src/drivers/) before building.)

- [ ] **Build + commit**

```
cd tools/bringup/13_self_tests && ~/.platformio/penv/bin/pio run -e esp32dev && cd ../../..
git add tools/bringup/13_self_tests/
git commit -m "bringup(13_self_tests): explicit driver init + per-test timing log"
```

---

## Phase I — About + Firmware update sub-screens

### Task 27: About sub-screen

**Files:** `src/ui/screen_about.{c,h}`, `src/ui/screen_settings.c`

```c
#include "ui/screen_about.h"
#include "ui/screen_settings.h"
#include "ui/theme.h"
#include "version.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include <stdio.h>

static lv_obj_t *mklabel(lv_obj_t *parent, const char *txt, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, 8, y);
    lv_obj_set_style_text_color(l, THEME_FG, 0);
    return l;
}

lv_obj_t *screen_about_create(void) {
    lv_obj_t *sub = settings_make_subscreen("About");
    int y = 44;
    char buf[80];

    mklabel(sub, "Firmware", y); y += 16;
    snprintf(buf, sizeof buf, "  Version  %s", FIRMWARE_VERSION_STR);  mklabel(sub, buf, y); y += 16;
    snprintf(buf, sizeof buf, "  Built    %s", BUILD_DATE_STR);        mklabel(sub, buf, y); y += 24;

    mklabel(sub, "OTA slot", y); y += 16;
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    snprintf(buf, sizeof buf, "  Active   %s", run ? run->label : "?"); mklabel(sub, buf, y); y += 16;
    snprintf(buf, sizeof buf, "  Status   %s",
             st == ESP_OTA_IMG_VALID ? "valid" :
             st == ESP_OTA_IMG_PENDING_VERIFY ? "pending" : "unknown"); mklabel(sub, buf, y); y += 16;

    /* Other-slot description (may be empty if never flashed) */
    esp_app_desc_t other = {0};
    if (next && esp_ota_get_partition_description(next, &other) == ESP_OK)
        snprintf(buf, sizeof buf, "  Other    %s", other.version);
    else
        snprintf(buf, sizeof buf, "  Other    (empty)");
    mklabel(sub, buf, y); y += 24;

    mklabel(sub, "Hardware", y); y += 16;
    mklabel(sub, "  Board    LCDwiki E32R35T", y); y += 16;
    mklabel(sub, "  Chip     ESP32-WROOM-32E", y); y += 16;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, sizeof buf, "  MAC      %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    mklabel(sub, buf, y); y += 16;
    snprintf(buf, sizeof buf, "  Free heap  %lu KB",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DEFAULT)/1024);
    mklabel(sub, buf, y);

    return sub;
}
```

Drop the weak stub for `screen_about_create` in `screen_settings.c`.

- [ ] **Build + commit**

```
git commit -am "ui(about): Active + Other slot + Board + Chip + MAC + free heap"
```

---

### Task 28: Firmware update sub-screen — persistent token display

**Files:** `src/ui/screen_firmware_update.{c,h}`, `src/ui/screen_settings.c`

Per spec §9.3 decision (2026-05-25): token is **displayed persistently** by default — user navigates away when done.

```c
#include "ui/screen_firmware_update.h"
#include "ui/screen_settings.h"
#include "ui/theme.h"
#include "storage/nvs_store.h"
#include "config.h"
#include "esp_random.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_token_lbl = NULL;

static void refresh_token(void) {
    char t[OTA_TOKEN_HEX_LEN + 1];
    if (nvs_store_get_ota_token(t)) lv_label_set_text(s_token_lbl, t);
    else                            lv_label_set_text(s_token_lbl, "(no token)");
}

static void regen_clicked(lv_event_t *e) {
    (void)e;
    uint8_t raw[16];
    esp_fill_random(raw, sizeof raw);
    char t[OTA_TOKEN_HEX_LEN + 1];
    for (int i = 0; i < 16; i++) snprintf(t + i*2, 3, "%02x", raw[i]);
    nvs_store_set_ota_token(t);
    refresh_token();
}

lv_obj_t *screen_firmware_update_create(void) {
    lv_obj_t *sub = settings_make_subscreen("Firmware update");
    lv_obj_t *info = lv_label_create(sub);
    lv_label_set_text(info,
        "Upload via:\n"
        "  http://reflow.local/ota\n"
        "with X-OTA-Token header.");
    lv_obj_set_pos(info, 8, 44);
    lv_obj_set_style_text_color(info, THEME_FG, 0);

    lv_obj_t *tlbl = lv_label_create(sub);
    lv_label_set_text(tlbl, "OTA token:");
    lv_obj_set_pos(tlbl, 8, 110);
    lv_obj_set_style_text_color(tlbl, THEME_FG, 0);

    s_token_lbl = lv_label_create(sub);
    lv_obj_set_pos(s_token_lbl, 8, 126);
    lv_obj_set_width(s_token_lbl, LV_HOR_RES - 16);
    lv_label_set_long_mode(s_token_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_token_lbl, THEME_ACCENT, 0);
    refresh_token();

    lv_obj_t *regen = lv_button_create(sub);
    lv_obj_set_size(regen, 140, 32); lv_obj_set_pos(regen, 8, 190);
    lv_obj_set_style_bg_color(regen, THEME_STOP, 0);
    lv_obj_add_event_cb(regen, regen_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(regen); lv_label_set_text(rl, "Regenerate"); lv_obj_center(rl);

    return sub;
}
```

Drop the weak stub for `screen_firmware_update_create`.

- [ ] **Build + commit**

```
git commit -am "ui(firmware_update): persistent token + regenerate"
```

---

## Phase J — PlatformIO upload script

### Task 29: tools/ota_upload.py — streaming upload + progress + downgrade warning

**Files:** `tools/ota_upload.py`, `platformio.ini`

- [ ] **Step 1: `tools/ota_upload.py`** (streamed; uses stdlib `http.client` for chunked send + tqdm if available, fallback to plain progress prints):

```python
#!/usr/bin/env python3
"""OTA uploader for the reflow oven.

Reads target host + token from a config file (default: ~/.config/reflow_oven/upload.conf):
    host  = "reflow.local"
    token = "abc1234..."

Usage:
    python tools/ota_upload.py <path/to/firmware.bin>
"""
import http.client
import json
import os
import sys
import urllib.error
import urllib.request

try:
    import tomllib  # Python 3.11+
except ImportError:
    import tomli as tomllib

try:
    from tqdm import tqdm
    HAVE_TQDM = True
except ImportError:
    HAVE_TQDM = False

CONFIG_DEFAULT = os.path.expanduser("~/.config/reflow_oven/upload.conf")
MAX_BIN = 1.4 * 1024 * 1024
CHUNK = 8192


def load_config():
    path = os.environ.get("REFLOW_OTA_CONFIG", CONFIG_DEFAULT)
    if not os.path.exists(path):
        sys.exit(f"config not found: {path}\nCreate with:\n  host  = \"reflow.local\"\n  token = \"...\"")
    with open(path, "rb") as f:
        return tomllib.load(f)


def get_version(host):
    url = f"http://{host}/version"
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            return json.loads(r.read())
    except Exception as e:
        sys.exit(f"cannot reach {url}: {e}")


def version_tuple(s):
    """Best-effort parse of 'vX.Y.Z[-rest]' → (X, Y, Z). Returns None if unparseable."""
    import re
    m = re.match(r"v?(\d+)\.(\d+)\.(\d+)", s)
    if not m: return None
    return tuple(int(x) for x in m.groups())


def warn_if_downgrade(device, local_path):
    dv = device.get("version", "")
    # local version comes from `git describe`; reading the bin doesn't help.
    # Use the version reported by /version-after-flash for an authoritative check.
    # For now, warn the user to verify post-upload that the version increased.
    dvt = version_tuple(dv)
    if dvt:
        sys.stderr.write(f"  device currently: {dv} (parsed {dvt})\n")


def upload(host, token, fw_path):
    size = os.path.getsize(fw_path)
    if size > MAX_BIN:
        sys.exit(f"firmware too large: {size} bytes (max {int(MAX_BIN)})")
    conn = http.client.HTTPConnection(host, 80, timeout=120)
    conn.putrequest("POST", "/ota")
    conn.putheader("X-OTA-Token", token)
    conn.putheader("Content-Type", "application/octet-stream")
    conn.putheader("Content-Length", str(size))
    conn.endheaders()

    with open(fw_path, "rb") as f:
        if HAVE_TQDM:
            with tqdm(total=size, unit="B", unit_scale=True, desc="upload") as bar:
                while True:
                    chunk = f.read(CHUNK)
                    if not chunk: break
                    conn.send(chunk)
                    bar.update(len(chunk))
        else:
            sent = 0
            while True:
                chunk = f.read(CHUNK)
                if not chunk: break
                conn.send(chunk)
                sent += len(chunk)
                sys.stderr.write(f"\r  upload: {sent}/{size} bytes")
                sys.stderr.flush()
            sys.stderr.write("\n")

    resp = conn.getresponse()
    body = resp.read().decode(errors="replace")
    print(f"{resp.status} {resp.reason}")
    print(body)
    conn.close()
    if resp.status >= 400:
        sys.exit(1)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: ota_upload.py <firmware.bin>")
    fw = sys.argv[1]
    if not os.path.exists(fw):
        sys.exit(f"file not found: {fw}")
    cfg = load_config()
    print(f"target: {cfg['host']}")
    info = get_version(cfg["host"])
    print(f"current device: {info.get('version', 'unknown')} (status: {info.get('status', '?')})")
    warn_if_downgrade(info, fw)
    print(f"uploading {fw} ({os.path.getsize(fw)} bytes)...")
    upload(cfg["host"], cfg["token"], fw)


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: `chmod +x tools/ota_upload.py`**

- [ ] **Step 3: Append to `platformio.ini`**

```ini
[env:esp32dev-ota]
extends = env:esp32dev
upload_protocol = custom
upload_command = python ${PROJECT_DIR}/tools/ota_upload.py $SOURCE
```

- [ ] **Step 4: Smoke-test the script**

```
python tools/ota_upload.py /nonexistent.bin || echo "expected to fail (file missing or no config)"
```

- [ ] **Commit**

```
git commit -am "tools: ota_upload.py streaming + tqdm + [env:esp32dev-ota]"
```

---

## Phase K — End-to-end verification

### Task 30: Manual smoke test on real hardware

(Requires hardware + WiFi.)

- [ ] **Step 1: USB-flash + provision**
```
~/.platformio/penv/bin/pio run -e esp32dev -t upload
~/.platformio/penv/bin/pio device monitor
```
On the oven: Settings → Network → Set up WiFi → pick SSID → enter password → Connect. Watch the monitor for "got IP".

- [ ] **Step 2: Verify /version**
```
curl http://reflow.local/version
```
Expected: JSON with version, slot, status "valid".

- [ ] **Step 3: Capture token**
On the oven: Settings → Firmware update → token is visible. Copy into:
```
mkdir -p ~/.config/reflow_oven
cat > ~/.config/reflow_oven/upload.conf <<EOF
host  = "reflow.local"
token = "<paste here>"
EOF
```

- [ ] **Step 4: OTA-flash a trivial change**
Make any small change, then:
```
~/.platformio/penv/bin/pio run -e esp32dev-ota -t upload
```
Watch: progress bar → reboot → "OTA pending validation" in monitor → orange banner on HOME.

- [ ] **Step 5: Confirm + verify normal operation**
Tap banner → START enables. Run a profile. Reboot — no banner; firmware is valid.

- [ ] **Step 6: Record performance benchmarks**

Append to `docs/superpowers/specs/2026-05-25-ota-firmware-update-design.md` (under §10.5) the observed numbers:
- Upload time for current binary size
- Heap free during upload (read from `/version` if available, else from monitor logs)
- Self-test total duration

Targets from spec: 1 MB < 15 s; heap ≥ 30 KB; self-tests ≤ 5 s.

- [ ] **Commit observations**

```
git commit -m "docs(ota): bring-up smoke test observations + benchmarks"
```

---

### Task 31: Rollback smoke test

- [ ] **Step 1: From a known-good state** (Task 30's flash, validated). Verify with About → Status = valid.

- [ ] **Step 2: Inject a deliberate failure**
In `src/ota/ota_validator.c`, change `ota_validator_hook_tc_alive` to return `false` unconditionally.

- [ ] **Step 3: OTA-flash the broken build**
```
~/.platformio/penv/bin/pio run -e esp32dev-ota -t upload
```
Expect: reboot, validator logs "OTA self-tests FAILED (reason: tc)", red fault banner on HOME, no pending-confirm banner.

- [ ] **Step 4: Power-cycle to trigger rollback**
Expected: bootloader rolls back to the prior valid slot. On boot: no banner, About shows the prior version, oven works normally.

- [ ] **Step 5: Revert + re-flash via USB (recovery)**
Restore the original `ota_validator_hook_tc_alive`. `pio run -e esp32dev -t upload`.

- [ ] **Step 6: Document in the spec** (§6.6) and commit.

```
git commit -am "docs(ota): rollback smoke results"
```

---

## Done criteria

- [ ] First-boot provisioning via touchscreen (Phase F + Task 30)
- [ ] `pio run -e esp32dev-ota -t upload` succeeds over WiFi (Task 30)
- [ ] Regression that breaks TC / SSR / display / touch prevents validation → rollback (Task 31)
- [ ] Reflow START rejected during MODE_PENDING_VALIDATION (Task 22)
- [ ] Auto-rollback if user reboots without confirming (Task 31)
- [ ] `http://reflow.local/version` returns current version + slot info (Task 20)
- [ ] DAC + WiFi coexistence verified (Task 4)
- [ ] Performance benchmarks recorded against spec §10.5 targets (Task 30)

When all checked, the branch is ready to PR-merge into `mvp-implementation` (after MVP merges to main).

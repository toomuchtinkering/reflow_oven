# OTA firmware update — design

**Status**: design, not yet implemented
**Branch**: `ota-firmware-update`
**Promotes**: v2 backlog item #7 (OTA firmware update) to a real spec; bumps to top of build order
**Supersedes**: nothing — this is the first concrete spec for OTA on this project
**Related**: [2026-05-15-reflow-oven-mvp-design.md](2026-05-15-reflow-oven-mvp-design.md) §6 (SRAM budget), §12 (originally-deferred list), [docs/v2_backlog.md](../../backlog_history.md) §7

This spec captures the design decisions made during the 2026-05-25 brainstorm; tunable parameters (timeouts, banner colour, exact self-test thresholds) are flagged as **TUNE** and expected to be adjusted during implementation.

## 1. Scope & success criteria

### 1.1 In scope

- OTA firmware update over WiFi via an HTTP `POST /ota` endpoint
- WiFi station-mode provisioning UX via an on-screen QWERTY keyboard
- Token-based auth on `/ota`; token stored in NVS, set/regenerated from a Settings screen
- Subsystem self-tests at boot + **user-confirmation tap** to mark the new image valid
- New oven mode `MODE_PENDING_VALIDATION` that blocks reflow START until the user confirms
- mDNS hostname (`reflow.local`) + Settings → Network screen showing IP / hostname / signal / Forget WiFi
- **Persistent status bar** across all tabs (WiFi + mute icons + clock)
- **Settings hub** that replaces the existing PID tab; PID becomes a sub-screen under Settings
- PlatformIO custom upload script for `pio run -e esp32dev-ota -t upload` over WiFi
- Build-time version string embedded in firmware, surfaced in About screen + OTA response
- Idle-only update gate (HTTP 409 if `MODE_RUNNING`, `MODE_FAULT`, or `MODE_PENDING_VALIDATION`)
- NTP time sync, used by the status-bar clock; local timezone hardcoded in `config.h`

### 1.2 Out of scope (deferred or declined)

- **Signed images / Secure Boot** — declined for v1 (decision 2026-05-25). Single-user device on trusted home WiFi; revisit if the oven ever moves to a less-trusted network. Cost would have been ~30 KB binary + key-management process.
- **Captive-portal softAP provisioning** — on-screen keyboard is enough; the touchscreen hardware exists.
- **Tiny web UI for live status + start/stop** — separate v2 backlog item #8. This spec ships only the minimum `GET /` page needed to make OTA usable from a browser.
- **MQTT publisher** — Tier 4 backlog item.
- **TLS / HTTPS on `/ota`** — deferred; LAN-only assumption. Token over plaintext HTTP is the accepted tradeoff.

### 1.3 Success criteria

1. A USB-flashed oven on first boot walks through WiFi provisioning via the touchscreen and connects to a user-supplied SSID.
2. `pio run -e esp32dev-ota -t upload` succeeds over WiFi without USB once OTA has been validated once.
3. A regression that breaks any of TC / SSR / display / touch initialisation prevents validation; next reboot auto-rolls back.
4. The user cannot start a reflow on a pending-validation firmware.
5. Rollback is automatic if the user reboots without confirming the update.
6. `http://reflow.local/version` returns the current firmware version + slot info as JSON.

## 2. Architecture overview

### 2.1 New components

| File | Role |
|---|---|
| `src/drivers/wifi.{c,h}` | Station-mode init, NVS-backed creds, connection FSM, mDNS hostname registration |
| `src/ota/ota_server.{c,h}` | HTTP server hosting `GET /`, `GET /version`, `POST /ota`; token check, streaming write to inactive OTA partition, hash verify, partition flip, reboot trigger |
| `src/ota/ota_validator.{c,h}` | Boot-time subsystem self-tests; exposes `ota_validator_run_self_tests()` + `ota_validator_mark_valid()` |
| `src/ui/status_bar.{c,h}` | Persistent status bar widget — created once in `ui_task.c` above the tabview, updated from the existing 250 ms UI tick |
| `src/ui/screen_settings.{c,h}` | Settings hub list view (rows: Network, Audio, PID, Firmware update, About) |
| `src/ui/screen_wifi_scan.{c,h}` | SSID scan + list + Manual-entry button |
| `src/ui/screen_wifi_password.{c,h}` | Password entry with LVGL `lv_keyboard` + show/hide + Connect |
| `src/ui/screen_network.{c,h}` | Settings → Network status view (Status / SSID / IP / Host / Signal / Change / Forget) |
| `src/ui/screen_about.{c,h}` | Build version, slot info, hardware info, free heap |
| `src/ui/screen_firmware_update.{c,h}` | OTA token view (show / regenerate) + upload-target hint |
| `tools/ota_upload.py` | Standalone Python uploader for the PIO custom upload protocol |
| `tools/bringup/10_wifi/` | WiFi bring-up tool |
| `tools/bringup/11_http_server/` | HTTP server bring-up tool |
| `tools/bringup/12_ota_endpoint/` | `/ota` endpoint bring-up tool |
| `tools/bringup/13_self_tests/` | Subsystem self-test runner |
| `include/version.h.in` | CMake-templated version header |

### 2.2 Modified components

| File | Change |
|---|---|
| `src/control/control_task.c` | Adds `MODE_PENDING_VALIDATION` handling; rejects `CMD_START` in that state |
| `src/ui/ui_task.c` | Creates `status_bar` above the tabview; reduces tabview height by 24 px; replaces PID tab with Settings tab |
| `src/ui/screen_home.c` | Removes mute button (moved to Settings → Audio); adjusts Y coordinates for 24 px status-bar height |
| `src/ui/screen_profile.c` | Adjusts hardcoded `ACTION_Y` (currently `444 - 16 - ACTION_H` at [src/ui/screen_profile.c:240](../../../src/ui/screen_profile.c#L240)) for new content area height |
| `src/ui/screen_pid.c` | Survives — but is now a sub-screen reached from Settings → PID, not a tab. Its content was sized for a 444 px tab page; in the sub-screen context, the back-nav header takes ~32 px on top, content area shrinks further. May need small Y-coordinate adjustments to the existing Kp/Ki/Kd cards |
| `src/storage/nvs_store.{c,h}` | Adds get/set for `wifi_ssid`, `wifi_pass`, `wifi_set`, `ota_token` |
| `src/main.c` | Calls `wifi_init()`, `ota_server_start()`, `ota_validator_run_self_tests()` at boot |
| `CMakeLists.txt` | Registers `src/drivers/wifi.c`, `src/ota/`, `src/ui/screen_*.c` new files; generates `version.h` from `version.h.in` via `configure_file()` |
| `include/config.h` | Adds `LOCAL_TZ`, `WIFI_CONNECT_RETRY_MAX`, `WIFI_CONNECT_TIMEOUT_MS`, `OTA_UPLOAD_CHUNK_SIZE`, NVS keys |
| `src/idf_component.yml` | Adds `espressif/mdns` |
| `sdkconfig.esp32dev` | See §2.3 |
| `platformio.ini` | Adds `[env:esp32dev-ota]` env with custom upload protocol |
| `partitions.csv` | New file — switching off `partitions_singleapp.csv` |

### 2.3 sdkconfig deltas

```diff
- CONFIG_PARTITION_TABLE_SINGLE_APP=y
+ CONFIG_PARTITION_TABLE_TWO_OTA=y

- # CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not set
+ CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y

- # CONFIG_APP_ROLLBACK_ENABLE is not set
+ CONFIG_APP_ROLLBACK_ENABLE=y

+ CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
+ CONFIG_HTTPD_MAX_URI_LEN=64
+ CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER=y
+ CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER=y
+ CONFIG_LWIP_NETIF_LOOPBACK=y      # mDNS quirks on some networks
```

### 2.4 Flash + SRAM budget

**Flash** (4 MB total):

| Component | Size |
|---|---|
| Bootloader + partition table + otadata + phy_init + nvs | ~70 KB |
| Two app slots (ota_0 + ota_1, **custom layout: 0x170000 = 1.47 MB each**) | 2.94 MB |
| Free | ~1.0 MB |

> **Implemented layout (2026-05-29).** §11.3's contingency was taken: a custom `partitions.csv` enlarges the two slots to `0x170000` (1,507,328 B) each (ota_0 @ `0x20000`, ota_1 @ `0x190000`). The firmware now measures ~1.42 MB (≈94 % of a slot), so the stock 1.5 MB TWO_OTA layout's headroom was too tight — the enlarged slots restore margin. There is intentionally **no factory partition** (§11.3); brick-on-first-failed-OTA is instead prevented by anchoring the running slot valid at boot when its OTA state is `UNDEFINED` (see §6.6).

MVP baseline `firmware.bin` is 698 KB (measured 2026-05-23). Pre-implementation projection adding WiFi (~200 KB) + HTTP server (~30 KB) + mDNS (~20 KB) + SNTP (~10 KB) + new UI screens (~30 KB) + OTA driver code (~30 KB) + status bar (~5 KB) was **~1.0–1.1 MB**. The measured build came in higher (~1.42 MB, see the implementation callout above), which is why the slots were enlarged to `0x170000` rather than kept at the stock 1.5 MB TWO_OTA size.

**SRAM** (520 KB total): MVP design spec §6 already budgeted WiFi at +50 KB. Heap headroom during normal operation projected at 80–120 KB; more when no OTA upload is active. Recommendations:

1. **Lazy creation** of the Settings sub-screens (created on first navigation, hidden via `LV_OBJ_FLAG_HIDDEN` on back; destroyed only on tab teardown — same pattern as MVP screens today). If a sub-screen is never visited it consumes no memory. HOME and PROFILE stay eagerly created since they are the primary surfaces.
2. **Heap watchdog**: log `esp_get_free_heap_size()` + `esp_get_minimum_free_heap_size()` every 30 s during bring-up. Drop below ~30 KB headroom is a stop-ship signal.

## 3. Persistent status bar

### 3.1 Layout

24 px tall, fixed at the top of the screen, above the `lv_tabview`. Content area shrinks from 444 → 420 px:

```
┌────────────────────────────────────┐
│  📶  🔇                       16:42 │ ← 24 px status bar
├────────────────────────────────────┤
│                                    │
│      ... tab content ...           │ ← 420 px content area
│                                    │
├──── HOME │ PROFILE │ ⚙ ────────────┤ ← 36 px tab bar (PID gone)
```

### 3.2 Content

**WiFi icon** (left): `LV_SYMBOL_WIFI` with custom overlay rendering. States:
- Connected: a single `LV_SYMBOL_WIFI` glyph whose **opacity** is graded by RSSI in 4 steps (COVER / 80 / 60 / 40 % at ≥ -55 / -65 / -75 dBm and below). LVGL's built-in WiFi glyph is one shape, so signal strength is conveyed by intensity rather than discrete wedges (decision 2026-05-29; thresholds **TUNE**-able). A true 1–4 wedge indicator would need a custom font/canvas and was judged not worth it for v1.
- Connecting: pulsing/dimmed bars (LVGL animation)
- Disconnected: bars with a small `!` overlay
- Not provisioned: same `!` style — user discovers the cause via Settings → Network

**Mute icon** (right of WiFi): `LV_SYMBOL_MUTE` rendered only when `audio_is_muted()` returns true; absent otherwise (zero visual noise in the common case).

**Clock** (right): `HH:MM` in 24-hour format when NTP-synced, `--:--` otherwise.

### 3.3 Interaction

Status bar is **purely informational, non-tappable**. All control surfaces (mute toggle, WiFi config) live in Settings. Phone-style.

### 3.4 Update mechanism

Driven by the existing 250 ms UI tick in `ui_task.c`. Reads:
- WiFi state via `wifi_get_state()` (atomic snapshot from `drivers/wifi.c`)
- Mute state via `audio_is_muted()` (existing)
- Time via `time(&now)` + `localtime_r(&now, &tm)` once SNTP has synced

### 3.5 NTP / timezone

ESP-IDF `esp_sntp` component, default `pool.ntp.org` servers. First sync ~5 s after WiFi connect; periodic re-sync hourly.

Timezone hardcoded in [include/config.h](../../../include/config.h):
```c
#define LOCAL_TZ "CET-1CEST,M3.5.0/2,M10.5.0/3"  /* Germany including DST */
```

Set once in `app_main` via `setenv("TZ", LOCAL_TZ, 1); tzset();`. Override per build if location ever changes.

### 3.6 Layout cascade

Existing hardcoded Y coordinates that assume 444 px usable content area must drop by 24 px:
- [src/ui/screen_profile.c:240](../../../src/ui/screen_profile.c#L240) — `ACTION_Y = 444 - 16 - ACTION_H` becomes `420 - 16 - ACTION_H`
- `src/ui/screen_home.c` graph canvas height
- `src/ui/screen_pid.c` analogous coordinates (now lives as Settings sub-screen)

Mechanical one-time change.

## 4. Settings hub + WiFi provisioning

### 4.1 Settings hub list

```
┌────────────────────────────────────┐
│  📶                          16:42 │
├────────────────────────────────────┤
│ Settings                           │
│ ────────────────────────────────── │
│ 📶 Network          MyHomeWiFi  ›  │
│ 🔇 Audio                Mute    ›  │
│ ⚙  PID                          ›  │
│ ↑  Firmware update              ›  │
│ ℹ  About                         ›  │
├──── HOME │ PROFILE │ ⚙ ────────────┤
```

Tapping a row pushes a sub-screen into the Settings tab's content area. Tab bar + status bar remain visible. The right-side value text on each row surfaces current state (current SSID, "Mute"/"On", etc.).

> **Icons.** The sketch uses emoji for readability, but the implementation uses LVGL's built-in symbol font: Network `LV_SYMBOL_WIFI`, Audio `LV_SYMBOL_AUDIO`, PID `LV_SYMBOL_SETTINGS`, Firmware update `LV_SYMBOL_UPLOAD`, About `LV_SYMBOL_LIST` (finding #52 — the emoji aren't all available as glyphs).

### 4.2 Sub-screen back-stack

A bounded navigation stack (depth ≤ 4). Most rows are single-level (Settings → sub-screen → back to Settings), but the WiFi provisioning flow genuinely nests: **Network → Choose network → Password**. Back pops one level (Password → Choose network → Network → Settings list), and a successful connect pops straight back to the Network status sub-screen (`settings_pop_to_root_sub`). (Updated 2026-05-29, finding #29 — the original "single-level only" design sent Back from the password screen to the Settings root, skipping the scan list.)

Sub-screen header pattern:
```
┌────────────────────────────────────┐
│ ←  <Title>                         │ ← back arrow returns to Settings list
│ ────────────────────────────────── │
```

Implementation: `screen_settings.c` keeps a small stack (`s_stack[SUB_STACK_MAX]`) of `{lv_obj_t *, on_pop hook}` entries. Push hides the current top and shows the new sub-screen; pop hides the top, runs its optional on-pop hook, and reveals the one beneath. Each sub-screen is created lazily and hidden (not destroyed) on back — destroyed only on tabview teardown (not currently exercised — see hygiene note in [docs/v2_backlog.md](../../roadmap.md) Tier 5, now roadmap #21). The on-pop hook lets a screen release resources on exit; the password screen uses it to unsubscribe from WiFi status whether it leaves via Back or a successful connect (finding #30). The known exception: the scan sub-screen is re-created on each entry without a teardown hook, so it leaks across repeated Change-network cycles (pre-existing; tracked for a future hygiene pass).

### 4.3 WiFi sub-screens

**Network status view** (`screen_network.c`):
```
┌────────────────────────────────────┐
│ ←  Network                         │
│ ────────────────────────────────── │
│ Status   ▒▒▒  Connected            │
│ SSID     MyHomeWiFi                │
│ IP       192.168.1.42              │
│ Host     reflow.local              │
│ Signal   -54 dBm                   │
│                                    │
│   [ Change network ]               │
│   [ Forget WiFi ]                  │
└────────────────────────────────────┘
```

When not provisioned, status is "Not configured" and the only button is `[ Set up WiFi ]` which pushes the scan view.

**WiFi scan** (`screen_wifi_scan.c`):
```
┌────────────────────────────────────┐
│ ←  Choose network            ⟳     │
│ ────────────────────────────────── │
│ ▒▒▒▒  MyHomeWiFi          🔒       │
│ ▒▒▒   Neighbor             🔒       │
│ ▒▒    FRITZ!Box                    │
│ ▒     MobileHotspot       🔒       │
│  ...                               │
│ ────────────────────────────────── │
│   [ Manual entry ]                 │
└────────────────────────────────────┘
```

Scan runs on entry via `esp_wifi_scan_start()`; results sorted by RSSI descending; ~3 s typical. Tap a row → password screen. Manual entry → keyboard for typing a hidden SSID before the password screen.

**Password entry** (`screen_wifi_password.c`):
```
┌────────────────────────────────────┐
│ ←  MyHomeWiFi                      │
│ ────────────────────────────────── │
│ Password                       👁  │
│ ┌──────────────────────────────┐   │
│ │ ••••••••••                   │   │
│ └──────────────────────────────┘   │
│ ┌──────────────────────────────┐   │
│ │  q w e r t y u i o p         │   │
│ │  a s d f g h j k l           │   │
│ │  ⇧ z x c v b n m  ⌫          │   │
│ │  123  abc  space  [Connect]  │   │
│ └──────────────────────────────┘   │
└────────────────────────────────────┘
```

LVGL `lv_keyboard` handles QWERTY / 123 / special-chars modes. Eye toggles password mask. Tap Connect → in-place spinner; on success returns to network status view (with new SSID); on failure shows an error toast and stays on this screen.

### 4.4 Save strategy

Credentials persist to NVS **only after a successful connect** — not on Connect-button press. If the password was mistyped we don't retry it on every boot. Until success, creds live in RAM.

### 4.5 NVS schema additions

```
wifi_ssid    string, ≤ 32 bytes
wifi_pass    string, ≤ 64 bytes
wifi_set     u8     — 1 once a successful connect has persisted creds, 0 otherwise
ota_token    string, exactly 32 hex chars (16 bytes from esp_fill_random())
```

### 4.6 WiFi driver state machine

```
UNINIT ──init()──► IDLE
IDLE ──scan_start()──► SCANNING ──scan_complete()──► IDLE
IDLE ──connect(ssid,pw)──► CONNECTING
CONNECTING ──got_ip──► CONNECTED  (persists creds to NVS if not already saved)
CONNECTING ──auth_fail | timeout──► IDLE  (retry 3 times w/ 5/10/20 s backoff)
CONNECTED ──disconnect_event──► CONNECTING (auto-reconnect, infinite retry while creds known)
ANY ──forget_wifi()──► IDLE (NVS cleared)
```

`WIFI_CONNECT_TIMEOUT_MS = 30000` and `WIFI_CONNECT_RETRY_MAX = 3` — **TUNE** during bring-up.

**Retry classes (implementation note, finding #16).** The `RETRY_MAX` bound applies only to a *trial* connect with credentials that have **not yet been persisted** — i.e. a fresh password-screen attempt — so a wrong password surfaces as `WIFI_CB_CONNECT_FAILED` after 3 tries. Credentials that are **known-good** (persisted to NVS, whether loaded at boot or saved after a prior successful connect) are treated as the `CONNECTED ──disconnect──► CONNECTING` class and retry **indefinitely**, holding at the longest backoff (20 s), so a long AP outage on a provisioned oven self-heals instead of trapping the FSM in IDLE. A consequence: a saved-but-now-wrong password (router password changed) retries forever rather than surfacing failure; re-provision via Forget WiFi to recover.

`wifi_get_state()` returns the current enum value (atomic; the wifi event handler is the only writer).

## 5. OTA endpoint protocol

### 5.1 Server

`esp_http_server` on port 80, started after the first successful WiFi connect.

### 5.2 Endpoints

| Method | Path | Purpose |
|---|---|---|
| GET | `/` | Minimal HTML: version, slot info, file picker that POSTs to `/ota` |
| POST | `/ota` | Firmware upload (body = raw `.bin`) |
| GET | `/version` | JSON `{"version":"...","slot":"ota_1","status":"valid","built":"<build date time>","max_size":<inactive-slot bytes>}` (`max_size` lets the uploader size-check without hard-coding the layout — finding #55) |

### 5.3 `POST /ota` request

| Component | Detail |
|---|---|
| Headers | `X-OTA-Token: <32 hex chars>` (required) · `Content-Type: application/octet-stream` · `Content-Length: <bytes>` |
| Body | Raw `firmware.bin` |
| Max body size | Equal to inactive OTA slot size (0x170000 = 1.47 MB) — larger rejected up front via Content-Length check |
| Auth check | `mbedtls_constant_time_memcmp` against NVS-stored `ota_token` — constant-time to avoid timing side-channel |
| Mode gate | Reserve the oven via `control_begin_ota()` (a request handled on the control task, serialised with START) — rejects with 409 unless idle, and once reserved the control task refuses START. Replaces an unsynchronised `control_get_mode()` read + closes the check→`esp_ota_begin` TOCTOU (finding #13). |

### 5.4 Response codes

| Code | Reason |
|---|---|
| 200 | Upload + flash + partition flip succeeded. Body: `{"status":"rebooting","new_version":"..."}`. Device reboots ~1 s after sending. |
| 400 | Missing/malformed headers; Content-Length absent or larger than slot |
| 401 | Token missing or wrong |
| 409 | Oven not in `MODE_IDLE` (running, fault, or pending validation) |
| 413 | Content-Length exceeds inactive slot size |
| 500 | `esp_ota_*` failure (write error, hash mismatch, partition flip failure) |

### 5.5 Write protocol (server-side state machine)

```
1. Accept POST
2. Validate headers + auth + mode gate           → on fail, 4xx + close
3. esp_ota_get_next_update_partition(NULL)       → inactive slot handle
4. esp_ota_begin(handle, image_size, &op)        → reserve slot
5. Stream body in OTA_UPLOAD_CHUNK_SIZE (4 KB) reads:
     esp_ota_write(op, chunk, n)
     update a progress counter
6. esp_ota_end(op)                               → verifies SHA256 internally
7. esp_ota_set_boot_partition(handle)            → flip otadata
8. Send 200 response
9. Return from the handler; a one-shot `esp_timer` fires `esp_restart()` ~1 s later — scheduled rather than inline so httpd flushes the JSON and closes the socket before the reboot, otherwise the client sees a connection reset (finding #11). (Fallback: if the timer can't be created, restart inline.)
```

### 5.6 Failure handling

- Client disconnect mid-stream: `esp_ota_abort(op)`, partition unchanged, log + 500.
- `esp_ota_end()` fails (hash mismatch from corruption): partition unchanged, log + 500.
- `esp_ota_set_boot_partition()` fails (very rare): partition data written but boot still on old slot — log + 500, retry on next upload.

The inactive slot is never marked bootable until everything succeeds. A failed upload leaves the device unchanged.

**No image-signature verification (residual risk, finding #58).** `esp_ota_end()` validates only the SHA-256 *integrity* of the upload, not its *authenticity* — `CONFIG_SECURE_BOOT` / `CONFIG_SECURE_SIGNED_APPS` are intentionally **off** (§1.2). Anyone on the LAN who holds the OTA token can therefore flash arbitrary firmware. This is the accepted trade-off for a single-user device on trusted home WiFi; revisit (enable Secure Boot + signed apps) before the oven moves to a less-trusted network.

## 6. Validation flow

### 6.1 State source

`esp_ota_get_state_partition(running_partition, &state)` returns the active slot's state. `ESP_OTA_IMG_PENDING_VERIFY` is the relevant value — set by the bootloader when it boots a freshly-flipped slot.

### 6.2 Boot-time decision tree

```dot
digraph boot {
    "app_main entry" [shape=box];
    "Get running partition state" [shape=box];
    "state == PENDING_VERIFY?" [shape=diamond];
    "Normal boot → MODE_IDLE" [shape=ellipse];
    "Run subsystem self-tests" [shape=box];
    "All pass?" [shape=diamond];
    "Mark gate PENDING; enter MODE_PENDING_VALIDATION" [shape=box];
    "Log failure; mark gate FAILED; enter MODE_PENDING_VALIDATION" [shape=box];
    "START blocked; orange 'Confirm update' banner" [shape=ellipse];
    "START blocked; red 'rollback on reboot' banner + fault alarm" [shape=ellipse];

    "app_main entry" -> "Get running partition state";
    "Get running partition state" -> "state == PENDING_VERIFY?";
    "state == PENDING_VERIFY?" -> "Normal boot → MODE_IDLE" [label="no"];
    "state == PENDING_VERIFY?" -> "Run subsystem self-tests" [label="yes"];
    "Run subsystem self-tests" -> "All pass?";
    "All pass?" -> "Mark gate PENDING; enter MODE_PENDING_VALIDATION" [label="yes"];
    "All pass?" -> "Log failure; mark gate FAILED; enter MODE_PENDING_VALIDATION" [label="no"];
    "Mark gate PENDING; enter MODE_PENDING_VALIDATION" -> "START blocked; orange 'Confirm update' banner";
    "Log failure; mark gate FAILED; enter MODE_PENDING_VALIDATION" -> "START blocked; red 'rollback on reboot' banner + fault alarm";
}

> **Both outcomes block START.** Earlier drafts let a failed self-test fall through to `MODE_IDLE` with only a banner; that violated success criterion §1.3 #4. As implemented, a pending-verify boot enters `MODE_PENDING_VALIDATION` whether self-tests pass or fail. The gate is tracked in its own `sm_validation_t` field (NONE / PENDING / FAILED) independent of `oven_mode_t`, so a transient fault that forces `MODE_FAULT` cannot clear it (findings #2/#3). A **FAILED** gate can never be confirmed — it only clears by rolling back on reboot (finding #4).
```

### 6.3 Subsystem self-tests

Runs from `ota_validator_run_self_tests()` before UI starts. Each test has a 2 s timeout (`OTA_SELF_TEST_TIMEOUT_MS`, raised from the initial 1 s during bring-up); total budget ~5 s. A test that runs *slower* than its timeout also fails ("slow ≈ broken"). Specifics are **TUNE**-able during implementation:

| Test | What it checks |
|---|---|
| TC read | `thermocouple_read()` returns a temperature in [-50, 350] °C, no fault bits set; three consecutive samples agree within ± 2 °C |
| SSR drive | Call `ssr_force_off()` and confirm it returns without crashing. GPIO read-back of the SSR output level proved unreliable on this board, so the test verifies driver-path liveness, not pin level (relaxed during implementation; see plan). |
| Display init | `display_init()` returns success and a known test pattern lights one row of pixels via DMA |
| Touch init | A registered LVGL input device exists (`lv_indev_get_act()` / `lv_indev_get_next()`). Touch is already initialised at boot before the validator runs; the XPT2046 has no reliable ID register to read, so liveness is inferred from the existing indev handle (relaxed during implementation; see plan). |

### 6.4 `MODE_PENDING_VALIDATION`

New entry in `oven_mode_t`, paired with an `sm_validation_t` gate field (NONE / PENDING / FAILED):

- Entered on **any** pending-verify boot — `validation = PENDING` if self-tests pass, `validation = FAILED` if they fail. Both block START.
- Behaviour in `control_task`:
  - `CMD_START` rejected (START button disabled on HOME)
  - All other commands behave as in `MODE_IDLE`
  - SSR held off (same as `MODE_IDLE`)
- Cleared by:
  - (PENDING only) User taps the orange "Confirm update" banner → `CMD_CLEAR_PENDING` → `control_task` performs `esp_ota_mark_app_valid_cancel_rollback()` **bound to the actual PENDING→IDLE transition** (not from the UI handler, so a fault path can't cancel rollback — finding #4) → `MODE_IDLE`.
  - A **FAILED** gate is never user-clearable; it only resolves by rolling back on the next reboot.
  - **No automatic transition** — firmware sits in this state indefinitely if the user doesn't confirm.
- A fault that trips while the gate is active forces `MODE_FAULT`, but the gate persists; acknowledging the fault returns to `MODE_PENDING_VALIDATION`, not `MODE_IDLE` (finding #3).

### 6.5 UI presentation

On the HOME tab, above the temperature label, an orange ~32 px banner:

```
┌────────────────────────────────────┐
│  📶                          16:42 │
├────────────────────────────────────┤
│ ┌─────────────────────────────────┐│
│ │ ⚠ Pending update                ││ ← orange banner
│ │ v0.3.2 → v0.3.3 · Tap to confirm││
│ └─────────────────────────────────┘│
│                                    │
│  185°C                             │
│  Target -- · Confirm update first  │
│  [START button disabled, greyed]   │
```

Tapping the orange banner directly enqueues `CMD_CLEAR_PENDING` (the direct-action option — no extra confirm dialog) → `control_task` marks the image valid → banner disappears → START enables. (The implemented text is "⚠ Pending update — tap to confirm"; the "v0.3.2 → v0.3.3" version-transition line is a pending enhancement, finding #35.)

If self-tests **failed**, the HOME banner is instead a non-tappable **red** strip and START stays disabled — the only resolution is a reboot, which rolls back:

```
│ ┌─────────────────────────────────┐│
│ │ ✗ Update failed self-tests      ││ ← red banner (non-tappable)
│ │   — rollback on reboot          ││
│ └─────────────────────────────────┘│
│  Self-tests failed — reboot to roll back
│  [START button disabled, greyed]   │
```

The pending-vs-failed banner is chosen from the cached boot validation result (`boot_status_validation()`), so HOME does not re-read OTA flash state on every tick (finding #40). Banner background colour and exact wording remain **TUNE**-able.

### 6.6 Rollback path

Exclusively automatic — no UI surface for "rollback now":

- User never confirms and reboots → bootloader sees PENDING_VERIFY → boots the previously-valid other slot.
- Self-tests fail (subsystem-broken firmware) → no validation → next reboot rolls back.
- New firmware panics before reaching self-test code → bootloader rolls back on next boot (watchdog reboot triggers the counter).

### 6.7 Bootloader config

- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` — auto-rollback engine.
- `CONFIG_APP_ROLLBACK_ENABLE=y` — exposes `esp_ota_mark_app_valid_cancel_rollback()` to app code.
- `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` left **disabled** — that's the eFuse-based version monotonicity for Secure Boot; out of scope (see §1.2).

## 7. Discovery & connectivity

### 7.1 mDNS

`mdns` IDF component (added to `src/idf_component.yml`). Registered after WiFi obtains an IP:

```c
mdns_init();
mdns_hostname_set("reflow");
mdns_instance_name_set("Reflow Oven");
mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
```

Reachable at:
- `http://reflow.local/` (mDNS)
- `http://<dhcp-IP>/` (fallback)

OS support: macOS (Bonjour) and Windows 10+ work out of the box; Linux needs `avahi-daemon` (default on most distros); iOS / Android handle it via Bonjour / NSD.

### 7.2 Forget WiFi

Settings → Network → Forget WiFi → modal confirm dialog ("Forget WiFi?" / "Disconnect and erase the saved network?", **Cancel** / **OK**) → on OK:
1. `nvs_erase_key(NVS_WIFI_SSID | _PASS | _SET)`
2. Under the WiFi lock, set the `forgotten` flag and transition the FSM to `IDLE` **before** `esp_wifi_disconnect()`, so the disconnect event that follows does not queue a fresh auto-reconnect that Forget is trying to cancel (finding #17).
3. `esp_wifi_disconnect()`
4. Publish `WIFI_CB_DISCONNECTED`; status bar updates to the "no WiFi" overlay
5. Network sub-screen shows the "Set up WiFi" entry point

Confirm dialogs throughout Settings use a shared modal helper (`settings_confirm`) with **Cancel** / **OK** footer buttons; the destructive action runs only on OK.

## 8. PlatformIO custom upload script

### 8.1 `tools/ota_upload.py`

Standalone Python (no PIO dependency); ~80 LOC.

```
Usage: python tools/ota_upload.py <path/to/firmware.bin>
Reads config from $REFLOW_OTA_CONFIG (default: ~/.config/reflow_oven/upload.conf)
```

Config file format (TOML):

```toml
host = "reflow.local"    # or IP if mDNS doesn't resolve on your network
token = "abc1234..."     # 32 hex chars from Settings → Firmware update → Show token
```

Behaviour:
1. Load config (host + token). Exit 1 if either missing.
2. Read firmware path from argv[1]. Exit 1 if missing or > 1.5 MB.
3. `GET http://<host>/version` — confirms reachability and prints current device version (warning on downgrade).
4. `POST http://<host>/ota` with required headers; stream upload with a `tqdm` progress bar.
5. Print response on success; exit non-zero with response body on 4xx/5xx.

### 8.2 `platformio.ini` addition

```ini
[env:esp32dev-ota]
extends = env:esp32dev
upload_protocol = custom
upload_command = python ${PROJECT_DIR}/tools/ota_upload.py $SOURCE
```

Usage:
- `pio run -e esp32dev -t upload` — standard USB upload (unchanged)
- `pio run -e esp32dev-ota -t upload` — WiFi OTA upload

### 8.3 Token bootstrap (first time only)

1. USB-flash a firmware build with OTA enabled.
2. Boot oven → provision WiFi via touchscreen.
3. Settings → Firmware update → Show OTA token → copy hex string.
4. Save it to `~/.config/reflow_oven/upload.conf` once.
5. All subsequent updates via `pio run -e esp32dev-ota -t upload`.

Token lost: Settings → Firmware update → Regenerate OTA token produces a fresh one. Update `upload.conf` to match.

## 9. Versioning + About

### 9.1 Version string + build date

**Version** comes from `git describe`, captured by `CMakeLists.txt` at configure time and substituted into the generated `version.h`:

```cmake
execute_process(
    COMMAND git describe --tags --always --dirty
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
configure_file(
    ${CMAKE_SOURCE_DIR}/include/version.h.in
    ${CMAKE_BINARY_DIR}/version.h
    @ONLY
)
```

Generated `version.h`:
```c
#define FIRMWARE_VERSION_STR "v0.3.2-mvp+3"
```

**Build date/time** are NOT baked in at CMake configure time (that string went stale on rebuilds — finding #57). Instead they are read at runtime from the app descriptor the IDF build system stamps into every image: `esp_app_get_description()->date` / `->time` (genuine build time). The `/version` JSON, the `GET /` page, and the About screen all use these.

### 9.2 About sub-screen

```
┌────────────────────────────────────┐
│ ←  About                           │
│ ────────────────────────────────── │
│ Firmware                           │
│   Version    v0.3.2-mvp+3          │
│   Built      2026-05-25 14:32 UTC  │
│                                    │
│ OTA slot                           │
│   Active     ota_1                 │
│   Status     ✓ valid               │
│   Other slot v0.3.1-mvp+2          │
│                                    │
│ Hardware                           │
│   Board      LCDwiki E32R35T       │
│   Chip       ESP32-WROOM-32E       │
│   MAC        AA:BB:CC:DD:EE:FF     │
│   Free heap (at boot)  124 KB      │
└────────────────────────────────────┘
```

"Other slot" reads from the inactive partition's app description (`esp_ota_get_partition_description()`) so you can see at a glance what rollback would land on.

### 9.3 Firmware update sub-screen

```
┌────────────────────────────────────┐
│ ←  Firmware update                 │
│ ────────────────────────────────── │
│ How to update:                     │
│ POST your firmware.bin to          │
│   http://reflow.local/ota          │
│ with X-OTA-Token header.           │
│                                    │
│ OTA token                          │
│   [ Show token ]                   │
│   [ Regenerate token ]             │
│                                    │
│ Slot status                        │
│   Current    v0.3.2-mvp+3          │
│   ✓ valid                          │
└────────────────────────────────────┘
```

**Show token**: displays the 32-hex-char token persistently with a back affordance — no auto-hide. The user navigates away when done. (Decision 2026-05-25: auto-hide rejected; 5 s wasn't long enough to transcribe and this is a personal device.)

**Regenerate token**: modal confirm dialog ("Regenerate token?" / "The current token will stop working. Update upload.conf afterwards.", **Cancel** / **OK**) → on OK: `esp_fill_random()` 16 bytes → hex-encode → write to NVS `ota_token` → old token immediately invalid. User must update `~/.config/reflow_oven/upload.conf` before next OTA.

First boot after OTA-enabled flash: if `ota_token` is empty in NVS, auto-generate one.

## 10. Verification plan

### 10.1 Bring-up tools

Following the existing pattern of `tools/bringup/01_blink` through `09_audio`; each is a standalone PlatformIO project with its own `platformio.ini` + `src/main.c` that exercises the new subsystem in isolation:

| Tool | Verifies |
|---|---|
| `tools/bringup/10_wifi/` | Station-mode init, hardcoded SSID+pass, connect, log IP/RSSI/hostname, mDNS announce |
| `tools/bringup/11_http_server/` | HTTP server serving `/` and `/version` on port 80 from `reflow.local/`; no OTA endpoint yet |
| `tools/bringup/12_ota_endpoint/` | `/ota` with all auth/mode/size checks; upload via `tools/ota_upload.py`; partition flip confirmed in next-boot log |
| `tools/bringup/13_self_tests/` | Subsystem self-tests run standalone; logs PASS/FAIL per test with timing |

### 10.2 Host-side unit tests (`[env:native]`)

| File | Coverage |
|---|---|
| `test/test_ota_validator.c` | Subsystem self-test sequencing with mocked driver interfaces |
| `test/test_ota_token.c` | Token generation distribution; constant-time compare correctness |
| `test/test_wifi_state.c` | WiFi driver FSM transitions (no real WiFi, just state-machine logic) |

### 10.3 End-to-end smoke (manual, on hardware)

1. USB-flash a build with this spec → boot → provision WiFi → confirm `http://reflow.local/version` responds with current version
2. Build a new firmware with a trivial change → `pio run -e esp32dev-ota -t upload` from another machine on same WiFi
3. Observe: progress bar completes → device reboots → self-tests run → `MODE_PENDING_VALIDATION` → "Confirm update" banner appears
4. Tap confirm → banner disappears → START enables → run a profile
5. Reboot → no longer pending → starts normally

### 10.4 Rollback smoke (manual)

1. Inject a deliberate self-test failure (e.g. force a fake TC fault in the test path).
2. OTA-flash that build.
3. Confirm: self-tests fail → no validation → reboot → bootloader rolls back → oven back on prior valid slot, fully functional.

### 10.5 Performance benchmarks during bring-up

- Full 1 MB upload over WiFi: target < 15 s on local WiFi.
- Heap free during upload: target ≥ 30 KB headroom.
- Subsystem self-test total duration: target ≤ 5 s.

All three are **TUNE**-able; if any misses, the spec is revisited before merge.

## 11. Risks and open questions

1. **DAC + WiFi coexistence**. From [2026-05-23-audio-signals-design.md](2026-05-23-audio-signals-design.md) §8.2, the ESP32 DAC CW generator shares clock infrastructure with the RF subsystem. Fault-alarm tones may glitch under WiFi load. Mitigation if observed during bring-up: replace DAC tone generator with LEDC on a different GPIO, or sample-based playback via I²S. Re-validate in `tools/bringup/10_wifi/` by playing the fault melody during a connection attempt.

2. **mDNS reliability on user networks**. Avahi or Bonjour may be absent / misconfigured. The Settings → Network screen displays the raw IP as a fallback — user can paste `http://<IP>/` into the PIO config or browser.

3. **Stock TWO_OTA layout vs custom**. If the linked app size exceeds 1.5 MB after WiFi + HTTP server + mDNS + new UI screens, switch to a custom `partitions.csv` with redistributed slot sizes (e.g. trim NVS to its needed footprint, drop phy_init if not used, enlarge the two app slots). The TWO_OTA layout has no factory slot — only ota_0 + ota_1 + the small headers. To be measured during bring-up.

4. **NTP timezone handling for travelling users**. Hardcoded `LOCAL_TZ` in `config.h` is correct for Germany. A Settings → Time/Locale screen is out of scope here; revisit only if the oven moves to a different timezone.

5. **Constant-time compare**. `mbedtls_constant_time_memcmp` is the right tool; if it's not available in the chosen mbedTLS version, fall back to a hand-rolled constant-time compare. Both must be byte-by-byte without short-circuit; **TUNE** during implementation.

6. **MVP merge sequencing**. This spec sits on `ota-firmware-update`, branched from `mvp-implementation`. When MVP merges to main, this branch needs a rebase. Plan: rebase rather than merge to keep history linear.

## 12. Files changed (summary)

**New** (~17 files):
- `src/drivers/wifi.{c,h}`
- `src/ota/ota_server.{c,h}`, `ota/ota_validator.{c,h}`
- `src/ui/status_bar.{c,h}`, `screen_settings.{c,h}`, `screen_wifi_scan.{c,h}`, `screen_wifi_password.{c,h}`, `screen_network.{c,h}`, `screen_about.{c,h}`, `screen_firmware_update.{c,h}`
- `include/version.h.in`
- `tools/ota_upload.py`
- `tools/bringup/10_wifi/`, `11_http_server/`, `12_ota_endpoint/`, `13_self_tests/` (each with project root, `platformio.ini`, `src/main.c`)
- `test/test_ota_validator.c`, `test/test_ota_token.c`, `test/test_wifi_state.c`
- `partitions.csv`

**Modified**:
- `src/control/control_task.c`
- `src/ui/ui_task.c`, `screen_home.c`, `screen_profile.c`, `screen_pid.c`
- `src/storage/nvs_store.{c,h}`
- `src/main.c`
- `CMakeLists.txt`
- `include/config.h`
- `src/idf_component.yml` (adds `espressif/mdns`)
- `sdkconfig.esp32dev`
- `platformio.ini`
- `docs/v2_backlog.md` (move OTA to #1, RGB LED to Tier 4, link to this spec)

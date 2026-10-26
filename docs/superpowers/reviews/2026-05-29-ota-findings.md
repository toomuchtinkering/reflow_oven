# OTA branch — independent review findings

Date: 2026-05-29
Branch: `ota-firmware-update` (compared to `main`)
Spec: [`docs/superpowers/specs/2026-05-25-ota-firmware-update-design.md`](../specs/2026-05-25-ota-firmware-update-design.md)
Plan: [`docs/superpowers/plans/2026-05-25-ota-firmware-update.md`](../plans/2026-05-25-ota-firmware-update.md)

Two independent reviewers (Opus 4.7), one for code quality, one for spec compliance, walked the branch in parallel. Findings below are deduplicated; tags `[CQ #N]` and `[SP A.x.y]` cross-reference the raw reports. Items where both reviewers independently flagged the same thing carry both tags.

> **Resolution status — 2026-05-29 (all 59 addressed).** Fixed in six waves; each wave was build- + test-verified (`pio test -e native` = 90 cases, `pio run -e esp32dev` = SUCCESS) and passed an independent spec + code-quality review.
> - **Wave 1 (safety):** 1–7. #1 closed *in code* (anchor the running slot valid when its OTA state is UNDEFINED) instead of adding a factory partition — respects the no-factory layout (§11.3) and the flash budget (slot ~94% full). The validation gate was decoupled from `oven_mode_t` into `sm_validation_t` so faults can't defeat rollback.
> - **Wave 2 (OTA upload):** 8, 9, 11, 28.
> - **Wave 3 (UX):** 16, 17, 22 (already correct in code — documented), 31, 33, 34.
> - **Wave 4 (spec drift):** 44–49 reconciled by updating the spec/plan to the deliberately-chosen code; #48 kept the opacity-graded icon and removed the dead wedge code (#39).
> - **Wave 5 (tier 3/5):** 18–25, 29, 30, 32, 35–43 (#40 done in wave 1; spec §4.2 updated for the nav stack).
> - **Wave 6 (tier 2/4/6):** 10, 12, 13, 14, 15, 26, 27, 50–59. #13 replaced the unsynchronised mode read with a control-task reservation handshake; #57 switched the build date to `esp_app_get_description()` (build-time).
>
> **Deviations / residual items:** #51 is functional-only (true constant-time timing can't be asserted in a portable unit test — documented in the test). #58 (Secure Boot) remains intentionally declined (§1.2) and is now documented as a residual risk in §5.6. One **pre-existing, out-of-findings** issue was surfaced during review: the WiFi *scan* sub-screen leaks across repeated Change-network cycles (a safe fix needs a callback-after-free guard) — noted in §4.2 for a future hygiene pass. A build warning "Flash size: Expected 4MB, found 2MB" comes from the board manifest, not these changes — set `board_upload.flash_size = 4MB` if it ever blocks a real flash.

Severity vocabulary:
- **Safety contract** = breaks the rollback / pending-validation property the OTA design was built around
- **Likely runtime bug** = will trigger on real hardware with high probability
- **Race / concurrency** = correctness depends on timing
- **UX bug** = visible behaviour wrong, recoverable
- **Drift** = code does not match spec/plan but isn't necessarily wrong; decide whether to update spec or update code

---

## Tier 1 — Safety contract bugs (defeat the rollback story)

1. **No factory partition → brick on first failed OTA.** `partitions.csv` declares only ota_0/ota_1. If the first OTA image's self-tests fail, the bootloader rolls back into an empty slot. — [partitions.csv](../../../partitions.csv) [CQ #15]
2. **Failed self-tests do NOT block START.** `boot_status_set_validator_pending(true)` is only called when self-tests pass, so on failure control_task initial mode stays IDLE → START stays enabled. Only the red banner shows. — [src/ui/screen_home.c:527](../../../src/ui/screen_home.c#L527), [src/main.c:77-91](../../../src/main.c#L77-L91) [SP A.9.5]
3. **PENDING_VALIDATION leaks through fault path.** `sm_trip_fault` overwrites `mode = MODE_FAULT`. After fault-ack you land in IDLE, can START, and rollback safety is silently gone. — [src/control/reflow_sm.c:42](../../../src/control/reflow_sm.c#L42) [CQ #43]
4. **`CMD_CLEAR_PENDING` / `ota_validator_mark_valid()` not bound to the actual state transition.** Rollback can be cancelled even when SM has already left PENDING via fault. Move `mark_valid` to the control task's CLEAR_PENDING handler, gated on actually being in PENDING state. — [src/control/control_task.c:125-127](../../../src/control/control_task.c#L125-L127) [CQ #44]
5. **`boot_status_*` flags not `volatile`/atomic.** Written by main, read from LVGL task, no memory barrier. — [src/ota/boot_status.c:1-11](../../../src/ota/boot_status.c#L1-L11) [CQ #16]
6. **No audible cue on self-test failure** — only `ESP_LOGE`. Safety device should sound the fault alarm. — [src/main.c:70-91](../../../src/main.c#L70-L91) [CQ #17]

## Tier 2 — OTA server bugs that will hit on real uploads

7. **Stack overflow in httpd worker.** `char buf[OTA_UPLOAD_CHUNK_SIZE]` (4 KB) on a 4 KB worker stack. Likely guru-meditation on first POST. Same bug in bringup 12. — [src/ota/ota_server.c:113](../../../src/ota/ota_server.c#L113), [tools/bringup/12_ota_endpoint/src/main.c](../../../tools/bringup/12_ota_endpoint/src/main.c) [CQ #8]
8. **`recv` timeout-budget logic broken.** `last_t` only updates on timeout, not on successful read. After 25 s of successful streaming, one timeout subtracts the entire elapsed → budget goes negative instantly. — [src/ota/ota_server.c:115-126](../../../src/ota/ota_server.c#L115-L126) [CQ #30]
9. **Uninitialised `expected[]` on stack** in token compare path. Should be `= {0}`. — [src/ota/ota_server.c:99-102](../../../src/ota/ota_server.c#L99-L102) [CQ #7]
10. **Content-Type prefix-only match.** `application/octet-stream-evil` passes. Verify the next byte is `'\0'`, `' '`, or `';'`. — [src/ota/ota_server.c:89-93](../../../src/ota/ota_server.c#L89-L93) [CQ #29]
11. **`esp_restart()` from handler context, only 1 s after `httpd_resp_send`.** Client typically sees "connection reset" instead of the JSON. Schedule restart from a one-shot timer / detached task. — [src/ota/ota_server.c:144-147](../../../src/ota/ota_server.c#L144-L147) [CQ #22]
12. **409 response body is `text/html` plain string**, not JSON like other responses. — [src/ota/ota_server.c:77-81](../../../src/ota/ota_server.c#L77-L81) [SP A.2.1]
13. **`control_get_mode()` unsynchronized read** + TOCTOU between mode-check and `esp_ota_begin`. Replace with a CMD_BEGIN_OTA reservation in the control queue. — [src/control/control_task.c:254](../../../src/control/control_task.c#L254), [src/ota/ota_server.c:78](../../../src/ota/ota_server.c#L78) [CQ #10]
14. **`httpd_resp_send` return value ignored** before `esp_restart`. — [src/ota/ota_server.c:144](../../../src/ota/ota_server.c#L144) [CQ #28]
15. **No guard that `next_update_partition != running`.** Returns cryptic generic 500 instead of clear error. — [src/ota/ota_server.c:105-106](../../../src/ota/ota_server.c#L105-L106) [CQ #3]

## Tier 3 — WiFi driver concurrency / lifecycle

16. **WiFi state IDLE-trap after retry exhaustion.** After 5+10+20 s burst, FSM force-sets IDLE and never auto-reconnects. Spec implies infinite retry while creds known. Long AP outage = permanently disconnected. — [src/drivers/wifi.c:120-133](../../../src/drivers/wifi.c#L120-L133) [SP A.1.1]
17. **`wifi_forget` race with retry timer / disconnect event.** `esp_wifi_disconnect()` is called BEFORE `s_forgotten=true`, so the disconnect event handler queues a retry that the forget path is about to cancel. Set `s_forgotten=true` under LOCK first. — [src/drivers/wifi.c:265-277](../../../src/drivers/wifi.c#L265-L277) [CQ #6]
18. **`wifi_connect_async` releases lock between `esp_wifi_disconnect()` and `esp_wifi_connect()`.** Disconnect event can queue a retry, then connect runs twice, `s_retry_count` accounting drifts. — [src/drivers/wifi.c:258-262](../../../src/drivers/wifi.c#L258-L262) [CQ #19]
19. **mDNS/SNTP one-time init has TOCTOU window** when lock is dropped and re-acquired around `mdns_init`/`esp_sntp_init`. — [src/drivers/wifi.c:162-180](../../../src/drivers/wifi.c#L162-L180) [CQ #4, #5]
20. **mDNS/SNTP not re-initialised after `wifi_forget` + new SSID.** `advertised` flag never clears. — [src/drivers/wifi.c:162-171](../../../src/drivers/wifi.c#L162-L171) [CQ #20]
21. **`wifi_get_rssi` returns 0 on error** → status bar paints "full signal" instead of disconnected. Return `INT_MIN` on error. — [src/drivers/wifi.c:279-283](../../../src/drivers/wifi.c#L279-L283), [src/ui/status_bar.c:51-56](../../../src/ui/status_bar.c#L51-L56) [CQ #34]
22. **WiFi credentials written to NVS BEFORE connection succeeds** → wrong password gets persisted. Also stored plaintext (NVS not encrypted). Persist only on `WIFI_CB_CONNECTED`. — [src/drivers/wifi.c:158-160](../../../src/drivers/wifi.c#L158-L160), [src/storage/nvs_store.c:104-112](../../../src/storage/nvs_store.c#L104-L112) [CQ #18]
23. **mDNS hostname collision silently fails.** Two ovens on same LAN both think they own `reflow.local`. Log return codes; consider MAC suffix. — [src/drivers/wifi.c:166-169](../../../src/drivers/wifi.c#L166-L169) [CQ #50]
24. **`wifi_scan_start` second call wedges FSM.** Second call returns false but FSM already advanced to SCANNING; locks out future scans. — [src/drivers/wifi.c:301-307](../../../src/drivers/wifi.c#L301-L307) [CQ #47]
25. **WiFi event-task stack hosts ~1.5 KB scan arrays.** Tight today, will panic later. Move to file-static or heap. — [src/drivers/wifi.c:138-140](../../../src/drivers/wifi.c#L138-L140) [CQ #27]

## Tier 4 — Control/SM correctness

26. **`s_curve` mutated by `profile_curve_generate` WITHOUT taking publisher mutex** while UI memcpys from `s_state->curve`. Torn curve array on profile-apply. — [src/control/control_task.c:85-101](../../../src/control/control_task.c#L85-L101), [src/ui/ui_task.c:39-46](../../../src/ui/ui_task.c#L39-L46) [CQ #9]
27. **PID integrator carries state across STOP→START.** `pid_reset` fires on `pid_active false→true` which works in practice, but worth an explicit reset on mode→IDLE for documentation. — [src/control/control_task.c:225-228](../../../src/control/control_task.c#L225-L228) [CQ #31]

## Tier 5 — UI lifecycle bugs

28. **Password screen lifecycle race.** `cleanup_screen()` deletes `s_password_sub` while a stale `lv_async_call` from a previous attempt is still in flight; can delete a NEW screen instance. Add a session-id token. — [src/ui/screen_wifi_password.c:29-38](../../../src/ui/screen_wifi_password.c#L29-L38) [CQ #13]
29. **Password screen "back" navigates to settings root, not scan list.** Single-level `s_current_sub` instead of a real navigation stack. — [src/ui/screen_wifi_password.c:20-26](../../../src/ui/screen_wifi_password.c#L20-L26) [CQ #12]
30. **Password screen back-press leaks WiFi subscription** if user backs out without success/fail (cleanup only runs on those two events). — [src/ui/screen_wifi_password.c:52](../../../src/ui/screen_wifi_password.c#L52) [CQ #26]
31. **Network screen never refreshes.** `refresh()` runs once at create-time, not wired into UI tick. After successful connect, screen still says "Not configured". — [src/ui/screen_network.c:14-29](../../../src/ui/screen_network.c#L14-L29) [SP A.1.5]
32. **Network screen always shows Change/Forget** even when no creds exist. Spec §4.3 wants `[ Set up WiFi ]` only when unprovisioned. — [src/ui/screen_network.c:31-57](../../../src/ui/screen_network.c#L31-L57) [SP A.1.3]
33. **Forget WiFi: no confirm dialog** — one-tap immediate erase. Spec §7.2 mandates confirm. — [src/ui/screen_network.c:12](../../../src/ui/screen_network.c#L12) [SP A.1.4]
34. **Regenerate token: no confirm dialog** — one-tap immediate regen. Spec §9.3 mandates confirm. — [src/ui/screen_firmware_update.c:18-26](../../../src/ui/screen_firmware_update.c#L18-L26) [SP A.6.1]
35. **Pending banner missing version-transition text** ("v0.3.2 → v0.3.3"). — [src/ui/screen_home.c](../../../src/ui/screen_home.c) [SP A.9.4]
36. **About screen labels stale after first visit.** Sub-screen lazy-created once and cached; free-heap shows boot value forever. Either refresh on focus or relabel "Free heap (at boot)". — [src/ui/screen_about.c:18-58](../../../src/ui/screen_about.c#L18-L58) [CQ #58]
37. **`screen_home_set_curve` re-runs every 250 ms** despite being expensive. Drive from `profile_applied_seq` instead. — [src/ui/ui_task.c:53-54](../../../src/ui/ui_task.c#L53-L54), [src/ui/screen_home.c:301-405](../../../src/ui/screen_home.c#L301-L405) [CQ #32]
38. **Status bar clock uses `tm_year > 70` sentinel** instead of SNTP sync status; momentary wrong time during sync. — [src/ui/status_bar.c:85-91](../../../src/ui/status_bar.c#L85-L91) [CQ #33]
39. **`wifi_bars_for_rssi` returns same glyph in every branch** — dead code; opacity-only is the actual mechanism. — [src/ui/status_bar.c:45-50](../../../src/ui/status_bar.c#L45-L50) [CQ #23, SP A.1.7]
40. **`screen_home` re-reads OTA partition every 250 ms** via `ota_validator_pending()` → flash read. Cache at boot in `boot_status`. — [src/ui/screen_home.c:527](../../../src/ui/screen_home.c#L527) [CQ #46]
41. **`screen_pid` saved-toast timer leaks** if user re-saves before the 1.5 s fade. Track + cancel in-flight timer. — [src/ui/screen_pid.c:44-51](../../../src/ui/screen_pid.c#L44-L51) [CQ #40]
42. **Scan results race.** `s_pending_results` written from WiFi event task, read on LVGL task without sequencing. Heap-alloc + pass via `lv_async_call` arg, or double-buffer. — [src/ui/screen_wifi_scan.c:14-17](../../../src/ui/screen_wifi_scan.c#L14-L17) [CQ #25]
43. **`wifi_scan_start` doesn't clear stale `s_pending_results`** before issuing scan; ghost APs possible. — [src/drivers/wifi.c:301-307](../../../src/drivers/wifi.c#L301-L307) [CQ #24]

## Tier 6 — Spec drift / tooling (decide: update spec or update code)

44. **Partition slot 0x170000 vs spec/plan 0x160000** — drift introduced by final-review fix. Update spec to match, or shrink back. — [partitions.csv](../../../partitions.csv) [SP A.5.3]
45. **`OTA_SELF_TEST_TIMEOUT_MS = 2000` vs spec 1000 ms.** — [include/config.h:194-195](../../../include/config.h#L194-L195) [SP A.3.4]
46. **SSR readback dropped** (spec §6.3 wanted GPIO read-back). Plan documents the relaxation; spec was not updated. — [src/ota/ota_validator.c:36-41](../../../src/ota/ota_validator.c#L36-L41) [SP A.3.2]
47. **Touch XPT2046 ID-register check dropped** (spec §6.3). Plan documents the relaxation; spec was not updated. — [src/ota/ota_validator.c:49-52](../../../src/ota/ota_validator.c#L49-L52) [SP A.3.3]
48. **RSSI thresholds: 4 distinct icons in spec, opacity-graded single icon in code.** — [src/ui/status_bar.c](../../../src/ui/status_bar.c) [SP A.1.7]
49. **`/version` JSON has extra `"built"` field** not in spec. Benign. — [src/ota/ota_server.c:52-57](../../../src/ota/ota_server.c#L52-L57) [SP A.2.7]
50. **`test_wifi_state.c` uses local `wifi_event_t` enum re-declaration** of the same shape. Tests pass but a future driver-enum reorder would silently break the contract. — [test/test_wifi_state/test_wifi_state.c:12-22](../../../test/test_wifi_state/test_wifi_state.c#L12-L22) [SP A.11.1]
51. **`test_ota_token` doesn't actually assert constant-time property** — just functional equality. [SP A.11.2]
52. **Settings icons differ from spec sketch** (e.g. `LV_SYMBOL_AUDIO` vs `🔇`). [SP A.8.2]
53. **mDNS instance name not set in bringup 12.** [SP A.4.2]
54. **`ota_upload.py` "downgrade warn" only prints version**, never compares. — [tools/ota_upload.py](../../../tools/ota_upload.py) [SP A.5.4]
55. **`MAX_BIN = 0x170000` duplicated** between Python and partitions.csv. Read from `/version` instead. — [tools/ota_upload.py:30](../../../tools/ota_upload.py#L30) [CQ #55]
56. **`ota_upload.py` 120 s blanket timeout, no progress recovery** on stalled writes. — [tools/ota_upload.py:73](../../../tools/ota_upload.py#L73) [CQ #56]
57. **`BUILD_DATE_STR` is CMake-configure-time, not build-time** — stale on rebuilds. — [CMakeLists.txt:16](../../../CMakeLists.txt#L16) [CQ #36]
58. **No signed-image enforcement.** `CONFIG_SECURE_BOOT`/`CONFIG_SECURE_SIGNED_APPS` not set. Documented as residual risk in spec, but worth a build-time check. [CQ #2]
59. **`audio_play_tone` uses `ESP_ERROR_CHECK` on every DAC call** — DAC quirk can panic the device. Log + skip instead. — [src/drivers/audio.c:54-78](../../../src/drivers/audio.c#L54-L78) [CQ #67]

---

## Suggested fix waves

**Wave 1 — safety contract (one sitting):** items 1, 2, 3, 4, 7. Together they prevent the device bricking on first failed OTA and prevent the rollback safety from being silently defeated.

**Wave 2 — OTA upload actually works:** items 8, 9, 11, 28. Without these the OTA endpoint will likely crash or stall on the first real upload.

**Wave 3 — visible UX bugs from manual testing:** items 16, 17, 22, 31, 33, 34. These are the most likely sources of the bugs the user observed during smoke testing.

**Wave 4 — spec drift housekeeping:** items 44-49 (decide each: update spec, or fix code).

**Wave 5 — long-tail concurrency hardening:** remaining tier 3 / 5 items.

## Process notes

- Code-quality reviewer reported ~80 raw findings (some self-reclassified to "no finding"); ~60 stood after dedup.
- Spec-compliance reviewer reported ~50 spec items walked; ~20 surfaced drift or partial implementation.
- High overlap on UI lifecycle and confirm-dialog issues (both reviewers found independently).
- Tasks 30 and 31 (manual hardware smoke tests) are deferred to hardware availability and were NOT in scope of these reviews.

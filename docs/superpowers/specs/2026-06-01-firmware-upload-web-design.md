# Firmware upload from the web UI — design

**Status**: design — **DEFERRED, not implemented on this branch** (confirmed 2026-06-02). The PID-tuning web UI shipped without it; firmware updates continue via the CLI (`tools/ota_upload.py`). Browser-based upload is a future item — pick this spec up then.
**Branch**: `pid-tuning-web-ui`
**Builds on**: the existing `POST /ota` + `GET /version` ([2026-05-25-ota-firmware-update-design.md](2026-05-25-ota-firmware-update-design.md)) and the embedded tuning SPA ([2026-05-31-pid-tuning-web-ui-design.md](2026-05-31-pid-tuning-web-ui-design.md)).

## 1. Scope & goal

Add a firmware-upload UI to the embedded tuning SPA so an operator can flash a new `firmware.bin` from a browser on the LAN — with a file picker / drag-drop, upload progress, and reboot/version confirmation — instead of only the CLI (`tools/ota_upload.py`). It is a **pure client** of the existing `POST /ota`; no device endpoint, handler, or contract changes.

**In scope**
- A "Firmware" card on the tuning page (served at `/`): current version, drag-drop + file-pick, Flash with confirm, upload-progress bar, reboot/version confirmation, mapped errors.
- Simulator (`tools/web_sim.py`) mock of `/ota` + `/version` for local preview.

**Out of scope (declined / deferred)**
- Any change to the `/ota` or `/version` handlers, token auth, or the idle-reservation handshake — the §1.5 OTA invariant is preserved precisely *because* nothing server-side changes.
- Rollback/slot management, changelogs, signature display beyond what `/version` already returns.
- Auth beyond the existing `X-OTA-Token`; no new persistence.

## 2. Device contract (existing — reference only, unchanged)

- **`POST /ota`** — headers `X-OTA-Token` + `Content-Type: application/octet-stream`; body = raw `firmware.bin`. Success: `200 {"status":"rebooting","new_version":"vX"}`, then the device reboots ~1 s later. Errors: `400` (bad/missing length or content-type), `401` (token), `409` (oven not idle), `413` (larger than the inactive slot), `500` (internal). (`/ota` itself has no `503` path — that code is only on `/stop` and `/suggest-gains`.)
- **`GET /version`** — `{version, slot, status, built, max_size}`; `status` ∈ {valid, pending, unknown}; `max_size` is the inactive-slot size in bytes.

## 3. UI — `src/web/tuning.html` card + `src/web/app.js`

A card in the right rail:
- Header `FIRMWARE`; a **current-version** line from `GET /version` (e.g. "running v1.4.0 · valid"), fetched on load.
- A dashed **drop zone that is also click-to-pick** (hidden `<input type=file accept=".bin">`). On select: show filename + size; if `size > max_size` (from `/version`) show a pre-warning (the device would `413`).
- A **Flash** button, a persistent **status line**, and a **progress bar** (shown only during upload).

**States**: `idle → file-selected → uploading(N%) → rebooting → updated(vX)` or `error(reason)`.

**Flow**
1. Select or drop a `.bin`.
2. **Flash** → `confirm("This replaces the firmware and reboots the oven — continue?")` → `prompt` for the OTA token (same pattern as Suggest/Stop).
3. Upload via **`XMLHttpRequest`** (POST `/ota`, `X-OTA-Token` + `Content-Type: application/octet-stream`, body = the `File`); `xhr.upload.onprogress` drives the **% bar**. (`fetch` cannot report upload progress — hence XHR.) Pass the `File` object **directly** as the body — XHR streams it from disk; never `readAsArrayBuffer` a ~1.4 MB image into memory. **Pause the `/telemetry` poll for the duration** (upload + reboot wait): the device's httpd has a single worker that services requests serially, so a concurrent poll would queue behind the multi-second upload and the UI would flip to "connection lost" mid-flash. A "connection lost" state while `uploading`/`rebooting` is therefore expected and must not be surfaced as an error; pausing also keeps the shared socket budget clear.
4. On HTTP `200`: capture the pre-upload version and the reply's `new_version`, show "rebooting…", and **poll `GET /version`** every ~2 s. Fetch failures or non-200 while the device is down are treated as "still rebooting". Consider the update **confirmed** once `/version` (after a downtime) reports either `status: "pending"` (a freshly-booted, not-yet-validated OTA image) **or** a version different from the pre-upload one → "✓ updated to vX, confirm on the oven". `status: "pending"` is the **authoritative** signal (verified: a freshly-OTA'd image boots `PENDING_VERIFY` and is never auto-marked valid — it stays `pending` until confirmed at the touchscreen), and it correctly handles re-flashing the *same* version string. The version-change check is a best-effort fallback — a same-version reply must **not** be treated as a failure. After a ~60 s timeout → "couldn't confirm — reload to check".
5. **Idle-gating**: the Flash control is enabled only when telemetry `mode === "idle"`. When `mode === "pending_validation"` (e.g. right after a previous OTA, awaiting on-oven confirmation) the note reads "confirm the update on the oven first"; for an active run it reads "stop the run first". Both mirror the device, which grants OTA only when idle (otherwise `409`).

**Error mapping** (HTTP → message): `401` bad token · `409` oven busy · `413` image too large for the slot · `400` bad request · `5xx` device error · network error mid-upload (outside the expected reboot window) → "upload failed (connection)".

## 4. Simulator — `tools/web_sim.py`

- `GET /version` → `{version, slot:"app0", status:"valid", built, max_size}`; the reported version **bumps on a successful upload**.
- `POST /ota` → accept any non-empty token (dev); `409` if a run is active (`mode != idle`); otherwise read the body, reply `200 {"status":"rebooting","new_version":<vNext>}`, then enter a ~3 s "reboot window" during which `/version` returns `503`, after which `/version` reports `{version:<vNext>, status:"pending"}` (a freshly-booted OTA image awaiting validation). This exercises the full progress → rebooting → updated path including the `pending` detection (and the 409 path by starting a run first). Note: the simulator is multi-threaded (`ThreadingHTTPServer`), so it **cannot** reproduce the real device's single-worker request serialization — the telemetry-pause-during-upload behavior (§3 step 3) must be verified on hardware, not inferred from the sim passing.

## 5. Flash budget

The embedded SPA grows ~1–2 KB gzipped; current headroom is ~63 KB. No concern; re-measured at build (`build_web_asset.py` prints the size).

## 6. Error handling & edge cases

- Upload while not idle → device `409` (UI also pre-gates).
- Image larger than the slot → device `413` (UI may pre-warn via `max_size`).
- Connection drops mid-upload → `xhr.onerror` → "upload failed".
- Reboot not confirmed within ~60 s → advise reloading.
- Wrong file type → `accept=".bin"` filter only; no strict client validation. The device validates the image and rolls back a bad one (OTA design) — that remains the real safety net.
- During the upload the single-worker httpd serializes requests, so `/telemetry` is paused (§3 step 3); a transient "connection lost" while `uploading`/`rebooting` is expected, not an error.
- After a successful flash the device boots into `pending_validation` (awaiting on-oven confirmation), **not** `idle`; the card reflects this and points the operator to the touchscreen rather than showing "stop the run first".

## 7. Testing

- **Simulator**: `curl GET /version`; `POST /ota` with a dummy body → `200`; confirm `/version` bumps after the reboot window; `POST /ota` while a run is active → `409`.
- **Firmware**: builds; embedded asset regenerates; flash-budget re-checked.
- **Browser (manual, via the sim)**: pick a real `firmware.bin` → progress → rebooting → updated; exercise the `409` path (start a run first).
- No node unit tests (DOM/XHR-bound — consistent with the rest of the SPA, whose pure math lives in `fit.js`).

## 8. Safety

Confirm dialog before flashing; idle-gating; and the device's existing OTA validation + rollback remain the real safety net — this UI cannot bypass them. No CSRF machinery is added: the `X-OTA-Token` *custom request header* is itself effective CSRF protection (a cross-site page can't set it without a CORS preflight the device never answers, and the token is not an ambient cookie credential), and the `confirm()` + token `prompt()` double-gate accidental submits. **The §1.5 "OTA must keep working" invariant is untouched: no server-side handler, route, or auth changes.**

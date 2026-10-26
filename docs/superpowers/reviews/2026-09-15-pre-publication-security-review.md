# Pre-publication review: safety and security on a local network

Date: 2026-09-15. Reviewer: Claude (Fable 5.1), on request of the maintainer.
Scope: the minimal set of points to settle before the repository goes public.
Assumptions agreed before the review: git history will be squashed to one commit;
the oven lives on a trusted home LAN; licensing and third-party content are in
scope; the report is delivered as this file.

## Verdict

The firmware needs **no security fix** before publication. The network surface
was designed so that the LAN can only ever make the oven *safer* (stop) or
*change what the operator will confirm on the touchscreen* (gains, firmware);
it can never switch the heater on. What does need work before publishing is
**documentation and licensing**, not code.

## Must do before the repository is public

### 1. Third-party material and a LICENSE file (blocker)

- There is no `LICENSE` for the project's own code. Pick one (MIT or
  Apache-2.0 are the usual choices for ESP-IDF projects) and add it.
- `dev_ressources/reference_design/` (18 MB) is the complete STM32 source,
  CMSIS/HAL drivers and the Nextion `.HMI` file of the Instructables
  "DIY REFLOW OVEN" project. Its license could not be verified during the
  review (the page did not render); Instructables projects default to
  CC BY-NC-SA 4.0, which would forbid redistribution under a permissive
  license and requires attribution and share-alike. Unless the author has
  granted more, replace the directory with the link that is already in
  `reference_design.md`. The ST HAL/CMSIS files are BSD-3 and would be fine
  on their own, but they are only there because of the reference design.
- `dev_ressources/board_ressources/` (38 MB) redistributes LCDwiki manuals,
  Espressif datasheets and the technical reference manual, and component
  datasheets from third parties (Sitronix, Analog Devices, WCH, and others).
  Datasheets are copyrighted; the safe form is links plus the existing
  `board_reference.md` summary. `dev_ressources/peripherals/MAX31855.pdf` is
  the same case.
- `src/web/uplot.*` ships with `uplot.LICENSE` (MIT). Correct as is.

### 2. Document the security model in the README (blocker, documentation only)

Nothing in the README tells a stranger what the network interface assumes.
Add a short "Network and security model" section stating, at minimum:

- Everything is plain HTTP on port 80. The OTA token travels in clear text;
  anyone who can sniff the LAN, or read `~/.config/reflow_oven/upload.conf`,
  can flash arbitrary firmware onto a device that switches mains and heats
  to 250 C. Regenerate the token from the Firmware Update screen if it may
  have leaked.
- `/telemetry`, `/profile`, `/version` and the tuning page are unauthenticated.
  They are read-only, but they expose live temperatures, PID gains, heap and
  crash diagnostics to anyone on the network.
- No firmware signing, no secure boot, no flash or NVS encryption. The WiFi
  password and OTA token are stored in plain NVS; physical access to the
  board reads them out. (Spec §1.2 already records this as accepted risk.)
- Intended for a trusted home LAN only. Never port-forward the device or
  place it on a guest or shared network. The heater can only be started from
  the touchscreen; STOP, gain suggestions (touch-confirmed) and OTA (idle
  oven only) are the only network-reachable actions.

### 3. Squash with a public author identity

The history you are squashing carries a GitLab noreply address on 61 commits
that names both the maintainer and the internal NAS host. The squash commit
takes its author from the git config at that moment, so set a public identity
before squashing and check the result with `git log --format='%an <%ae>'`.
Docs also carried local home-directory paths in two plans; harmless.

### 4. Strip EXIF from the oven photos (recommended)

The eight JPEGs in `dev_ressources/oven/` carry EXIF with the phone model
(Samsung SM-S901B) and its firmware string. No GPS tags were found. Strip
metadata (`exiftool -all=` or re-export) before publishing. They are also
66 MB of a roughly 120 MB tree; downscaling is optional.

## Verified and found sound (no action)

- **Heater cannot be started from the network.** Of all queue commands,
  `src/ota/ota_server.c` only posts STOP, SUGGEST_PID and the OTA reservation.
  START, step test and drying originate solely from touchscreen handlers.
  A suggested gain set is validated by `pid_validate` and then waits for a
  tap on the PID tuning screen; it is never applied from the request.
- **Token handling.** 128 bits from `esp_fill_random`, hex-encoded, compared
  with `mbedtls_ct_memcmp`; over-long headers are rejected as missing.
  Brute force over the LAN is not a realistic threat at this size.
- **CSRF and DNS rebinding.** Every state-changing route needs the custom
  `X-OTA-Token` header, so a hostile web page cannot fire them cross-origin
  (the preflight is never answered). Rebinding only reaches the read-only
  routes.
- **OTA gating.** Firmware is written only after `control_begin_ota` grants
  an idle-mode reservation decided on the control task, so a run cannot start
  mid-upload; the reservation is released on every failure path. A new image
  must pass on-device self-tests and a touchscreen confirmation before the
  bootloader stops rolling back, and the USB-flashed base image is anchored
  valid so there is always a slot to fall back to.
- **Request parsing.** `/suggest-gains` bounds the body to 191 bytes, reads it
  completely, and both it and `/ota` abort on a 30 s no-progress stall so a
  slow client cannot pin the single httpd worker. The OTA size is checked
  against the slot before the reservation.
- **WiFi.** Credentials are only persisted after a successful GOT_IP. With a
  non-empty password ESP-IDF defaults the auth threshold to WPA2, so an open
  evil-twin AP with the same SSID is not joined. Bluetooth is compiled out.
- **Secrets in the tree.** No real SSID, password, token or `upload.conf` is
  tracked or was ever committed; bring-up firmwares only ever carried the
  `YOUR_SSID_HERE` placeholders and a documented dummy token.
- **Safety loop.** Sensor fault, stale reading, over-temperature and
  electronics over-temperature each force the SSR off before anything else
  runs, the task watchdog panics after 2 s, a frozen UI reboots the device
  with the SSR driven low, and the shutdown handler drives the SSR low on any
  graceful restart. This matches the README's own description.

## Low-severity observations (fix when convenient)

1. **Self-XSS in the tuning page.** `src/web/app.js` builds archive rows with
   template strings and assigns `innerHTML`; `fw` and `rule` come from
   localStorage or an imported archive file. A crafted archive can run script
   in the page. The page stores no token (it prompts every time), so the
   impact is limited to the importing user. Escape the fields or use
   `textContent`.
2. **Socket exhaustion hides the web STOP.** `HTTPD_DEFAULT_CONFIG` allows 7
   open sockets with LRU purge off; a misbehaving client can occupy them all
   and make `/stop` unreachable. The touchscreen STOP is unaffected, which is
   the safety-relevant path. Enabling `lru_purge_enable` costs nothing.
3. **Content-Length edge case in `/ota`.** `int total = req->content_len`
   turns 4294967295 into -1, which `esp_ota_begin` reads as "size unknown";
   the empty transfer then fails in `esp_ota_end`. Harmless, and it needs a
   valid token, but a `> part->size` check on the unsigned value would make
   the intent explicit.
4. **`tools/web_sim.py` listens on all interfaces** (`0.0.0.0`) and, in
   proxy mode, relays the browser's token to the device. Bind `127.0.0.1` by
   default, or say in its docstring that it is LAN-visible.
5. **mDNS name collision** between two ovens is logged but not resolved
   (already recorded as CQ #50 in the 2026-05-29 OTA findings).

## What was not reviewed

Physical-access attacks (UART, JTAG, flash readout) and the mains wiring are
out of scope by the task definition; the README's safety warning already
covers the hardware side. Managed components (LVGL, esp_lcd, esp_lvgl_port)
were taken as trusted upstream code.

## Resolution (2026-09-15, branch `fix/pre-publication-findings`)

Everything above is implemented except the squash, which by decision happens
only when pushing to GitHub; GitLab keeps the full history, including the
removed files and the original photos.

| Item | Resolution |
|---|---|
| Must 1, license and third-party material | Licensing decided the same day: MIT for code (`LICENSE`), CERN-OHL-P-2.0 for the 3MF parts and schematic, CC BY 4.0 for docs, photos, plots and run logs (`LICENSES/`), mapped in the README. 100 third-party files are untracked but kept on disk and ignored: the reference design with its HMI file, the LCDwiki PDFs, the component datasheets, the MAX31855 PDF, and `thermal_fuse_example.png`, which turned out to be a screenshot of a shop product photo. The Markdown files next to them link the sources. The Instructables license is still unverified because the page is captcha-gated. |
| Must 2, README | "Network and security model" and "License" sections added. |
| Must 3, squash | Deferred to the GitHub push, by decision. |
| Must 4, photo metadata | Done, and the finding was understated. The original "no GPS" check searched for the literal text `GPS`, which an EXIF GPS directory never contains, so it proved nothing. A proper parse confirms there is no GPS directory. Each photo did carry the capture time, an image unique ID, the Samsung trailer's mobile country code (262, Germany) and a ~4 MB embedded Motion Photo video. The rebuild keeps only the primary image stream, verified pixel-identical with PIL, plus the orientation tag on the one rotated photo. The photos shrink from ~7 MB to ~3 MB each. |
| Low 1, archive XSS | `escapeHtml` in `src/web/html.js`, with node tests, applied to every run field in the archive rows. The asset bundler now fails the build if any module script is left un-inlined. |
| Low 2, socket exhaustion | `lru_purge_enable` set on the HTTP server. |
| Low 3, Content-Length cast | The size check now compares `size_t` before narrowing to `int`. |
| Low 4, simulator bind | `tools/web_sim.py` listens on `127.0.0.1` by default; `--bind` opts in to the network. |
| Low 5, mDNS collision | No change. The premise does not hold for this component version: espressif/mdns 1.11.1 renames the host on a probe conflict (`mangle_name`, so a second oven becomes `reflow-2.local`). Only the static `reflow.local` labels on the Network and Firmware update screens would be wrong on that second oven. |

Beyond the list: the AliExpress links in `peripherals.md` lost their
order-list tracking parameters, which showed they were copied from the
account's order history.

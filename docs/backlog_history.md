# Backlog history

Everything that has left the [roadmap](roadmap.md): what shipped, what was
solved another way, and what was decided against, each with the reason. The
reasons are kept so the same questions don't have to be argued again. A closed
item can be reopened with a new argument; open an issue for it.

Item numbers are never reused, and new items continue from #27. The roadmap was
called `v2_backlog.md` until October 2026, so older documents cite items as
"`v2_backlog` #N". The [number index](#number-index) at the end says where
every number went.

---

## Shipped

Items marked "—" never had a number; they are listed so the record is complete.

| # | Item | Shipped | Where to read more |
|---|------|---------|--------------------|
| — | Audio signals for stages, completion and faults | 2026-05 | [audio spec](superpowers/specs/2026-05-23-audio-signals-design.md) |
| — | Cool-down rate in the reflow profile | 2026-05 | profile storage format v1 → v2 |
| 15 | Home curve redrawn only when the profile changes | 2026-05 | `src/ui/ui_task.c`, see below |
| 7 | Built-in web page *(most of it)* | 2026-05 | [PID tuning web UI spec](superpowers/specs/2026-05-31-pid-tuning-web-ui-design.md), `src/web/`, see below |
| 1 | Firmware updates over WiFi (OTA) with rollback | 2026-06 | [OTA spec](superpowers/specs/2026-05-25-ota-firmware-update-design.md), `src/ota/` |
| 11a | Mute switch in Settings → Audio | 2026-06 | `src/ui/screen_audio.c` |
| 13 | Speaker and WiFi working at the same time | 2026-06 | [`tools/bringup/10_wifi/`](../tools/bringup/10_wifi/) plays a tone during and after connecting |
| — | Firmware upload from the browser | 2026-06 | [firmware upload spec](superpowers/specs/2026-06-01-firmware-upload-web-design.md) |
| — | About screen with a switch to the other firmware slot | 2026-06 | [About screen spec](superpowers/specs/2026-06-10-about-slot-switch-and-electronics-temp-design.md) |
| — | Feedforward in the PID (`k_hold`, `k_rate`) | 2026-07 | [feedforward spec](superpowers/specs/2026-07-19-feedforward-design.md) |
| — | Electronics over-temperature fault at 75 °C | 2026-08 | [About screen spec](superpowers/specs/2026-06-10-about-slot-switch-and-electronics-temp-design.md) |
| — | Fault diagnostics: crash record, UI watchdog, LVGL assert trap | 2026-09 | `src/diag/`, `include/ui_supervisor.h` |
| — | Filament drying mode | 2026-09 | [drying spec](superpowers/specs/2026-08-30-filament-drying-mode-design.md) |
| 19 | Tuning chart rebuilt on uPlot | 2026-09 | [uPlot spec](superpowers/specs/2026-09-03-uplot-tuning-chart-design.md); §10 lists the uPlot behaviours that cost the most debugging time |
| 20 | Electronics temperature during a long dry | 2026-09 | see below |
| 22 | Boot counter on the crash record | 2026-09 | `src/diag/crash_log.c`, see below |
| 23 | Drying documentation corrected to match the UI | 2026-09 | drying spec §8.1 and its implementation plan |
| 24 | DHCP hostname, so the router doesn't list the oven as `espressif` | 2026-09 | `esp_netif_set_hostname()` in `src/drivers/wifi.c` |
| 26 | `POST /ota` reports the version it wrote | 2026-09 | `src/ota/ota_server.c`, `tools/pio_gen_version.py`, see below |

### Notes on shipped items

**#7 shipped in a different shape than planned.** It was described as a status
page with start/stop and a profile preview. What exists is the PID tuning web
page: `GET /` serves the page, `GET /telemetry` the live data (reflow, step test
and drying), `GET /profile` the active profile, and `POST /stop` and
`POST /suggest-gains` the two actions. Status, stop and profile preview are
therefore covered. Starting a run from the browser was left out on purpose and
later decided against; see [Decided against](#decided-against).

**#15 had already been done when it was still listed as open.** The curve on the
Home screen is only rebuilt when the profile changes since May 2026 (finding #37
in the [OTA review](superpowers/reviews/2026-05-29-ota-findings.md)). The item
stayed on the backlog until the October 2026 cleanup noticed.

**#20: how warm the electronics get during a long dry.** The question was
whether a multi-hour dry could reach the 75 °C electronics limit
(`CJ_LIMIT_C`), measured at the thermocouple amplifier's cold junction. A 4-hour
ASA dry at 80 °C ([run log](../dev_ressources/oven/2026_09_05_dry_run_ASA.json))
reached 38.8 °C and was still rising by 0.9 °C an hour. Steady state was not
reached, because the electronics warm up more slowly than the run lasted. Two
curve fits give 46.2 °C and 42.1 °C as the final value, so the limit keeps at
least 29 °C of margin. For a 90 °C dry (nylon, PC) the extrapolation gives
45–55 °C, depending on room temperature. The question was never the exact final
value but whether 75 °C is close, and it is not. The limit stays a protective
cut-out rather than a working margin: if it trips, something has changed
(venting, fan, room temperature), and that is a fault to investigate. The result
is also in §11 of the drying spec.

**#22: the boot counter.** `/version` reports the last crash record with
`boots_ago`, so `1` means "the fault that caused this boot". A counter was
chosen over an endpoint that clears the record, because it makes the record
readable instead of hiding it.

**#26: why `POST /ota` reported the wrong version.** It answered with the
version being replaced instead of the one just written. Now it reads the
version from the slot it has just written and answers with
`{slot, previous_version, new_version}`. The root cause was in the build: the
version string fell back to a `git describe` that ESP-IDF runs only when the
build is first configured, so builds kept an old version and build date.
`tools/pio_gen_version.py` now writes `version.txt`, which ESP-IDF reads as the
project version, and forces a reconfigure whenever the version changes.

---

## Solved another way

### 3. Run log stored on the device · 4. Review screen for past runs

Both were planned on the device. Both are covered by the tuning web page
instead: the browser keeps the last 50 runs in its local storage, with export
and import as JSON, overlays any combination of them for comparison, and shows
metrics per run (overshoot, time above liquidus, how well a dry held its
temperature).

The device versions are not just redundant but unwanted. NVS is a single 24 KB
partition shared with the settings, and a 12-hour dry sampled every 2 s is
about 21,600 points. A review screen on the device would also need LVGL pool
memory that is not available (see [roadmap #21](roadmap.md#21-screens-that-can-be-destroyed-and-rebuilt-includes-the-former-14)).

**What this leaves out:** a run is only recorded while a browser has the page
open. A run nobody watches is not recorded, and a run opened partway through is
marked `joined_late`. Recording on the device was proposed as #25 and decided
against, so this is an accepted limitation: **if a run is not watched, it is not
recorded.**

### 14. Lifetime guard for the "saved" timers

Folded into [roadmap #21](roadmap.md#21-screens-that-can-be-destroyed-and-rebuilt-includes-the-former-14).
The crash it guards against can only happen once a screen can be destroyed,
which is what #21 introduces. Fixing it on its own would guard one label out of
about ten screen modules with the same pattern.

---

## Decided against

These were scope decisions, made in September 2026 unless marked otherwise.
The oven does what it is wanted for, and each of these buys less than the RAM,
flash, bench time or safety design it would cost.

| # | Item | Why not |
|---|------|---------|
| 7 | Start a run from the browser *(the remaining part of #7)* | The code would be small, mirroring `POST /stop`. The real work is the safety design for switching on a mains heater with nobody in the room: allowed only when the oven is idle, a confirmation step on the page, and a clear signal on the device. That is a lot of design for a convenience on a device you are standing next to anyway. |
| 8 | PID autotune (relay feedback, Åström–Hägglund) | What it was for is already covered: the web page's step test with an adjustable Ziegler–Nichols fit, `POST /suggest-gains`, and feedforward taking the steady-state load off the integrator. Two to three days of work against an evening of assisted manual tuning. |
| 9 | Dimming the backlight and switching it off when idle | The backlight pin is a plain on/off GPIO, so this would first need a PWM channel (LEDC). Nice to have, and nothing depends on it. |
| 10 | Emergency stop permanently in the status bar | STOP is already the action button on the Home tab, and one tap stops a reflow, a step test or a dry. This would only save the tap that switches to the Home tab. |
| 11 | Volume slider *(the mute switch shipped as 11a)* | The tone comes from the ESP32's DAC, and the FM8002E amplifier has no volume control of its own; its SHUTDOWN pin only switches it on and off. A real volume control means scaling the DAC amplitude plus a new NVS setting, for an oven that beeps a few times per run. |
| 16 | Status shown on the board's RGB LED | The LED sits inside the closed housing, where nobody can see it. |
| 17 | MQTT publishing for Home Assistant | Nothing here consumes MQTT, and the web page already shows the live status. An MQTT client would also need permanent RAM next to the web server and the WiFi stack, with a heap that already dips to 14.6 KB. |
| 18 | Importing and exporting profiles via SD card | The browser covers both uses: firmware upload and the run archive. The SD slot also shares its SPI bus with the display (the display runs at about 40 MHz, SD cards prefer 25 MHz or less), it was never tested during bring-up, and flash and RAM are both full. |
| 25 | Recording unattended runs on the device | About 32 bytes per run in a ring buffer in NVS would have fit, but it brought back the on-device run storage that #3 and #4 had moved away from. Its main use was evidence for #20, which was cheaper to get from one watched multi-hour run. Proposed and decided against on the same day. |
| — | Door switch | Needs wiring changes inside the oven. |
| — | Second, independent temperature sensor | Needs wiring changes inside the oven. |
| — | Signed firmware and Secure Boot | Decided against in May 2026. For a single-user device on a trusted home network, the update token is enough; see "Network and security model" in the [README](../README.md#network-and-security-model). Reconsider if the oven moves to a less trusted network. |

---

## Number index

| # | Where it went |
|---|---------------|
| 1 | shipped |
| 2 | open, [roadmap](roadmap.md) |
| 3 | solved another way (browser run archive) |
| 4 | solved another way (browser run archive) |
| 5 | open, [roadmap](roadmap.md), together with #6 |
| 6 | open, [roadmap](roadmap.md), together with #5 |
| 7 | mostly shipped; starting runs from the browser decided against |
| 8 | decided against |
| 9 | decided against |
| 10 | decided against |
| 11 | mute shipped as 11a; volume slider decided against |
| 12 | open, [roadmap](roadmap.md) |
| 13 | shipped |
| 14 | folded into #21 |
| 15 | shipped |
| 16 | decided against |
| 17 | decided against |
| 18 | decided against |
| 19 | shipped |
| 20 | shipped |
| 21 | open, [roadmap](roadmap.md) |
| 22 | shipped |
| 23 | shipped |
| 24 | shipped |
| 25 | decided against |
| 26 | shipped |

The next new item is **#27**.

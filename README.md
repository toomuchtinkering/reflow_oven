# Reflow Oven Controller

Firmware for a DIY solder-reflow oven: a converted mini convection oven driven
by an ESP32 with a resistive-touch LCD, a K-type thermocouple, and a
solid-state relay. Based on the
[DIY-REFLOW-OVEN Instructables tutorial](https://www.instructables.com/DIY-REFLOW-OVEN/)
(STM32 + Nextion reference design), retargeted to the all-in-one
[LCDwiki E32R35T](http://www.lcdwiki.com/E32R35T) display board.

> ⚠️ **Safety:** this project switches mains voltage and reaches 250 °C.
> Don't build or modify it unless you know what you're doing on both counts.
> The firmware's safety layer (over-temperature latch, sensor-staleness
> watchdog, task watchdog, SSR force-off on any fault) is a last line of
> defense, not a substitute for a thermal fuse and proper mains wiring.

## Hardware

| Part | Role |
|---|---|
| LCDwiki E32R35T (ESP32-WROOM-32E) | Controller + 3.5" 320×480 ST7796 touch LCD (XPT2046) |
| MAX31855 | K-type thermocouple amplifier, SPI (use an **ungrounded-junction** probe) |
| Solid-state relay on GPIO 32 | Heater switching, 2 s time-proportioning window |
| FM8002E (onboard) | Audio amp for stage/fault/completion signals |

Pin map and every tunable constant live in [include/config.h](include/config.h)
— nothing is hard-coded elsewhere.

## Features

- **Reflow profiles** on the touchscreen: heat-up rates, soak temp/time,
  reflow temp/time, persisted to NVS, rendered as a planned curve with a live
  temperature trace.
- **PID control** with editable gains, anti-windup, derivative low-pass, and
  feedforward terms for the holding duty and the commanded ramp rate.
- **Filament drying mode**: a 40–100 °C hold for up to 16 h, with presets,
  as a run mode of its own next to reflow.
- **Browser tuning dashboard** served by the oven: open-loop step test with a
  Ziegler–Nichols fit, a live chart, and a run archive. Suggested gains only
  take effect after a tap on the touchscreen.
- **Safety state machine**: over-temp, thermocouple fault/stale detection,
  fault latching with acknowledge, watchdog-supervised control task.
- **WiFi + OTA updates**: token-authed `POST /ota` endpoint with rollback —
  a new image must pass on-device self-tests and a tap-to-confirm before it
  is marked valid, otherwise the bootloader rolls back. The About screen can
  also boot the other firmware slot by hand.
- **Audio signals** for stage transitions, run completion, and faults.

## Network and security model

The oven is built for a **trusted home network**. Read this before connecting it
to anything else.

- **The heater cannot be started over the network.** Reflow runs, step tests
  and drying start only from the touchscreen. Over HTTP a client can stop a
  run, suggest PID gains that wait for confirmation on the touchscreen, and
  upload firmware while the oven is idle.
- **Plain HTTP, token in clear text.** `POST /ota`, `/stop` and
  `/suggest-gains` require the `X-OTA-Token` header, a random 128-bit token
  shown under Settings → Firmware update. It travels unencrypted, so anyone
  who can sniff the network, or read `~/.config/reflow_oven/upload.conf`, can
  flash arbitrary firmware onto a device that switches mains. Regenerate the
  token on that screen if it may have leaked.
- **The read-only endpoints are open.** `/`, `/telemetry`, `/profile` and
  `/version` need no token. They expose live temperatures, PID gains, the
  firmware version, heap statistics and crash diagnostics to every device on
  the network.
- **No firmware signing, secure boot or flash encryption.** The WiFi password
  and the OTA token are stored unencrypted in NVS, so physical access to the
  board reveals both.
- **Never port-forward the oven or put it on a guest or shared network.** For
  remote access, use a VPN into your home network instead.

## Repository layout

```
include/config.h     single source of truth for pins + tunables
src/control/         PID, reflow state machine, profile curve, control task
src/drivers/         thermocouple, SSR, display, touch, audio, WiFi
src/ui/              LVGL touch UI (screens, status bar, theme)
src/ota/             OTA server, boot validator, rollback handling
src/audio/           event detection + melodies
src/storage/         NVS schema + persistence
test/                host-side Unity tests (pure-logic modules)
tools/bringup/       standalone hardware bring-up firmwares (01–13)
tools/ota_upload.py  OTA flasher (reads ~/.config/reflow_oven/upload.conf)
docs/superpowers/    design specs, implementation plans, review notes
dev_ressources/      oven photos, CAD, electrical design, run logs; links to datasheets
```

## Building

The project uses [PlatformIO](https://platformio.org/) with the ESP-IDF
framework (components resolved via the IDF Component Manager).

```bash
pio run -e esp32dev              # build
pio run -e esp32dev -t upload    # first flash over serial
pio run -e esp32dev-ota -t upload  # subsequent flashes over WiFi (OTA)
```

## Testing

Pure-logic modules (PID, state machine, profile curve, frame decoding, …)
compile on the host and are covered by Unity tests:

```bash
pio test -e native
```

Hardware-facing code is verified with the staged bring-up firmwares (below)
and on-device self-tests after OTA.

## Hardware bring-up firmwares

[tools/bringup/](tools/bringup/) contains 13 standalone PlatformIO projects,
each exercising exactly one subsystem in commissioning order. Each is flashed
on its own (`pio run -t upload` inside the folder) and prints its results over
serial, so a hardware problem is isolated before the full firmware ever runs:

| # | Project | Verifies |
|---|---|---|
| 01 | `blink` | Toolchain, serial flashing, onboard RGB LED |
| 02 | `display` | ST7796 panel init, colors, orientation |
| 03 | `touch` | XPT2046 raw touch readout (shared SPI bus with LCD) |
| 04 | `lvgl_hello` | LVGL + display + touch integration |
| 05 | `thermocouple` | MAX31855 SPI frames, fault bits, plausibility |
| 06 | `ssr` | SSR input switching + pull-down levels (heater disconnected!) |
| 07 | `safety` | TC-fault → SSR force-off chain with latched fault (heater disconnected!) |
| 08 | `touch_cal` | 5-point touch calibration; prints the matrix for config.h |
| 09 | `audio` | FM8002E amp + DAC tones |
| 10 | `wifi` | WiFi connect + DAC coexistence |
| 11 | `http_server` | Minimal HTTP server on the board |
| 12 | `ota_endpoint` | Minimal OTA upload endpoint |
| 13 | `self_tests` | The OTA validator's 4 alive-check hooks (TC, SSR, display, touch) |

## Documentation

Design specs and implementation plans live in
[docs/superpowers/](docs/superpowers/), starting with the
[MVP design spec](docs/superpowers/specs/2026-05-15-reflow-oven-mvp-design.md).
What could come next, and in which order, is in the
[roadmap](docs/roadmap.md). Everything shipped, solved another way or decided
against is in [docs/backlog_history.md](docs/backlog_history.md), with the
reasons.

## License

Copyright © 2026 toomuchtinkering for the project's own material, under three
licenses depending on the kind of file:

| What | License |
|---|---|
| Firmware, tools, tests, build configuration, this README, and everything not listed below | MIT, see [LICENSE](LICENSE) |
| Hardware designs: the 3MF parts in `dev_ressources/oven/` and the schematic in `dev_ressources/electrical_design/` | CERN-OHL-P-2.0, see [LICENSES/CERN-OHL-P-2.0.txt](LICENSES/CERN-OHL-P-2.0.txt) |
| Documentation and media: `docs/`, the Markdown files, photos, plots and JSON run logs under `dev_ressources/` | CC BY 4.0, see [LICENSES/CC-BY-4.0.txt](LICENSES/CC-BY-4.0.txt) |

The schematic generator `generate_schematic.py` is code and stays MIT. Code
snippets inside `docs/` may also be used under MIT. Attribute CC BY material
as "toomuchtinkering, Reflow Oven Controller" with a link to this repository.

The vendored [uPlot](https://github.com/leeoniya/uPlot) chart library keeps
its own MIT license ([src/web/uplot.LICENSE](src/web/uplot.LICENSE)). Vendor
datasheets, the LCDwiki board documents and the Instructables reference design
are not redistributed; the Markdown files under `dev_ressources/` link to their
sources.

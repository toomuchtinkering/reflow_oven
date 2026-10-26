# Bring-up 12 — Minimal OTA endpoint

Edit `WIFI_SSID` and `WIFI_PASS` in `src/main.c`, then:

    ~/.platformio/penv/bin/pio run -e esp32dev -t upload && \
        ~/.platformio/penv/bin/pio device monitor

## Building a new firmware to upload

The bringup tool flashes itself first; the firmware you upload via the
`/ota` endpoint is some *other* image — typically the MVP/OTA-branch
main firmware. Build it from the project root without flashing:

    cd ../../..    # project root
    ~/.platformio/penv/bin/pio run -e esp32dev

The compiled image lands at:

    .pio/build/esp32dev/firmware.bin

This is the file you POST in the curl command below. To make a visible
change before uploading (so you can confirm the new image actually
booted), edit a log line in `src/main.c` (e.g. add `ESP_LOGI("app",
"OTA test build 42")`) before the `pio run` step. After OTA-flashing,
the monitor will show that log on reboot.

Quick variant for building a fresh bin from this same bringup project
(if you want to OTA-flash a slightly-modified bringup-12 over itself):

    ~/.platformio/penv/bin/pio run -e esp32dev    # no upload
    # produces .pio/build/esp32dev/firmware.bin in THIS bringup directory

## Upload via curl

From a PC on the same WiFi (path adjusts to wherever the bin actually
lives):

    curl -X POST -H "X-OTA-Token: abcdef0123456789abcdef0123456789" \
        --data-binary @../../../.pio/build/esp32dev/firmware.bin \
        http://reflow.local/ota

Expected response: `{"status":"rebooting"}` then the device reboots into the new image.

The hardcoded token in `src/main.c` is for bring-up only. Production uses a randomised token stored in NVS (Task 20).

Reverts to MVP firmware via:

    cd ../../..
    ~/.platformio/penv/bin/pio run -e esp32dev -t upload

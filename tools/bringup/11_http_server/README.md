# Bring-up 11 — HTTP server hello

Edit `WIFI_SSID` and `WIFI_PASS` in `src/main.c`, then:

    ~/.platformio/penv/bin/pio run -e esp32dev -t upload && \
        ~/.platformio/penv/bin/pio device monitor

Expected log:
- `got IP: 192.168.x.x`
- `HTTP server up; visit http://reflow.local/`

Verify from a PC on the same WiFi:

    curl http://reflow.local/

Expected response: `<!doctype html><h1>reflow oven bringup 11</h1>`

**Note:** ESP-IDF defaults to WPA2-PSK when a non-empty password is set. Open networks, WEP, or WPA-only APs are not supported.

Reverts to MVP firmware via:

    cd ../../..
    ~/.platformio/penv/bin/pio run -e esp32dev -t upload

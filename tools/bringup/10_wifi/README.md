# Bring-up 10 — WiFi + DAC coexistence

Edit `WIFI_SSID` and `WIFI_PASS` in `src/main.c`, then:

**Note:** ESP-IDF defaults to WPA2-PSK when a non-empty password is set. Open networks, WEP, or WPA-only APs are not supported by this bringup tool — connection will silently time out.

    ~/.platformio/penv/bin/pio run -e esp32dev -t upload && \
        ~/.platformio/penv/bin/pio device monitor

Expected log:
- `DAC coexistence smoke — playing 4kHz tone during connect`
- `got IP: 192.168.x.x`
- `DAC coexistence smoke — playing 4kHz tone post-connect`
- `mDNS advertised: reflow.local`
- `RSSI: -XX dBm, AP: <ssid>` every 5 s

**Audible check**: both tones should be steady 4 kHz. If you hear glitching, popping, or frequency drift during the *post-connect* tone, the DAC + WiFi clock contention risk from the audio spec §8.2 is real on this hardware — note for the OTA spec §11.1 risk mitigation.

Reverts to MVP firmware via:

    cd ../../..
    ~/.platformio/penv/bin/pio run -e esp32dev -t upload

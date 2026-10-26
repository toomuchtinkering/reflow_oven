# Bring-up 13 — Standalone self-test runner

Builds + flashes the validator's 4 alive-check hooks (TC, SSR, display, touch)
in isolation. Useful for tuning per-test timing budgets against the spec.

    ~/.platformio/penv/bin/pio run -e esp32dev -t upload && \
        ~/.platformio/penv/bin/pio device monitor

Expected log:

    TC      : PASS (~100 ms)
    SSR     : PASS (~1 ms)
    Display : PASS (~1 ms)
    Touch   : PASS (~1 ms)
    TOTAL   : ~150 ms

The TC hook reads 3 samples 50 ms apart, so ~150 ms is normal.

Spec budgets (from `include/config.h`):
- `OTA_SELF_TEST_TIMEOUT_MS` = 1000 (per-test)
- `OTA_SELF_TEST_TOTAL_MS`   = 5000 (total)

If any test reports FAIL or any duration exceeds the budget, the production
OTA validator (in main firmware) would refuse to mark the new image valid
on a pending boot -> bootloader rolls back.

Reverts to MVP firmware via:

    cd ../../..
    ~/.platformio/penv/bin/pio run -e esp32dev -t upload

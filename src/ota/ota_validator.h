#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Driver "alive" hooks — overridden in tests; real impls inspect cached driver
 * state without re-initialising hardware (display + touch are already up). */
bool ota_validator_hook_tc_alive(void);
bool ota_validator_hook_ssr_alive(void);
bool ota_validator_hook_display_alive(void);
bool ota_validator_hook_touch_alive(void);

/* Pure consensus check for one TC sample window (3 samples): no fault flags,
 * all in [-50, 350] °C, spread <= OTA_TC_CONSENSUS_TOLERANCE. The on-target
 * tc_alive hook retries several windows because boot-time readings wiggle. */
bool ota_validator_tc_window_ok(const float t[3], const uint8_t fault_flags[3]);

bool ota_validator_run_self_tests(void);
bool ota_validator_pending(void);
void ota_validator_mark_valid(void);

/* Anchor the running slot as valid if its OTA state is UNDEFINED (e.g. the
 * USB-flashed base image, whose otadata the bootloader never marks). Ensures
 * a valid slot to roll back into always exists, so a failed *first* OTA
 * cannot brick the device (finding #1). No-op if the slot is already VALID or
 * is pending verification. */
void ota_validator_anchor_running(void);

/* Last self-test failure reason — for boot-time logging + optional UI surface. */
const char *ota_validator_last_failure(void);

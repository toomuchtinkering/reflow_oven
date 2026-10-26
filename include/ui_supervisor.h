#pragma once
#include <stdbool.h>
#include <stdint.h>

/* UI-stall predicate, kept pure and header-only so it builds — and is tested —
 * in the native env alongside the other logic modules. The stateful half
 * (reading the heartbeat, recording the breadcrumb, rebooting) lives in
 * control_task, which is ESP-only.
 *
 * `armed` is false until the first heartbeat has been observed, so a slow UI
 * bring-up (display + touch init, boot self-tests) can never trip the check:
 * a UI that has not started yet is not a UI that has stalled.
 *
 * The subtraction is done in unsigned arithmetic on purpose so the comparison
 * stays correct across the 32-bit millisecond wrap (~49.7 days). An oven left
 * powered for that long must not reboot itself on the rollover. */
static inline bool ui_supervisor_stalled(bool armed, uint32_t now_ms,
                                         uint32_t last_beat_ms,
                                         uint32_t timeout_ms) {
    if (!armed) return false;
    return (uint32_t)(now_ms - last_beat_ms) >= timeout_ms;
}

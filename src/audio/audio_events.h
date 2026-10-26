/* Pure transition-detection helper. No FreeRTOS / no IDF.
 * Host-testable. See spec 2026-05-23 §3.4.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "control/reflow_sm.h"
#include "control/profile_curve.h"

typedef enum {
    AUDIO_EVT_STAGE_TRANSITION,
    AUDIO_EVT_RUN_COMPLETE,
    AUDIO_EVT_FAULT,
    AUDIO_EVT_FAULT_ACK,
} audio_event_t;

typedef struct {
    oven_mode_t      mode;
    stage_t          stage;
    uint8_t          fault_flags;
    /* Only meaningful when mode has just left MODE_DRYING; DRY_END_NONE
     * otherwise. Needed because every dry exit lands in MODE_IDLE, so the
     * mode transition alone cannot tell a finished dry from a manual stop. */
    dry_end_reason_t dry_end_reason;
} audio_state_snapshot_t;

/* Diff prev -> curr and write events into out[]. Returns the number written
 * (0..4). Rules:
 *   - curr.stage != prev.stage and curr.stage in {HEAT_UP_1..COOL_DOWN}
 *     -> STAGE_TRANSITION
 *   - prev.mode==RUNNING and curr.mode==IDLE -> RUN_COMPLETE
 *   - prev.mode==DRYING and curr.mode==IDLE and
 *     curr.dry_end_reason==DRY_END_COMPLETE -> RUN_COMPLETE (spec §1.3/§1.4).
 *     A manual stop or a heat-up timeout stays silent: the user is already
 *     standing there for the first, and the second is reported as a fault
 *     line on screen, not as a "your filament is ready" chime.
 *   - prev.fault_flags==0 and curr.fault_flags!=0 -> FAULT
 *   - prev.mode==FAULT and curr.mode!=FAULT -> FAULT_ACK
 * Priority: when FAULT is emitted this tick, STAGE_TRANSITION is suppressed.
 */
size_t audio_detect_events(audio_state_snapshot_t prev,
                           audio_state_snapshot_t curr,
                           audio_event_t out[4]);

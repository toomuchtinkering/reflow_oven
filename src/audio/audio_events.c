#include "audio/audio_events.h"

static int stage_in_trigger_set(stage_t s) {
    /* Every active stage entry pips, COOL_DOWN included: entering cool-down is
     * the door-open cue and the most actionable moment of the run. RUN_COMPLETE
     * does NOT cover it — that waits for temp < COOL_DOWN_THRESHOLD_C, minutes
     * later, so without this the important transition would be silent. */
    return s >= STAGE_HEAT_UP_1 && s <= STAGE_COOL_DOWN;
}

size_t audio_detect_events(audio_state_snapshot_t prev,
                           audio_state_snapshot_t curr,
                           audio_event_t out[4]) {
    size_t n = 0;
    int fault_rising = (prev.fault_flags == 0) && (curr.fault_flags != 0);
    int fault_acked  = (prev.mode == MODE_FAULT) && (curr.mode != MODE_FAULT);
    /* A finished dry reuses RUN_COMPLETE (spec §1.3/§1.4). It is the single
     * most useful notification the feature has — a dry runs 6-16 h, so the
     * user is rarely watching when it ends. Gated on DRY_END_COMPLETE because
     * a manual stop and a heat-up timeout also land in MODE_IDLE. */
    int dry_complete = (prev.mode == MODE_DRYING) && (curr.mode == MODE_IDLE) &&
                       (curr.dry_end_reason == DRY_END_COMPLETE);
    int run_complete = ((prev.mode == MODE_RUNNING) && (curr.mode == MODE_IDLE)) ||
                       dry_complete;
    int stage_changed = (curr.stage != prev.stage) && stage_in_trigger_set(curr.stage);

    /* FAULT takes priority over a same-tick STAGE_TRANSITION. */
    if (fault_rising) {
        out[n++] = AUDIO_EVT_FAULT;
    } else if (stage_changed) {
        out[n++] = AUDIO_EVT_STAGE_TRANSITION;
    }
    if (run_complete) {
        out[n++] = AUDIO_EVT_RUN_COMPLETE;
    }
    if (fault_acked) {
        out[n++] = AUDIO_EVT_FAULT_ACK;
    }
    return n;
}

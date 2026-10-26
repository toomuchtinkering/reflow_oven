#include "control/reflow_sm.h"
#include "control/dry_setpoint.h"
#include "config.h"
#include <math.h>
#include <string.h>

void sm_init(sm_state_t *s) {
    memset(s, 0, sizeof(*s));
    s->mode  = MODE_IDLE;
    s->stage = STAGE_NONE;
}

void sm_trip_fault(sm_state_t *s, uint8_t flag) {
    if (s->mode == MODE_STEP_TEST) s->step_end_reason = STEP_END_FAULT;
    if (s->mode == MODE_DRYING) {
        /* Clear the stage as dry_end() does on every other exit: the UI and
         * telemetry read dry_stage straight out of oven_state_t, so leaving
         * it at "hold" would show a stopped oven still holding. */
        s->dry_end_reason = DRY_END_FAULT;
        s->dry_stage      = DRY_STAGE_NONE;
    }
    s->mode = MODE_FAULT;
    s->stage = STAGE_NONE;
    s->fault_flags |= flag;
}

bool sm_start_step_test(sm_state_t *s, uint8_t duty, float ceiling_c, uint32_t max_ms) {
    /* Mirror the START/OTA entry guard: idle only, not reserved for OTA, no
     * validation gate active. Starting any heat requires these same
     * preconditions as a normal reflow run. */
    if (s->mode != MODE_IDLE) return false;
    if (s->ota_reserved) return false;
    if (s->validation != SM_VALIDATION_NONE) return false;

    if (duty < STEP_TEST_DUTY_MIN) duty = STEP_TEST_DUTY_MIN;
    if (duty > STEP_TEST_DUTY_MAX) duty = STEP_TEST_DUTY_MAX;

    s->mode            = MODE_STEP_TEST;
    s->stage           = STAGE_NONE;
    s->elapsed_ms      = 0;
    s->step_duty       = duty;
    s->step_ceiling_c  = ceiling_c;
    s->step_max_ms     = max_ms;
    s->step_end_reason = STEP_END_NONE;
    return true;
}

bool sm_step_test_tick(sm_state_t *s, float temp_c) {
    if (s->mode != MODE_STEP_TEST) return false;

    /* Ceiling is the normal end-of-test. Checked before advancing time so a
     * test that starts already at/above the ceiling ends immediately. */
    if (temp_c >= s->step_ceiling_c) {
        s->mode = MODE_IDLE;
        s->stage = STAGE_NONE;
        s->step_end_reason = STEP_END_CEILING;
        return false;
    }
    s->elapsed_ms += CONTROL_TICK_MS;
    if (s->elapsed_ms >= s->step_max_ms) {
        s->mode = MODE_IDLE;
        s->stage = STAGE_NONE;
        s->step_end_reason = STEP_END_MAX_DURATION;
        return false;
    }
    return true;
}

static void dry_end(sm_state_t *s, dry_end_reason_t why) {
    s->mode           = MODE_IDLE;
    s->stage          = STAGE_NONE;
    s->dry_stage      = DRY_STAGE_NONE;
    s->dry_end_reason = why;
}

bool sm_start_dry(sm_state_t *s, float target_c, uint32_t hold_ms, float temp_c) {
    /* Reject non-finite inputs before anything else. NaN compares false
     * against every clamp and gate below, so a NaN target would survive
     * unclamped into dry_target_c and then defeat the caller's own
     * over-temperature guard (temp > NaN + margin is false) while never
     * reaching the hold band. A NaN temp_c would likewise slip past the
     * start gate and poison dry_setpoint() for the whole run. Same shape as
     * pid_validate() in src/storage/nvs_schema.c. */
    if (!isfinite(target_c) || !isfinite(temp_c)) return false;

    /* Same entry guard as every other heat-producing mode. */
    if (s->mode != MODE_IDLE) return false;
    if (s->ota_reserved) return false;
    if (s->validation != SM_VALIDATION_NONE) return false;

    if (target_c < DRY_TEMP_MIN_C) target_c = DRY_TEMP_MIN_C;
    if (target_c > DRY_TEMP_MAX_C) target_c = DRY_TEMP_MAX_C;
    if (hold_ms  < DRY_HOLD_MIN_MS)      hold_ms = DRY_HOLD_MIN_MS;
    if (hold_ms  > DRY_DURATION_MAX_MS)  hold_ms = DRY_DURATION_MAX_MS;

    /* Refuse a start hotter than the target rather than silently waiting to
     * cool. Clamped target first, so the gate tracks what will actually run. */
    if (temp_c > target_c + DRY_START_MARGIN_C) return false;

    s->mode                = MODE_DRYING;
    s->stage               = STAGE_NONE;
    s->elapsed_ms          = 0;
    s->dry_target_c        = target_c;
    s->dry_start_temp_c    = temp_c;
    s->dry_hold_ms         = hold_ms;
    s->dry_hold_elapsed_ms = 0;
    s->dry_stage           = DRY_STAGE_HEAT_UP;
    s->dry_end_reason      = DRY_END_NONE;
    return true;
}

bool sm_dry_tick(sm_state_t *s, float temp_c, float *setpoint_out) {
    if (s->mode != MODE_DRYING) return false;

    s->elapsed_ms += CONTROL_TICK_MS;

    if (s->dry_stage == DRY_STAGE_HEAT_UP) {
        /* One-way: once in the band we stay in HOLD, so a transient dip
         * cannot extend the run indefinitely. */
        if (temp_c >= s->dry_target_c - DRY_HOLD_BAND_C) {
            s->dry_stage = DRY_STAGE_HOLD;
        } else if (s->elapsed_ms >= DRY_HEATUP_TIMEOUT_MS) {
            /* Target unreachable (failed element, open door, huge load).
             * Without this the hold timer never starts and heating never
             * ends. */
            dry_end(s, DRY_END_HEATUP_TIMEOUT);
            return false;
        }
    }

    if (s->dry_stage == DRY_STAGE_HOLD) {
        s->dry_hold_elapsed_ms += CONTROL_TICK_MS;
        if (s->dry_hold_elapsed_ms >= s->dry_hold_ms) {
            dry_end(s, DRY_END_COMPLETE);
            return false;
        }
    }

    *setpoint_out = dry_setpoint(s->dry_start_temp_c, s->dry_target_c,
                                 s->elapsed_ms);
    return true;
}

bool sm_dry_stop(sm_state_t *s) {
    if (s->mode != MODE_DRYING) return false;
    dry_end(s, DRY_END_MANUAL_STOP);
    return true;
}

const char *dry_stage_name(dry_stage_t s) {
    switch (s) {
        case DRY_STAGE_HEAT_UP: return "heat_up";
        case DRY_STAGE_HOLD:    return "hold";
        default:                return "none";
    }
}

const char *dry_end_name(dry_end_reason_t r) {
    switch (r) {
        case DRY_END_COMPLETE:       return "complete";
        case DRY_END_MANUAL_STOP:    return "stopped";
        case DRY_END_HEATUP_TIMEOUT: return "heatup_timeout";
        case DRY_END_FAULT:          return "fault";
        default:                     return "none";
    }
}

static bool active_stage(stage_t s) {
    return s == STAGE_HEAT_UP_1 || s == STAGE_SOAK ||
           s == STAGE_HEAT_UP_2 || s == STAGE_REFLOW;
}

static void advance_stage_if_due(sm_state_t *s, const profile_curve_t *c) {
    if (!active_stage(s->stage)) return;
    if (s->elapsed_ms < c->phase_end_ms[s->stage]) return;
    switch (s->stage) {
        case STAGE_HEAT_UP_1: s->stage = STAGE_SOAK; break;
        case STAGE_SOAK:      s->stage = STAGE_HEAT_UP_2; break;
        case STAGE_HEAT_UP_2: s->stage = STAGE_REFLOW; break;
        case STAGE_REFLOW:
            s->stage = STAGE_COOL_DOWN;
            s->cool_down_start_ms = s->elapsed_ms;
            break;
        default: break;
    }
}

bool sm_step(sm_state_t *s,
             sm_event_t event,
             float temp_c,
             const profile_curve_t *curve) {

    if (s->mode == MODE_STEP_TEST) {
        if (event == SM_EVENT_CMD_STOP) {
            s->mode = MODE_IDLE;
            s->stage = STAGE_NONE;
            s->step_end_reason = STEP_END_MANUAL_STOP;
        }
        /* All other events ignored here; the control task calls
         * sm_step_test_tick directly for the per-tick advance. */
        return false;
    }

    if (s->mode == MODE_DRYING) {
        /* Route STOP through sm_dry_stop so the end reason is recorded; the
         * control task calls sm_dry_tick directly for the per-tick advance.
         * Falling through to the tail would double-count elapsed_ms. */
        if (event == SM_EVENT_CMD_STOP) sm_dry_stop(s);
        return false;
    }

    if (s->mode == MODE_PENDING_VALIDATION) {
        /* Only a genuine PENDING gate (self-tests passed) can be cleared by
         * the user; a FAILED gate must roll back via reboot (finding #4). */
        if (event == SM_EVENT_CMD_CLEAR_PENDING &&
            s->validation == SM_VALIDATION_PENDING) {
            s->validation = SM_VALIDATION_NONE;
            s->mode = MODE_IDLE;
        }
        return false;  /* no profile work while a validation gate is active */
    }

    if (s->mode == MODE_FAULT) {
        /* A latched fault must be acknowledged before anything else; any other
         * SM event queued this tick (START / STOP / CLEAR_PENDING) is
         * intentionally ignored here. CLEAR_PENDING is safe to drop: the
         * validation gate persists, START stays blocked, and once the fault is
         * acked the oven returns to MODE_PENDING_VALIDATION (below) so the
         * confirm banner reappears for a clean re-tap. */
        if (event == SM_EVENT_CMD_ACK_FAULT) {
            s->stage = STAGE_NONE;
            s->fault_flags = 0;
            s->elapsed_ms = 0;
            /* If a validation gate is still active, return to the blocked
             * state rather than IDLE so rollback safety is not lost across a
             * fault that tripped while pending validation (finding #3). */
            s->mode = (s->validation != SM_VALIDATION_NONE)
                    ? MODE_PENDING_VALIDATION : MODE_IDLE;
        }
        return false;
    }

    if (s->mode == MODE_IDLE) {
        /* Reject START while an OTA upload holds the oven reserved, so a run
         * can't begin between the server's idle-check and its esp_ota_begin
         * (finding #13). Also refuse to start above RUN_START_MAX_TEMP_C so
         * every run begins from a comparable near-ambient state; the UI
         * disables START and shows the reason, this is the enforcement. */
        if (event == SM_EVENT_CMD_START && !s->ota_reserved
            && temp_c <= RUN_START_MAX_TEMP_C) {
            s->mode = MODE_RUNNING;
            s->stage = STAGE_HEAT_UP_1;
            s->elapsed_ms = 0;
        }
        return false;
    }

    if (event == SM_EVENT_CMD_STOP) {
        s->mode = MODE_IDLE;
        s->stage = STAGE_NONE;
        s->elapsed_ms = 0;
        return false;
    }

    if (s->stage == STAGE_COOL_DOWN) {
        bool cool = temp_c < COOL_DOWN_THRESHOLD_C;
        bool timed_out = (s->elapsed_ms - s->cool_down_start_ms) > COOL_DOWN_TIMEOUT_MS;
        if (cool || timed_out) {
            s->mode = MODE_IDLE;
            s->stage = STAGE_NONE;
            s->elapsed_ms = 0;
        } else {
            s->elapsed_ms += CONTROL_TICK_MS;
        }
        return false;
    }

    s->elapsed_ms += CONTROL_TICK_MS;
    advance_stage_if_due(s, curve);
    return active_stage(s->stage);
}

const char *stage_name(stage_t s) {
    switch (s) {
        case STAGE_NONE:      return "Idle";
        case STAGE_HEAT_UP_1: return "Heat-up 1";
        case STAGE_SOAK:      return "Soak";
        case STAGE_HEAT_UP_2: return "Heat-up 2";
        case STAGE_REFLOW:    return "Reflow";
        case STAGE_COOL_DOWN: return "Cool-down";
        case STAGE_COUNT_:    return "?";  /* not a real stage */
    }
    return "?";
}

const char *fault_text(uint8_t fault_flags) {
    if (fault_flags & FAULT_TC_OPEN)   return "thermocouple open";
    if (fault_flags & FAULT_TC_SCG)    return "thermocouple short to GND";
    if (fault_flags & FAULT_TC_SCV)    return "thermocouple short to VCC";
    if (fault_flags & FAULT_TC_STALE)  return "stale sensor";
    if (fault_flags & FAULT_OVER_TEMP) return "over-temperature";
    if (fault_flags & FAULT_ELECTRONICS_HOT) return "electronics too hot";
    return "unknown";
}

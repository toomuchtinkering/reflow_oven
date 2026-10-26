/* Pure telemetry JSON serialiser — no FreeRTOS / httpd deps, host-testable.
 * Writes the GET /telemetry body into `buf`. Returns the strlen written
 * (excluding NUL), or -1 on truncation. */
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "control/pid.h"   /* pid_gains_t for the /profile head */

typedef struct {
    int      api;
    const char *mode;          /* already-resolved mode string */
    float    temp_c, setpoint_c;
    float    cj_c;             /* electronics (cold-junction) temp; NAN -> null */
    unsigned duty, elapsed_ms, fault, run_id;
    bool     step_active;
    unsigned step_duty;
    float    step_ceiling_c;
    unsigned step_max_ms;
    const char *step_end;      /* already-resolved end-reason string */
    /* Filament drying (v3). Meaningful while mode == "drying" or just after;
     * dry_active is false and the rest zero/"none" otherwise. */
    bool     dry_active;
    const char *dry_stage;     /* already-resolved: "none"/"heat_up"/"hold" */
    float    dry_target_c;
    unsigned dry_hold_ms, dry_hold_elapsed_ms;
    const char *dry_end;       /* already-resolved end-reason string */
    bool     pending_pid_valid;
    float    kp, ki, kd;
} telemetry_view_t;

int telemetry_json(const telemetry_view_t *v, char *buf, size_t n);

/* Head of the chunked GET /profile response, up to and including the opening
 * '[' of phase_end_ms (the handler streams the arrays after it). Pure and
 * host-testable like telemetry_json. Returns strlen written, -1 on
 * truncation. */
int profile_head_json(char *buf, size_t n, int api, const pid_gains_t *g,
                      float ambient_c, int step_ms, unsigned len);

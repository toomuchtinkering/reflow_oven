#include "ota/telemetry_json.h"
#include <math.h>
#include <stdio.h>

int telemetry_json(const telemetry_view_t *v, char *buf, size_t n) {
    /* NAN ("no reading yet") must serialise as null — printf'd "nan" is not
     * valid JSON and breaks the SPA's response parsing. */
    char cj_buf[16];
    if (isnan(v->cj_c)) snprintf(cj_buf, sizeof cj_buf, "null");
    else                snprintf(cj_buf, sizeof cj_buf, "%.2f", (double)v->cj_c);
    int w = snprintf(buf, n,
        "{\"api\":%d,\"mode\":\"%s\",\"temp_c\":%.2f,\"setpoint_c\":%.2f,"
        "\"cj_c\":%s,"
        "\"duty\":%u,\"elapsed_ms\":%u,\"fault\":%u,\"run_id\":%u,"
        "\"steptest\":{\"active\":%s,\"duty\":%u,\"ceiling_c\":%.1f,"
        "\"max_ms\":%u,\"end_reason\":\"%s\"},"
        "\"dry_active\":%s,\"dry_stage\":\"%s\",\"dry_target_c\":%.1f,"
        "\"dry_hold_ms\":%u,\"dry_hold_elapsed_ms\":%u,\"dry_end\":\"%s\",",
        v->api, v->mode ? v->mode : "unknown", v->temp_c, v->setpoint_c,
        cj_buf,
        v->duty, v->elapsed_ms, v->fault, v->run_id,
        v->step_active ? "true" : "false", v->step_duty, v->step_ceiling_c,
        v->step_max_ms, v->step_end ? v->step_end : "none",
        v->dry_active ? "true" : "false",
        v->dry_stage ? v->dry_stage : "none",
        v->dry_target_c,
        v->dry_hold_ms,
        v->dry_hold_elapsed_ms,
        v->dry_end ? v->dry_end : "none");
    if (w < 0 || (size_t)w >= n) return -1;            /* truncated */
    int w2 = v->pending_pid_valid
        ? snprintf(buf + w, n - w, "\"pending_pid\":{\"kp\":%.3f,\"ki\":%.3f,\"kd\":%.3f}}",
                   v->kp, v->ki, v->kd)
        : snprintf(buf + w, n - w, "\"pending_pid\":null}");
    if (w2 < 0 || (size_t)(w + w2) >= n) return -1;
    return w + w2;
}

int profile_head_json(char *buf, size_t n, int api, const pid_gains_t *g,
                      float ambient_c, int step_ms, unsigned len) {
    int w = snprintf(buf, n,
        "{\"api\":%d,"
        "\"gains\":{\"kp\":%.4f,\"ki\":%.5f,\"kd\":%.3f,"
        "\"k_hold\":%.4f,\"k_rate\":%.2f},"
        "\"ambient_ref_c\":%.2f,"
        "\"step_ms\":%d,\"len\":%u,\"phase_end_ms\":[",
        api, g->kp, g->ki, g->kd, g->k_hold, g->k_rate,
        ambient_c, step_ms, len);
    if (w < 0 || (size_t)w >= n) return -1;            /* truncated */
    return w;
}

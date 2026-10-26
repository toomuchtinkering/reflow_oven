#include "storage/nvs_schema.h"
#include "config.h"
#include <string.h>
#include <math.h>

void profile_to_blob(const profile_t *p, profile_v2_t *out) {
    out->version            = PROFILE_SCHEMA_VERSION;
    out->first_heatup_rate  = p->first_heatup_rate;
    out->soak_temp          = p->soak_temp;
    out->soak_time          = p->soak_time;
    out->second_heatup_rate = p->second_heatup_rate;
    out->reflow_temp        = p->reflow_temp;
    out->reflow_time        = p->reflow_time;
    out->cool_down_rate     = p->cool_down_rate;
}

bool profile_from_blob(const void *blob, size_t blob_len, profile_t *out) {
    if (blob_len != sizeof(profile_v2_t)) return false;
    const profile_v2_t *b = (const profile_v2_t *)blob;
    if (b->version != PROFILE_SCHEMA_VERSION) return false;
    out->first_heatup_rate  = b->first_heatup_rate;
    out->soak_temp          = b->soak_temp;
    out->soak_time          = b->soak_time;
    out->second_heatup_rate = b->second_heatup_rate;
    out->reflow_temp        = b->reflow_temp;
    out->reflow_time        = b->reflow_time;
    out->cool_down_rate     = b->cool_down_rate;
    return true;
}

void pid_to_blob(const pid_gains_t *g, pid_v2_t *out) {
    out->version = PID_SCHEMA_VERSION;
    out->kp = g->kp;
    out->ki = g->ki;
    out->kd = g->kd;
    out->k_hold = g->k_hold;
    out->k_rate = g->k_rate;
}

bool pid_from_blob(const void *blob, size_t blob_len, pid_gains_t *out) {
    if (blob_len == sizeof(pid_v2_t)) {
        const pid_v2_t *b = (const pid_v2_t *)blob;
        if (b->version != 2) return false;
        out->kp = b->kp;
        out->ki = b->ki;
        out->kd = b->kd;
        out->k_hold = b->k_hold;
        out->k_rate = b->k_rate;
        return true;
    }
    if (blob_len == sizeof(pid_v1_t)) {   /* legacy record: migrate */
        const pid_v1_t *b = (const pid_v1_t *)blob;
        if (b->version != 1) return false;
        out->kp = b->kp;
        out->ki = b->ki;
        out->kd = b->kd;
        out->k_hold = KFF_HOLD_DEFAULT;   /* ff introduced in v2 */
        out->k_rate = KFF_RATE_DEFAULT;
        return true;
    }
    return false;
}

void pid_defaults(pid_gains_t *out) {
    out->kp     = KP_DEFAULT;
    out->ki     = KI_DEFAULT;
    out->kd     = KD_DEFAULT;
    out->k_hold = KFF_HOLD_DEFAULT;
    out->k_rate = KFF_RATE_DEFAULT;
}

bool pid_validate(const pid_gains_t *g) {
    /* Reject NaN/Inf first: a range check alone lets NaN through (NaN compares
     * false to both bounds), so a malformed /suggest-gains body or corrupt NVS
     * blob could otherwise feed a non-finite gain into the live PID. */
    if (!isfinite(g->kp) || !isfinite(g->ki) || !isfinite(g->kd)) return false;
    if (g->kp < KP_MIN || g->kp > KP_MAX) return false;
    if (g->ki < KI_MIN || g->ki > KI_MAX) return false;
    if (g->kd < KD_MIN || g->kd > KD_MAX) return false;
    if (!isfinite(g->k_hold) || !isfinite(g->k_rate)) return false;
    if (g->k_hold < KFF_HOLD_MIN || g->k_hold > KFF_HOLD_MAX) return false;
    if (g->k_rate < KFF_RATE_MIN || g->k_rate > KFF_RATE_MAX) return false;
    return true;
}

/* Compile-time name + default value table (config.h DRY_PRESET_DEFAULTS). */
static const struct { const char *name; uint16_t temp_c; uint16_t hold_min; }
    s_dry_defaults[DRY_PRESET_COUNT] = DRY_PRESET_DEFAULTS;

const char *dry_preset_name(unsigned i) {
    if (i >= DRY_PRESET_COUNT) return "?";
    return s_dry_defaults[i].name;
}

void dry_presets_defaults(dry_presets_t *out) {
    for (unsigned i = 0; i < DRY_PRESET_COUNT; i++) {
        out->e[i].temp_c   = s_dry_defaults[i].temp_c;
        out->e[i].hold_min = s_dry_defaults[i].hold_min;
    }
}

void dry_presets_to_blob(const dry_presets_t *p, dry_presets_v1_t *out) {
    out->version = DRY_PRESETS_SCHEMA_VERSION;
    for (unsigned i = 0; i < DRY_PRESET_COUNT; i++) out->e[i] = p->e[i];
}

bool dry_presets_from_blob(const void *blob, size_t blob_len, dry_presets_t *out) {
    if (blob_len != sizeof(dry_presets_v1_t)) return false;
    const dry_presets_v1_t *b = (const dry_presets_v1_t *)blob;
    if (b->version != DRY_PRESETS_SCHEMA_VERSION) return false;
    for (unsigned i = 0; i < DRY_PRESET_COUNT; i++) out->e[i] = b->e[i];
    return true;
}

bool dry_presets_validate(const dry_presets_t *p) {
    const uint16_t hold_max_min = (uint16_t)(DRY_DURATION_MAX_MS / 60000u);
    /* Ceiling, not floor: a DRY_HOLD_MIN_MS that is not a whole number of
     * minutes must round UP, or the floor would admit holds below it (at
     * 90000 ms the floor is 1, so a 60000 ms hold would validate). The
     * ternary then guards only DRY_HOLD_MIN_MS == 0. Today's 60000 divides
     * exactly, so this is 1 either way. */
    const uint16_t hold_min_min = ((DRY_HOLD_MIN_MS + 59999u) / 60000u)
                                ? (uint16_t)((DRY_HOLD_MIN_MS + 59999u) / 60000u)
                                : 1;
    for (unsigned i = 0; i < DRY_PRESET_COUNT; i++) {
        if (p->e[i].temp_c < (uint16_t)DRY_TEMP_MIN_C) return false;
        if (p->e[i].temp_c > (uint16_t)DRY_TEMP_MAX_C) return false;
        if (p->e[i].hold_min < hold_min_min) return false;
        if (p->e[i].hold_min > hold_max_min) return false;
    }
    return true;
}

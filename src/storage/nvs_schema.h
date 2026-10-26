/* Versioned struct ↔ blob serialisation for profile, PID and drying-preset
 * records.
 * Pure logic — the actual NVS I/O lives in drivers/nvs_store (Phase 4).
 * See spec §8.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"           /* DRY_PRESET_COUNT, DRY_* bounds */
#include "control/profile_curve.h"
#include "control/pid.h"      /* pid_gains_t lives with the controller */

typedef struct __attribute__((packed)) {
    uint8_t  version;
    float    first_heatup_rate;
    uint16_t soak_temp;
    uint16_t soak_time;
    float    second_heatup_rate;
    uint16_t reflow_temp;
    uint16_t reflow_time;
    float    cool_down_rate;       /* v2 addition */
} profile_v2_t;

/* Legacy v1 record (pre-feedforward) — read-only, migrated on load. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    float   kp, ki, kd;
} pid_v1_t;

typedef struct __attribute__((packed)) {
    uint8_t version;             /* = 2 */
    float   kp, ki, kd;
    float   k_hold, k_rate;      /* v2 addition: feedforward (spec §3) */
} pid_v2_t;

void profile_to_blob(const profile_t *p, profile_v2_t *out);
bool profile_from_blob(const void *blob, size_t blob_len, profile_t *out);

void pid_to_blob(const pid_gains_t *g, pid_v2_t *out);
bool pid_from_blob(const void *blob, size_t blob_len, pid_gains_t *out);

void pid_defaults(pid_gains_t *out);

/* Range-check each gain against the bounds in config.h. Returns true if
 * the record is safe to use. Mirrors profile_validate. Used by nvs_store
 * to reject corrupted / out-of-bounds records on load — negative gains
 * would invert the control sign and create a thermal-runaway hazard. */
bool pid_validate(const pid_gains_t *g);

/* ---- drying presets (DRY_PRESETS_SCHEMA_VERSION lives in config.h) ---- */

/* Runtime form. Names are NOT stored — they are compile-time (see config.h
 * DRY_PRESET_DEFAULTS); only the editable values persist. */
typedef struct {
    uint16_t temp_c;
    uint16_t hold_min;
} dry_entry_t;

typedef struct {
    dry_entry_t e[DRY_PRESET_COUNT];
} dry_presets_t;

/* Stored form: 1 + 6*4 = 25 bytes. */
typedef struct __attribute__((packed)) {
    uint8_t     version;
    dry_entry_t e[DRY_PRESET_COUNT];
} dry_presets_v1_t;

/* dry_entry_t is shared between the runtime and stored forms and copied by
 * struct assignment, so a field added to it would resize the on-flash record
 * with no edit anywhere near the version macro — and every saved preset would
 * revert to defaults, logged only as a schema mismatch. Pin the size. */
_Static_assert(sizeof(dry_presets_v1_t) == 25,
               "dry preset record layout changed — bump DRY_PRESETS_SCHEMA_VERSION");

void dry_presets_to_blob(const dry_presets_t *p, dry_presets_v1_t *out);
bool dry_presets_from_blob(const void *blob, size_t blob_len, dry_presets_t *out);
void dry_presets_defaults(dry_presets_t *out);

/* Range-checks every entry against the drying bounds in config.h. A corrupt
 * or out-of-range record must never be partially trusted — an out-of-range
 * target is a thermal hazard, same reasoning as pid_validate. */
bool dry_presets_validate(const dry_presets_t *p);

/* Compile-time preset name for index i. Returns "?" when out of range.
 * Static string, never NULL, never freed. */
const char *dry_preset_name(unsigned i);

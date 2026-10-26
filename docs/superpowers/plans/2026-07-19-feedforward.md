# PID Feedforward + Self-Describing Run Files — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add model-based feedforward (`ff = k_hold·(setpoint−ambient) + k_rate·rate`) inside the PID, runtime-tunable from web + touchscreen, and make archived run files self-describing (fw version, gains, ambient, profile breakpoints, split per-phase metrics, k_hold auto-estimate).

**Architecture:** Feedforward lives inside `pid_step()` so the conditional-integration anti-windup tests the true output sum (spec Approach A). `pid_gains_t` moves to `control/pid.h` and grows to 5 fields; NVS PID blob goes v1→v2 with migration. The setpoint rate comes analytically from a new per-stage rate array in `profile_curve_t`. `GET /profile` additionally reports active gains + ambient ref; the SPA archives them per run and computes per-phase metrics + a k_hold estimate client-side.

**Tech Stack:** C11 (ESP-IDF via PlatformIO; host tests via `[env:native]` + Unity), LVGL touchscreen UI, vanilla-JS SPA (node:test for pure functions).

**Spec:** `docs/superpowers/specs/2026-07-19-feedforward-design.md`

**Commands (memorize):**
- Host tests (pio is NOT on PATH): `~/.platformio/penv/bin/pio test -e native` (filter one suite: `-f test_pid`)
- Web tests: `node --test test/web/`
- Device build only: `~/.platformio/penv/bin/pio run -e esp32dev`
- Do NOT flash from a subagent; OTA flashing is a manual step at the end (see `docs` + memory: touch CMakeLists for version bump, `tools/ota_upload.py`).

---

### Task 1: config.h — new constants, safer defaults

**Files:**
- Modify: `include/config.h` (PID gains block, ~lines 87–106)

- [ ] **Step 1: Edit the PID gains block**

Replace the current defaults/bounds (`KP_DEFAULT 10.0f`, `KI_DEFAULT 10.0f`, `KD_DEFAULT 0.0f`, `KI_MAX 50.0f`) and append the feedforward constants, so the block reads:

```c
/* PID gains. Defaults are the identified no-overshoot set from the
 * 2026-07-19 step test (L=32.5 s, N=1.12 °C/s @ 80 % duty) — with
 * feedforward carrying the duty, gentle feedback is the right factory
 * default. KI_MAX 1.0: every plausible Ki for this plant is < 0.05; a
 * larger cap allows a keypad entry ~1000x past stability. */
#define KP_MIN                    0.0f
#define KP_MAX                  100.0f
#define KP_DEFAULT                0.44f

#define KI_MIN                    0.0f
#define KI_MAX                    1.0f
#define KI_DEFAULT                0.0068f

#define KD_MIN                    0.0f
#define KD_MAX                   50.0f
#define KD_DEFAULT                4.75f

/* Feedforward (spec 2026-07-19 §1): ff = k_hold·(setpoint − ambient_ref)
 * + k_rate·max(rate, 0). k_hold = holding duty per °C above ambient
 * (fitted from 2026-07-19 run 1 quasi-steady segments). k_rate = duty per
 * °C/s of commanded ramp = 1/R from the step-test fit (plant property,
 * NOT profile-dependent). TUNE via step test (k_rate) / run estimate (k_hold). */
#define KFF_HOLD_MIN              0.0f
#define KFF_HOLD_MAX              1.0f
#define KFF_HOLD_DEFAULT          0.19f

#define KFF_RATE_MIN              0.0f
#define KFF_RATE_MAX            300.0f
#define KFF_RATE_DEFAULT         71.0f

/* Ambient reference clamp for the ff hold term: pid_reset captures the
 * run-start temperature, clamped to this range so a still-warm oven
 * doesn't zero out the hold model (losses scale with temp above ROOM). */
#define AMBIENT_REF_MIN_C        15.0f
#define AMBIENT_REF_MAX_C        40.0f
```

- [ ] **Step 2: Run the full native suite**

Run: `~/.platformio/penv/bin/pio test -e native`
Expected: PASS. (`test_nvs_schema` asserts defaults against the config macros, so it tracks the change. If any test hard-codes the old `KI_MAX 50`/defaults, update the literal to the macro.)

- [ ] **Step 3: Commit**

```bash
git add include/config.h
git commit -m "feat(config): feedforward constants, identified PID defaults, sane KI_MAX"
```

---

### Task 2: profile_curve — per-stage rates + `profile_curve_rate()`

**Files:**
- Modify: `src/control/profile_curve.h`, `src/control/profile_curve.c`
- Test: `test/test_profile_curve/test_profile_curve.c`

- [ ] **Step 1: Write the failing tests** (append to the existing suite; follow its `profile_defaults` usage)

```c
/* stage_rate[] is filled by generate: signed rates per stage (spec §2). */
void test_stage_rates_filled(void) {
    profile_t p; profile_defaults(&p);
    profile_curve_t *c = malloc(sizeof *c);
    TEST_ASSERT_TRUE(profile_curve_generate(&p, c));
    TEST_ASSERT_EQUAL_FLOAT(p.first_heatup_rate,  c->stage_rate[STAGE_HEAT_UP_1]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f,                 c->stage_rate[STAGE_SOAK]);
    TEST_ASSERT_EQUAL_FLOAT(p.second_heatup_rate, c->stage_rate[STAGE_HEAT_UP_2]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f,                 c->stage_rate[STAGE_REFLOW]);
    TEST_ASSERT_EQUAL_FLOAT(-p.cool_down_rate,    c->stage_rate[STAGE_COOL_DOWN]);
    free(c);
}

/* Rate lookup resolves the active stage by phase_end_ms; past end -> 0. */
void test_curve_rate_lookup(void) {
    profile_t p; profile_defaults(&p);
    profile_curve_t *c = malloc(sizeof *c);
    TEST_ASSERT_TRUE(profile_curve_generate(&p, c));
    /* mid heat-up 1 */
    TEST_ASSERT_EQUAL_FLOAT(p.first_heatup_rate,
        profile_curve_rate(c, c->phase_end_ms[STAGE_HEAT_UP_1] / 2));
    /* first ms of soak (boundary: end of heat-up-1 belongs to next stage) */
    TEST_ASSERT_EQUAL_FLOAT(0.0f,
        profile_curve_rate(c, c->phase_end_ms[STAGE_HEAT_UP_1]));
    /* mid heat-up 2 */
    TEST_ASSERT_EQUAL_FLOAT(p.second_heatup_rate,
        profile_curve_rate(c, (c->phase_end_ms[STAGE_SOAK]
                             + c->phase_end_ms[STAGE_HEAT_UP_2]) / 2));
    /* past everything -> 0 */
    TEST_ASSERT_EQUAL_FLOAT(0.0f,
        profile_curve_rate(c, c->phase_end_ms[STAGE_COOL_DOWN] + 60000u));
    free(c);
}
```

Add `#include <stdlib.h>` if not present. Register both in the suite's `main()` `RUN_TEST` list.

- [ ] **Step 2: Run to verify failure**

Run: `~/.platformio/penv/bin/pio test -e native -f test_profile_curve`
Expected: FAIL — `stage_rate` undeclared / `profile_curve_rate` undefined.

- [ ] **Step 3: Implement**

`profile_curve.h` — add to `profile_curve_t` and declare the lookup:

```c
typedef struct {
    float    samples[PROFILE_CURVE_MAX_SAMPLES];
    uint16_t length;
    uint32_t phase_end_ms[STAGE_COUNT_];
    /* Signed setpoint slope (°C/s) per stage; filled by generate.
     * Consumers wanting heat-only ff must clamp negatives themselves. */
    float    stage_rate[STAGE_COUNT_];
} profile_curve_t;

/* Setpoint slope of the stage active at elapsed_ms; 0 past the end. */
float profile_curve_rate(const profile_curve_t *c, uint32_t elapsed_ms);
```

`profile_curve.c` — in `profile_curve_generate()` after the phases are built (the `memset` already zeroed `stage_rate`, covering NONE/SOAK/REFLOW):

```c
    out->stage_rate[STAGE_HEAT_UP_1] = p->first_heatup_rate;
    out->stage_rate[STAGE_HEAT_UP_2] = p->second_heatup_rate;
    out->stage_rate[STAGE_COOL_DOWN] = -p->cool_down_rate;
```

and append:

```c
float profile_curve_rate(const profile_curve_t *c, uint32_t elapsed_ms) {
    for (int s = STAGE_HEAT_UP_1; s < STAGE_COUNT_; s++) {
        if (elapsed_ms < c->phase_end_ms[s]) return c->stage_rate[s];
    }
    return 0.0f;
}
```

- [ ] **Step 4: Run to verify pass**

Run: `~/.platformio/penv/bin/pio test -e native -f test_profile_curve`
Expected: PASS (all, including pre-existing tests).

- [ ] **Step 5: Commit**

```bash
git add src/control/profile_curve.h src/control/profile_curve.c test/test_profile_curve/test_profile_curve.c
git commit -m "feat(profile): per-stage setpoint rates + profile_curve_rate lookup"
```

---

### Task 3: pid.c — 5-field gains struct, feedforward, anti-windup on the sum

**Files:**
- Modify: `src/control/pid.h`, `src/control/pid.c`
- Modify: `src/storage/nvs_schema.h` (move `pid_gains_t` out), `src/storage/nvs_schema.c` (defaults/validate/from_blob fill)
- Modify: `src/control/control_task.c:157,248,370` (call sites), `src/ui/screen_pid.c` + `src/ui/screen_pid_tuning.c` compile untouched (struct grows compatibly)
- Test: `test/test_pid/test_pid.c`

**New API (breaking — all call sites updated in this task):**

```c
/* pid.h */
typedef struct {                    /* moved here from storage/nvs_schema.h */
    float kp, ki, kd;
    float k_hold;   /* %/°C   — holding duty per °C above ambient_ref */
    float k_rate;   /* %·s/°C — duty per °C/s of commanded ramp (1/R) */
} pid_gains_t;

typedef struct {
    pid_gains_t g;
    float integrator;
    float prev_temp;
    float d_filt;
    float ambient_ref;  /* captured by pid_reset, clamped to AMBIENT_REF_*_C */
} pid_state_t;

void  pid_init(pid_state_t *s, const pid_gains_t *g);
void  pid_set_gains(pid_state_t *s, const pid_gains_t *g);
void  pid_reset(pid_state_t *s, float current_temp);
/* rate: commanded setpoint slope (°C/s); negative treated as 0. */
float pid_step(pid_state_t *s, float setpoint, float rate, float temp, float dt);
```

`nvs_schema.h`: delete its `pid_gains_t` definition and `#include "control/pid.h"` instead (same layering direction as its existing `control/profile_curve.h` include).

- [ ] **Step 1: Mechanically migrate the existing test file**

In `test/test_pid/test_pid.c` replace every
`pid_init(&pid, A, B, C)` → `pid_init(&pid, &(pid_gains_t){A, B, C, 0.0f, 0.0f})`,
`pid_set_gains(&pid, A, B, C)` → `pid_set_gains(&pid, &(pid_gains_t){A, B, C, 0.0f, 0.0f})`,
`pid_step(&pid, SP, T, DT)` → `pid_step(&pid, SP, 0.0f, T, DT)`,
and gain-field assertions `pid.kp` → `pid.g.kp` (etc.). **Do not change any expected values** — with `k_hold = k_rate = 0` and `rate = 0` the controller must be bit-identical to today's; the untouched assertions ARE the spec-§10 regression test.

- [ ] **Step 2: Add the new failing tests** (append + register in `main()`)

```c
#include "config.h"   /* AMBIENT_REF_*_C at top of file if missing */

/* ff hold term: kp=ki=kd=0 isolates it. ambient_ref=25 (reset), setpoint 125
 * -> ff = 0.3*(125-25) = 30 %. temp==prev so D=0. */
void test_ff_hold_term(void) {
    pid_init(&pid, &(pid_gains_t){0, 0, 0, 0.3f, 0});
    pid_reset(&pid, 25.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 30.0f, pid_step(&pid, 125.0f, 0.0f, 125.0f, 0.25f));
}

/* ff rate term: 71 %·s/°C * 0.6 °C/s = 42.6 %. setpoint==temp==ambient_ref. */
void test_ff_rate_term(void) {
    pid_init(&pid, &(pid_gains_t){0, 0, 0, 0, 71.0f});
    pid_reset(&pid, 25.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 42.6f, pid_step(&pid, 25.0f, 0.6f, 25.0f, 0.25f));
}

/* Negative rate (cool-down tail) must contribute nothing. */
void test_ff_negative_rate_clamped(void) {
    pid_init(&pid, &(pid_gains_t){0, 0, 0, 0, 71.0f});
    pid_reset(&pid, 25.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, pid_step(&pid, 25.0f, -1.0f, 25.0f, 0.25f));
}

/* Hold term never negative: setpoint below ambient_ref contributes 0, not <0. */
void test_ff_hold_term_not_negative(void) {
    pid_init(&pid, &(pid_gains_t){0, 0, 0, 0.5f, 0});
    pid_reset(&pid, 30.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, pid_step(&pid, 20.0f, 0.0f, 20.0f, 0.25f));
}

/* ambient_ref clamps: a hot restart (90 °C) clamps to AMBIENT_REF_MAX_C. */
void test_ambient_ref_clamped(void) {
    pid_init(&pid, &(pid_gains_t){0, 0, 0, 0.2f, 0});
    pid_reset(&pid, 90.0f);
    TEST_ASSERT_EQUAL_FLOAT(AMBIENT_REF_MAX_C, pid.ambient_ref);
    pid_reset(&pid, 5.0f);
    TEST_ASSERT_EQUAL_FLOAT(AMBIENT_REF_MIN_C, pid.ambient_ref);
}

/* Anti-windup must saturate on ff + PID, not PID alone: ff alone exceeds 100,
 * error positive -> output clamped to 100 AND integrator frozen. */
void test_antiwindup_tests_the_sum(void) {
    pid_init(&pid, &(pid_gains_t){0.1f, 1.0f, 0, 1.0f, 0});  /* small P, big I */
    pid_reset(&pid, 25.0f);
    float before = pid.integrator;
    /* setpoint 200: ff = 1.0*(200-25) = 175 > 100 on its own; error > 0 */
    float out = pid_step(&pid, 200.0f, 0.0f, 150.0f, 0.25f);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, out);
    TEST_ASSERT_EQUAL_FLOAT(before, pid.integrator);
}
```

- [ ] **Step 3: Run to verify failure**

Run: `~/.platformio/penv/bin/pio test -e native -f test_pid`
Expected: compile FAIL (new signatures) — that's the failing state for this task.

- [ ] **Step 4: Implement**

`pid.c` becomes:

```c
#include "control/pid.h"
#include "config.h"
#include <assert.h>

static float clamp(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

void pid_init(pid_state_t *s, const pid_gains_t *g) {
    s->g = *g;
    s->integrator  = 0.0f;
    s->prev_temp   = 0.0f;
    s->d_filt      = 0.0f;
    s->ambient_ref = AMBIENT_REF_MIN_C;
}

void pid_set_gains(pid_state_t *s, const pid_gains_t *g) { s->g = *g; }

void pid_reset(pid_state_t *s, float current_temp) {
    s->integrator  = 0.0f;
    s->prev_temp   = current_temp;
    s->d_filt      = 0.0f;
    s->ambient_ref = clamp(current_temp, AMBIENT_REF_MIN_C, AMBIENT_REF_MAX_C);
}

float pid_step(pid_state_t *s, float setpoint, float rate, float temp, float dt) {
    assert(dt > 0.0f);
    const float error       = setpoint - temp;
    const float p_term      = s->g.kp * error;
    const float d_raw       = -s->g.kd * (temp - s->prev_temp) / dt;
    const float alpha       = dt / (PID_D_FILTER_TAU_S + dt);
    s->d_filt              += alpha * (d_raw - s->d_filt);
    const float d_term      = s->d_filt;
    /* Feedforward (spec §1): planned-trajectory duty from the plant model.
     * Both terms clamp at 0 — ff may never subtract duty (cool-down rate is
     * negative; a setpoint below ambient_ref is outside the loss model). */
    float ff_hold = s->g.k_hold * (setpoint - s->ambient_ref);
    if (ff_hold < 0.0f) ff_hold = 0.0f;
    const float ff          = ff_hold + s->g.k_rate * (rate > 0.0f ? rate : 0.0f);
    const float i_tentative = s->integrator + s->g.ki * error * dt;
    const float raw         = ff + p_term + i_tentative + d_term;

    float output;
    if (raw > PID_OUTPUT_MAX && error > 0.0f) {
        output = PID_OUTPUT_MAX;        /* freeze integrator */
    } else if (raw < PID_OUTPUT_MIN && error < 0.0f) {
        output = PID_OUTPUT_MIN;        /* freeze integrator */
    } else {
        s->integrator = i_tentative;
        output = clamp(raw, PID_OUTPUT_MIN, PID_OUTPUT_MAX);
    }

    s->prev_temp = temp;
    return output;
}
```

`pid.h`: as shown in the task header (keep the existing doc comments; extend the header comment to mention feedforward + spec ref). Keep `PID_OUTPUT_MIN/MAX` where they are.

`nvs_schema.h`: remove the local `pid_gains_t`, add `#include "control/pid.h"`.

`nvs_schema.c` (bridging until Task 4's v2 blob — prevents uninitialized ff fields):

```c
void pid_defaults(pid_gains_t *out) {
    out->kp     = KP_DEFAULT;
    out->ki     = KI_DEFAULT;
    out->kd     = KD_DEFAULT;
    out->k_hold = KFF_HOLD_DEFAULT;
    out->k_rate = KFF_RATE_DEFAULT;
}
```

In `pid_from_blob()` (still v1 in this task) set `out->k_hold = KFF_HOLD_DEFAULT; out->k_rate = KFF_RATE_DEFAULT;` before `return true;`.

`pid_validate()` — extend (keep the NaN-first pattern):

```c
    if (!isfinite(g->k_hold) || !isfinite(g->k_rate)) return false;
    if (g->k_hold < KFF_HOLD_MIN || g->k_hold > KFF_HOLD_MAX) return false;
    if (g->k_rate < KFF_RATE_MIN || g->k_rate > KFF_RATE_MAX) return false;
```

`control_task.c` call sites:
- line ~157: `pid_set_gains(&s_pid, &s_pid_gains);`
- line ~248: `pid_init(&s_pid, &s_pid_gains);`
- line ~370: `float out = pid_step(&s_pid, setpoint, profile_curve_rate(s_curve, s_sm.elapsed_ms), temp_c, dt);`

- [ ] **Step 5: Run to verify pass (full suite — schema + pid + everything)**

Run: `~/.platformio/penv/bin/pio test -e native`
Expected: PASS. Every pre-existing `test_pid` assertion passing with ff-zero gains is the regression guarantee.

- [ ] **Step 6: Verify the device build still compiles**

Run: `~/.platformio/penv/bin/pio run -e esp32dev`
Expected: SUCCESS (screen_pid/ota_server use `pid_gains_t` by value/field — the two new fields are compatible; suggest handler still fills only 3 fields, fixed in Task 6 — note it compiles because `g` is stack-allocated then validated; add `g.k_hold = KFF_HOLD_DEFAULT; g.k_rate = KFF_RATE_DEFAULT;` right after the three `json_get_float` calls as a stopgap so validation can't read garbage — Task 6 replaces this with required parsing).

- [ ] **Step 7: Commit**

```bash
git add src/control/pid.h src/control/pid.c src/storage/nvs_schema.h src/storage/nvs_schema.c src/control/control_task.c src/ota/ota_server.c test/test_pid/test_pid.c
git commit -m "feat(control): feedforward inside pid_step with anti-windup on the sum"
```

---

### Task 4: NVS schema v2 with v1 migration

**Files:**
- Modify: `include/config.h` (`PID_SCHEMA_VERSION` 1 → 2), `src/storage/nvs_schema.h`, `src/storage/nvs_schema.c`
- Test: `test/test_nvs_schema/test_nvs_schema.c`

- [ ] **Step 1: Write the failing tests** (append + register)

```c
void test_pid_v2_round_trip_includes_ff(void) {
    pid_gains_t in = { .kp = 2.5f, .ki = 0.035f, .kd = 10.0f,
                       .k_hold = 0.21f, .k_rate = 68.0f };
    pid_v2_t blob;
    pid_to_blob(&in, &blob);
    pid_gains_t out;
    TEST_ASSERT_TRUE(pid_from_blob(&blob, sizeof(blob), &out));
    TEST_ASSERT_EQUAL_FLOAT(0.21f, out.k_hold);
    TEST_ASSERT_EQUAL_FLOAT(68.0f, out.k_rate);
}

/* A stored v1 blob (pre-OTA device state) migrates: kp/ki/kd kept, ff = defaults. */
void test_pid_v1_blob_migrates_with_ff_defaults(void) {
    pid_v1_t v1 = { .version = 1, .kp = 1.55f, .ki = 0.011f, .kd = 15.96f };
    pid_gains_t out;
    TEST_ASSERT_TRUE(pid_from_blob(&v1, sizeof(v1), &out));
    TEST_ASSERT_EQUAL_FLOAT(1.55f, out.kp);
    TEST_ASSERT_EQUAL_FLOAT(KFF_HOLD_DEFAULT, out.k_hold);
    TEST_ASSERT_EQUAL_FLOAT(KFF_RATE_DEFAULT, out.k_rate);
}

void test_pid_validate_rejects_out_of_range_ff(void) {
    pid_gains_t g; pid_defaults(&g);
    g.k_hold = KFF_HOLD_MAX + 0.5f;
    TEST_ASSERT_FALSE(pid_validate(&g));
    pid_defaults(&g); g.k_rate = -1.0f;
    TEST_ASSERT_FALSE(pid_validate(&g));
    pid_defaults(&g); g.k_rate = NAN;
    TEST_ASSERT_FALSE(pid_validate(&g));
}
```

(The existing `test_pid_round_trip` keeps compiling once `pid_to_blob` takes `pid_v2_t` — update its local `pid_v1_t blob;` to `pid_v2_t blob;`.)

- [ ] **Step 2: Run to verify failure**

Run: `~/.platformio/penv/bin/pio test -e native -f test_nvs_schema`
Expected: FAIL — `pid_v2_t` undefined.

- [ ] **Step 3: Implement**

`config.h`: `#define PID_SCHEMA_VERSION 2`.

`nvs_schema.h` — keep `pid_v1_t` (needed to size/read legacy blobs), add:

```c
typedef struct __attribute__((packed)) {
    uint8_t version;             /* = 2 */
    float   kp, ki, kd;
    float   k_hold, k_rate;      /* v2 addition: feedforward (spec §3) */
} pid_v2_t;

void pid_to_blob(const pid_gains_t *g, pid_v2_t *out);
```

`nvs_schema.c`:

```c
void pid_to_blob(const pid_gains_t *g, pid_v2_t *out) {
    out->version = PID_SCHEMA_VERSION;
    out->kp = g->kp;  out->ki = g->ki;  out->kd = g->kd;
    out->k_hold = g->k_hold;  out->k_rate = g->k_rate;
}

bool pid_from_blob(const void *blob, size_t blob_len, pid_gains_t *out) {
    if (blob_len == sizeof(pid_v2_t)) {
        const pid_v2_t *b = (const pid_v2_t *)blob;
        if (b->version != 2) return false;
        out->kp = b->kp;  out->ki = b->ki;  out->kd = b->kd;
        out->k_hold = b->k_hold;  out->k_rate = b->k_rate;
        return true;
    }
    if (blob_len == sizeof(pid_v1_t)) {   /* legacy record: migrate */
        const pid_v1_t *b = (const pid_v1_t *)blob;
        if (b->version != 1) return false;
        out->kp = b->kp;  out->ki = b->ki;  out->kd = b->kd;
        out->k_hold = KFF_HOLD_DEFAULT;    /* ff introduced in v2 */
        out->k_rate = KFF_RATE_DEFAULT;
        return true;
    }
    return false;
}
```

Check `src/storage/nvs_store.c` (`nvs_store_save_pid`) for `pid_v1_t` usage and switch it to `pid_v2_t`.

**Migration note for the reviewer:** the device currently stores Kp 1.55 / Ki 0.011 / Kd 15.96 (run-1 gains). After this OTA those migrate with ff defaults — the *intended* post-flash state is the config defaults + ff, so the calibration run instructions (Task 11) include re-applying the no-overshoot set via the UI.

- [ ] **Step 4: Run to verify pass**

Run: `~/.platformio/penv/bin/pio test -e native`
Expected: PASS (full suite).

- [ ] **Step 5: Commit**

```bash
git add include/config.h src/storage/nvs_schema.h src/storage/nvs_schema.c src/storage/nvs_store.c test/test_nvs_schema/test_nvs_schema.c
git commit -m "feat(storage): pid NVS blob v2 with feedforward fields + v1 migration"
```

---

### Task 5: control_task — publish active gains + ambient for readers

**Files:**
- Modify: `src/oven_state.h`, `src/control/control_task.c`, `src/control/control_task.h`

(No host test — `control_task.c` is excluded from `[env:native]`; verified by device build + Task 6's endpoint + hardware.)

- [ ] **Step 1: Add published fields**

`oven_state.h` (inside the published-state struct, near `pending_pid`):

```c
    /* Active PID/ff gains + the ambient reference captured at run start —
     * published for GET /profile so run archives are self-describing. */
    pid_gains_t pid_active_gains;
    float       pid_ambient_ref_c;
```

- [ ] **Step 2: Publish on the three mutation points in `control_task.c`**

Add a helper above the task loop:

```c
static void publish_pid_info(void) {
    xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
    s_pub->pid_active_gains  = s_pid_gains;
    s_pub->pid_ambient_ref_c = s_pid.ambient_ref;
    xSemaphoreGive(s_pub->mutex);
}
```

Call it: (a) right after `pid_init(&s_pid, &s_pid_gains);` at task start (~line 248); (b) right after `pid_set_gains(&s_pid, &s_pid_gains);` in `CMD_APPLY_PID` (~line 157); (c) right after `pid_reset(&s_pid, temp_c);` on the run-start edge (~line 361) — that's where `ambient_ref` becomes current.

- [ ] **Step 3: Accessor**

`control_task.h`:

```c
/* Snapshot of the active PID/ff gains and the ambient reference captured at
 * the current/last run start. Safe from any task (mutex). */
void control_get_gains(pid_gains_t *out_gains, float *out_ambient_c);
```

`control_task.c` (next to `control_get_curve`, same `s_pub` boot-window guard):

```c
void control_get_gains(pid_gains_t *out_gains, float *out_ambient_c) {
    if (!s_pub) {
        if (out_gains) pid_defaults(out_gains);
        if (out_ambient_c) *out_ambient_c = 0.0f;
        return;
    }
    xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
    if (out_gains)     *out_gains     = s_pub->pid_active_gains;
    if (out_ambient_c) *out_ambient_c = s_pub->pid_ambient_ref_c;
    xSemaphoreGive(s_pub->mutex);
}
```

(`pid_defaults` needs `storage/nvs_schema.h` — already included by control_task.)

- [ ] **Step 4: Device build**

Run: `~/.platformio/penv/bin/pio run -e esp32dev`
Expected: SUCCESS.

- [ ] **Step 5: Commit**

```bash
git add src/oven_state.h src/control/control_task.c src/control/control_task.h
git commit -m "feat(control): publish active gains + ambient ref; control_get_gains accessor"
```

---

### Task 6: HTTP — /profile gains+ambient, 5-field /suggest-gains, API v2

**Files:**
- Modify: `include/config.h` (`TELEMETRY_API_VERSION` 1 → 2), `src/ota/ota_server.c`

- [ ] **Step 1: Bump the API version**

`config.h`: `#define TELEMETRY_API_VERSION 2`. Also update `const API_EXPECTED = 1` in `src/web/app.js` to `2` **in the same commit** (the SPA is embedded — they move together; a cached stale page then shows the existing mismatch banner and disables Suggest, per spec §5).

- [ ] **Step 2: Extend `profile_handler`**

In `src/ota/ota_server.c` `profile_handler` (~line 168), fetch gains and widen the head buffer:

```c
    pid_gains_t g; float amb;
    control_get_gains(&g, &amb);
    char head[320];
    int n = snprintf(head, sizeof head,
        "{\"api\":%d,"
        "\"gains\":{\"kp\":%.4f,\"ki\":%.5f,\"kd\":%.3f,"
        "\"k_hold\":%.4f,\"k_rate\":%.2f},"
        "\"ambient_ref_c\":%.2f,"
        "\"step_ms\":%d,\"len\":%u,\"phase_end_ms\":[",
        TELEMETRY_API_VERSION, g.kp, g.ki, g.kd, g.k_hold, g.k_rate,
        amb, PROFILE_CURVE_STEP_MS, (unsigned)len);
```

(Rest of the chunked emit unchanged.)

- [ ] **Step 3: Require 5 fields in `suggest_gains_handler`**

Replace the 3-field parse (and delete Task 3's stopgap defaults):

```c
    pid_gains_t g;
    if (!json_get_float(buf, "\"kp\"", &g.kp) ||
        !json_get_float(buf, "\"ki\"", &g.ki) ||
        !json_get_float(buf, "\"kd\"", &g.kd) ||
        !json_get_float(buf, "\"k_hold\"", &g.k_hold) ||
        !json_get_float(buf, "\"k_rate\"", &g.k_rate)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "missing kp/ki/kd/k_hold/k_rate");
    }
```

Body-length guard: the 5-field body is ~110 bytes — raise the `content_len > 127` limit to `191` and `char buf[192]`.

- [ ] **Step 4: Build + host suite**

Run: `~/.platformio/penv/bin/pio run -e esp32dev && ~/.platformio/penv/bin/pio test -e native`
Expected: both SUCCESS/PASS.

- [ ] **Step 5: Commit**

```bash
git add include/config.h src/ota/ota_server.c src/web/app.js
git commit -m "feat(api): v2 — /profile reports gains+ambient, /suggest-gains takes ff fields"
```

---

### Task 7: Touchscreen — K.hold / K.rate rows + 5-value confirm dialog

**Files:**
- Modify: `src/ui/screen_pid.c`, `src/ui/screen_pid_tuning.c`

- [ ] **Step 1: Extend `screen_pid.c`**

- Enum: `typedef enum { F_KP, F_KI, F_KD, F_KHOLD, F_KRATE, F_COUNT_ } pid_field_id_t;`
- `refresh_label()`: extend the value pick:

```c
    float v = (id == F_KP) ? s_buf.kp : (id == F_KI) ? s_buf.ki
            : (id == F_KD) ? s_buf.kd : (id == F_KHOLD) ? s_buf.k_hold
            : s_buf.k_rate;
```

- `keypad_done()`: add `else if (id == F_KHOLD) s_buf.k_hold = value; else if (id == F_KRATE) s_buf.k_rate = value;` before the final `else`.
- `field_clicked_cb()` switch — two cases, same shape as F_KD:

```c
    case F_KHOLD: cfg.field_label = "K.hold"; cfg.unit = "%/C";
        cfg.min = KFF_HOLD_MIN; cfg.max = KFF_HOLD_MAX; cfg.initial = s_buf.k_hold; break;
    case F_KRATE: cfg.field_label = "K.rate"; cfg.unit = "%s/C";
        cfg.min = KFF_RATE_MIN; cfg.max = KFF_RATE_MAX; cfg.initial = s_buf.k_rate; break;
```

- In the screen-create function: two more `make_sub_row(card, <y>, "K.hold", "%/C", F_KHOLD)` / `("K.rate", "%s/C", F_KRATE)` calls following the existing Kd row (advance y by `ROW_H` each), and grow the card height by `2 * ROW_H` (same arithmetic the existing rows use).
- `refresh_label` formats `%.2f` — fine for k_hold (0.19) and k_rate (71.00).

- [ ] **Step 2: Extend the confirm dialog in `screen_pid_tuning.c`**

The `snprintf` at ~line 173 gains the ff fields:

```c
    snprintf(msg, sizeof msg,
             "Kp %.2f  Ki %.3f  Kd %.1f\nK.hold %.2f  K.rate %.0f",
             s_confirm_gains.kp, s_confirm_gains.ki, s_confirm_gains.kd,
             s_confirm_gains.k_hold, s_confirm_gains.k_rate);
```

(Widen the local `msg` buffer if needed for the second line.)

- [ ] **Step 3: Device build**

Run: `~/.platformio/penv/bin/pio run -e esp32dev`
Expected: SUCCESS. (LVGL pool headroom is a hardware check — Task 11.)

- [ ] **Step 4: Commit**

```bash
git add src/ui/screen_pid.c src/ui/screen_pid_tuning.c
git commit -m "feat(ui): K.hold/K.rate rows on PID screen + 5-value suggest confirm"
```

---

### Task 8: fit.js — BOUNDS sync + k_rate derivation

**Files:**
- Modify: `src/web/fit.js`
- Test: `test/web/fit.test.mjs`

- [ ] **Step 1: Write the failing tests** (append)

```js
test('computeGains derives k_rate = dCO/N (inverse plant gain)', () => {
    /* N=0.8 °C/s at ΔCO=50 % -> R=0.016 -> k_rate = 1/R = 62.5 %·s/°C */
    const g = computeGains({ N: 0.8, dCO: 50, L: 20, rule: 'zn' });
    assert.ok(Math.abs(g.k_rate - 62.5) < 1e-3, `k_rate=${g.k_rate}`);
});

test('k_rate clamps to KFF_RATE_MAX on a degenerate slow fit', () => {
    /* Tiny N -> huge 1/R; must clamp to the firmware bound (300). */
    const g = computeGains({ N: 0.01, dCO: 80, L: 20, rule: 'zn' });
    assert.equal(g.k_rate, 300);
});

test('BOUNDS mirror config.h', () => {
    assert.equal(BOUNDS.KI_MAX, 1.0);        /* KI_MAX shrank in config.h */
    assert.equal(BOUNDS.KFF_RATE_MAX, 300);
    assert.equal(BOUNDS.KFF_HOLD_MAX, 1.0);
});
```

Add `BOUNDS` to the test file's import list.

- [ ] **Step 2: Run to verify failure**

Run: `node --test test/web/`
Expected: FAIL — `k_rate` undefined / BOUNDS mismatch.

- [ ] **Step 3: Implement in `fit.js`**

```js
export const BOUNDS = { KP_MAX: 100, KI_MAX: 1.0, KD_MAX: 50,
                        KFF_HOLD_MAX: 1.0, KFF_RATE_MAX: 300 };
```

In `computeGains`, next to the R computation, and in the return:

```js
    const kRate = clamp(dCO / N, BOUNDS.KFF_RATE_MAX);   // 1/R: %·s/°C
    ...
    return {
        kp: clamp(kp, BOUNDS.KP_MAX),
        ki: clamp(ki, BOUNDS.KI_MAX),
        kd: clamp(kd, BOUNDS.KD_MAX),
        k_rate: kRate,
    };
```

(The zero-gains degenerate-fit early return gains `k_rate: 0`.)

- [ ] **Step 4: Run to verify pass**

Run: `node --test test/web/`
Expected: PASS (all, incl. pre-existing).

- [ ] **Step 5: Commit**

```bash
git add src/web/fit.js test/web/fit.test.mjs
git commit -m "feat(web): derive k_rate from every step-test fit; sync BOUNDS"
```

---

### Task 9: fit.js — k_hold estimator from reflow runs

**Files:**
- Modify: `src/web/fit.js`
- Test: `test/web/fit.test.mjs`

- [ ] **Step 1: Write the failing tests** (append)

```js
import { estimateKHold } from '../../src/web/fit.js';   // extend existing import

/* Synthetic reflow run generated FROM the model with known k_hold: ramp
 * 0.5 °C/s from 25->150 then hold. duty = k_hold*(T-amb) + k_rate*slope. */
function synthRun({ kHold = 0.2, kRate = 70, amb = 25, dt = 1.25, n = 400 }) {
    const s = [];
    let T = amb;
    for (let i = 0; i < n; i++) {
        const t = i * dt;
        const slope = T < 150 ? 0.5 : 0;
        T = Math.min(150, amb + 0.5 * t);
        s.push({ t_s: t, temp_c: T, duty: kHold * (T - amb) + kRate * slope,
                 setpoint_c: T });
    }
    return s;
}

test('estimateKHold recovers a known k_hold from a synthetic run', () => {
    const est = estimateKHold(synthRun({}), { kRate: 70, ambientRef: 25, activeEndS: 500 });
    assert.equal(est.ok, true);
    assert.ok(Math.abs(est.median - 0.2) < 0.02, `median=${est.median}`);
    assert.ok(est.iqr < 0.05, `iqr=${est.iqr}`);
});

test('estimateKHold rejects a run with too few qualifying samples', () => {
    /* All samples below ambient+30 -> filtered out -> not ok. */
    const cold = Array.from({ length: 100 }, (_, i) =>
        ({ t_s: i, temp_c: 30, duty: 10, setpoint_c: 30 }));
    assert.equal(estimateKHold(cold, { kRate: 70, ambientRef: 25, activeEndS: 500 }).ok, false);
});

test('estimateKHold flags low confidence on scattered data', () => {
    /* Alternate duty between model value and model+30 -> wide IQR. */
    const s = synthRun({}).map((p, i) => i % 2 ? { ...p, duty: p.duty + 30 } : p);
    const est = estimateKHold(s, { kRate: 70, ambientRef: 25, activeEndS: 500 });
    assert.equal(est.ok, true);
    assert.equal(est.lowConfidence, true);
});
```

- [ ] **Step 2: Run to verify failure**

Run: `node --test test/web/`
Expected: FAIL — `estimateKHold` not exported.

- [ ] **Step 3: Implement in `fit.js`**

```js
/* Estimate k_hold (holding duty per °C above ambient) from a reflow run by
 * inverting the plant model per sample:
 *     k_hold(t) = (duty − k_rate·slope) / (temp − ambient_ref)
 * Qualifying filters (spec §7): active phases only (t ≤ activeEndS), heater
 * on, temp ≥ ambient+30 °C (divide-by-small noise guard), slope from a ±2
 * sample (~5 s) centered window. Median + IQR because the linear loss model
 * is approximate (wall-mass absorption early, super-linear radiation high) —
 * per-segment estimates genuinely scatter; a robust median with visible
 * spread is honest, a blind mean is not. lowConfidence: IQR > 0.10 (±0.05). */
export function estimateKHold(samples, { kRate, ambientRef, activeEndS }) {
    if (!(kRate > 0) || !Number.isFinite(ambientRef) || !samples) return { ok: false };
    const vals = [];
    for (let i = 2; i < samples.length - 2; i++) {
        const s = samples[i];
        if (!(s.t_s <= activeEndS) || !(s.duty > 0)) continue;
        if (!(s.temp_c >= ambientRef + 30)) continue;
        const a = samples[i - 2], b = samples[i + 2];
        if (!(b.t_s > a.t_s)) continue;
        const slope = (b.temp_c - a.temp_c) / (b.t_s - a.t_s);
        const k = (s.duty - kRate * slope) / (s.temp_c - ambientRef);
        if (Number.isFinite(k)) vals.push(k);
    }
    if (vals.length < 20) return { ok: false };
    vals.sort((x, y) => x - y);
    const q = p => vals[Math.floor(p * (vals.length - 1))];
    const median = q(0.5), iqr = q(0.75) - q(0.25);
    return { ok: true, median, iqr, n: vals.length, lowConfidence: iqr > 0.10 };
}
```

- [ ] **Step 4: Run to verify pass**

Run: `node --test test/web/`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/web/fit.js test/web/fit.test.mjs
git commit -m "feat(web): k_hold estimator (median+IQR) from reflow-run data"
```

---

### Task 10: app.js + tuning.html — self-describing archive, split metrics, k_hold suggest

**Files:**
- Modify: `src/web/app.js`, `src/web/tuning.html`

(Pure logic was tested in Tasks 8–9; this task is wiring. Manual verification via `tools/web_sim.py` in Step 6.)

- [ ] **Step 1: Fetch + hold fw version and the enriched profile**

Top-level in `app.js`:

```js
let fwVersion = null;
async function fetchVersion() {
    try { fwVersion = (await (await fetch('/version', { cache: 'no-store' })).json()).version ?? null; }
    catch { /* leave null; archive stores null */ }
}
fetchVersion();   // once per page session, alongside the first poll() kick-off
```

`fetchProfile()` needs no change — the enriched response (gains, ambient_ref_c) rides in `plannedCurve` automatically. Add a setpoint helper next to it:

```js
/* Planned setpoint at t seconds (curve is sampled at step_ms). */
function setpointAt(p, tS) {
    const i = Math.min(Math.round(tS * 1000 / p.step_ms), p.temp_c.length - 1);
    return p.temp_c[Math.max(0, i)];
}
```

- [ ] **Step 2: Replace `verifyRun()` with per-phase metrics**

```js
/* Per-phase verification metrics (spec §9). phase_end_ms indices follow the
 * firmware stage enum: [none, heat1, soak, heat2, reflow, cool]. */
function computeRunMetrics(samps, planned) {
    const out = { above_liquidus_s: 0 };
    let dt = 0;
    for (let i = 0; i < samps.length; i++) {
        if (i > 0) dt = samps[i].t_s - samps[i - 1].t_s;
        if (samps[i].temp_c >= LIQUIDUS_C) out.above_liquidus_s += dt;
    }
    let lastHeat = -1;
    for (let i = 0; i < samps.length; i++) if (samps[i].duty > 0) lastHeat = i;
    if (lastHeat >= 0) {
        let peak = -Infinity;
        for (let i = lastHeat; i < samps.length; i++) peak = Math.max(peak, samps[i].temp_c);
        out.coast = peak - samps[lastHeat].temp_c;
    }
    if (!planned || !Array.isArray(planned.phase_end_ms)) return out;  // liquidus+coast only
    const pe = planned.phase_end_ms.map(ms => ms / 1000);
    const win = (a, b) => samps.filter(s => s.t_s > a && s.t_s <= b);
    const lag = seg => {
        const e = seg.map(s => (s.setpoint_c ?? 0) - s.temp_c);
        return e.length ? { mean: e.reduce((x, y) => x + y, 0) / e.length,
                            max: Math.max(...e) } : null;
    };
    const r1 = lag(win(0, pe[1])), r2 = lag(win(pe[2], pe[3]));
    if (r1) { out.ramp1_lag_mean = r1.mean; out.ramp1_lag_max = r1.max; }
    if (r2) { out.ramp2_lag_mean = r2.mean; out.ramp2_lag_max = r2.max; }
    const soak = win(pe[1], pe[2]);
    if (soak.length)
        out.soak_excursion = Math.max(...soak.map(s => s.temp_c))
                           - setpointAt(planned, (pe[1] + pe[2]) / 2);
    const lastT = samps.length ? samps[samps.length - 1].t_s : pe[4];
    const refl = win(pe[3], lastT);
    if (refl.length)
        out.peak_overshoot = Math.max(...refl.map(s => s.temp_c))
                           - setpointAt(planned, (pe[3] + pe[4]) / 2);
    const est = estimateKHold(samps, { kRate: planned.gains?.k_rate,
                                       ambientRef: planned.ambient_ref_c,
                                       activeEndS: pe[4] });
    if (est.ok) { out.k_hold_est = est.median; out.k_hold_iqr = est.iqr;
                  out.k_hold_low_confidence = !!est.lowConfidence; }
    return out;
}

function profileSummary(p) {
    const pe = p.phase_end_ms.map(ms => ms / 1000);
    const bp = [[0, setpointAt(p, 0)]];
    for (let i = 1; i < pe.length; i++) bp.push([pe[i], setpointAt(p, pe[i])]);
    return { phase_end_s: { heat1: pe[1], soak: pe[2], heat2: pe[3],
                            reflow: pe[4], cool: pe[5] },
             breakpoints: bp };
}

function verifyRun() {
    const m = computeRunMetrics(samples, plannedCurve);
    lastMetrics = m;   // module-level: let the k_hold suggest button reuse it
    const est = (m.k_hold_est != null)
        ? ` · k_hold est ${m.k_hold_est.toFixed(3)} ±${(m.k_hold_iqr / 2).toFixed(3)}`
          + (m.k_hold_low_confidence ? ' ⚠' : '')
        : '';
    $('verifyText').textContent =
        ` soak +${(m.soak_excursion ?? 0).toFixed(1)}°`
      + ` · peak +${(m.peak_overshoot ?? 0).toFixed(1)}°`
      + ` · >${LIQUIDUS_C}°C ${(m.above_liquidus_s ?? 0).toFixed(0)} s` + est;
    $('verify').style.display = 'block';
    $('kholdSuggest').style.display =
        (m.k_hold_est != null && plannedCurve?.gains) ? 'inline-block' : 'none';
    saveRun({ kind: 'reflow', fw: fwVersion,
              gains: plannedCurve?.gains ?? null,
              ambient_ref_c: plannedCurve?.ambient_ref_c ?? null,
              profile: (plannedCurve && Array.isArray(plannedCurve.phase_end_ms))
                       ? profileSummary(plannedCurve) : null,
              metrics: m });
    drawChart();
}
```

Declare `let lastMetrics = null;` with the other module-level state; add `estimateKHold` to the `fit.js` import. Also record `fw: fwVersion` in `finalizeRun`'s step-test `saveRun` call (spec §8: step runs gain `fw` only).

- [ ] **Step 3: Suggest paths carry 5 fields**

Rework `suggest()` (step-test path) — fetch current k_hold, POST all five:

```js
async function currentOvenGains() {
    try { return (await (await fetch('/profile', { cache: 'no-store' })).json()).gains ?? null; }
    catch { return null; }
}

async function suggest() {
    const g = recompute(); if (!g) return;
    const cur = await currentOvenGains();
    g.k_hold = cur?.k_hold ?? 0.19;   // keep the oven's k_hold; fit has no basis for it
    const token = prompt('OTA token:'); if (!token) return;
    const r = await fetch('/suggest-gains', {
        method: 'POST', headers: { 'X-OTA-Token': token, 'Content-Type': 'application/json' },
        body: JSON.stringify(g),
    });
    showBanner(r.ok ? 'sent — confirm on the touchscreen' : 'rejected (' + r.status + ')');
}

/* Reflow-run path: push the k_hold estimate, keep everything else current.
 * Never auto-applied — same POST + touchscreen confirm as any suggestion. */
async function suggestKHold() {
    if (!lastMetrics || lastMetrics.k_hold_est == null) return;
    const cur = await currentOvenGains();
    if (!cur) { showBanner('oven unreachable'); return; }
    const body = { ...cur, k_hold: +lastMetrics.k_hold_est.toFixed(3) };
    const token = prompt('OTA token:'); if (!token) return;
    const r = await fetch('/suggest-gains', {
        method: 'POST', headers: { 'X-OTA-Token': token, 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
    });
    showBanner(r.ok ? 'sent — confirm on the touchscreen' : 'rejected (' + r.status + ')');
}
```

Wire the click handler where the other buttons are bound: `$('kholdSuggest').onclick = suggestKHold;`

- [ ] **Step 4: tuning.html + gain readouts**

- In the verify readout container (`id=verify`), add:
  `<button id=kholdSuggest style="display:none">apply k_hold est →</button>`
- In the gains card (below the Kd row):
  `<div class=row><span>K.rate</span><b id=g_krate>—</b></div>`
- In `recompute()` add `$('g_krate').textContent = g.k_rate.toFixed(0);`

- [ ] **Step 5: Archive list renders both shapes**

In `renderArchive()`, the reflow branch:

```js
        if (r.kind === 'reflow') {
            meta = 'reflow' + (r.fw ? ' · ' + r.fw : '');
            const m = r.metrics;
            detail = m
                ? `soak +${(m.soak_excursion ?? 0).toFixed(1)}° · peak +${(m.peak_overshoot ?? 0).toFixed(1)}° · >${LIQUIDUS_C}°C ${(m.above_liquidus_s ?? 0).toFixed(0)}s`
                : `overshoot ${(r.overshoot ?? 0).toFixed(1)}° · >${LIQUIDUS_C}°C ${(r.aboveLiquidus ?? 0).toFixed(0)}s`;   // legacy pre-v2 archives
        }
```

- [ ] **Step 6: Manual verification against the simulator**

Run: `python3 tools/web_sim.py` and exercise: a simulated reflow run end → verify readout shows the split metrics and (if the sim produces enough qualifying samples) the k_hold estimate; export a run → JSON contains `fw`, `gains`, `ambient_ref_c`, `profile.breakpoints`, `metrics`; legacy archived runs still render. Then run `node --test test/web/` once more.
Expected: all of the above; tests PASS.

- [ ] **Step 7: Commit**

```bash
git add src/web/app.js src/web/tuning.html
git commit -m "feat(web): self-describing run archive, per-phase metrics, k_hold suggest"
```

---

### Task 11: Final verification + hardware handoff

**Files:**
- Modify: `docs/superpowers/specs/2026-07-19-feedforward-design.md` (status line only)

- [ ] **Step 1: Full test sweep**

Run: `~/.platformio/penv/bin/pio test -e native && node --test test/web/ && ~/.platformio/penv/bin/pio run -e esp32dev`
Expected: all PASS / build SUCCESS.

- [ ] **Step 2: Update spec status**

Change the spec's `**Status:**` line to `implemented (pending hardware verification)`.

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/specs/2026-07-19-feedforward-design.md
git commit -m "docs(spec): mark feedforward design implemented pending hardware verify"
```

- [ ] **Step 4: Hardware checklist (manual — requires the maintainer + the oven; do NOT automate)**

1. OTA flash per the established workflow (touch CMakeLists for the version bump, `tools/ota_upload.py`, token from upload.conf; verify booted ELF SHA256 matches the build before debugging anything).
2. Confirm NVS migration: PID screen shows migrated Kp 1.55 / Ki 0.011 / Kd 15.96 + K.hold 0.19 / K.rate 71. Then apply the intended gentle set (Kp 0.44 / Ki 0.0068 / Kd 4.75) via the touchscreen.
3. LVGL: open every screen incl. the extended PID screen; no freeze (a silent while(1) here means the 64 KB pool overflowed — see memory note).
4. Web page: `api v2` chip, gains card shows 5 values, `/profile` in devtools carries `gains` + `ambient_ref_c`.
5. **Calibration reflow run:** expect ff to carry the duty (integrator small). Afterwards check the k_hold estimate in the verify readout; if confidence is good and it differs from 0.19, apply it via the new button. Archive-export the run into `dev_ressources/oven/` for analysis.

---

## Self-review notes (completed)

- **Spec coverage:** §1→Task 3, §2→Task 2, §3→Task 4, §4→Task 1, §5→Tasks 5–6, §6→Task 7, §7→Tasks 8–9, §8–§9→Task 10, §10→Tasks 2–4, 8–9, 11. No gaps.
- **Type consistency:** `pid_gains_t` 5 fields defined once in Task 3 and used in 4/5/6/7; `profile_curve_rate` defined Task 2, consumed Task 3 (control_task edit); `estimateKHold({kRate, ambientRef, activeEndS})` defined Task 9, called with the same keys in Task 10; `control_get_gains(pid_gains_t*, float*)` defined Task 5, called in Task 6.
- **Known judgment call:** Task 3 briefly leaves `/suggest-gains` filling ff fields with defaults (stopgap) until Task 6 requires them — documented in both tasks.

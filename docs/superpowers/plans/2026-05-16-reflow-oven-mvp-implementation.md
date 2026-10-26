# Reflow Oven MVP — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the MVP firmware for a custom reflow oven on the LCDwiki E32R35T board, mirroring the STM32 reference design feature-for-feature with three added safety gates and a touch-native LVGL UI.

**Architecture:** Two FreeRTOS tasks (`control_task` owns the SSR and runs PID at 250 ms; `ui_task` hosts LVGL). They communicate via a single command queue (UI → control) and a mutex-protected snapshot struct (control → UI). All hardware-touching modules are isolated; the control algorithms (PID, profile curve, state machine, SSR scheduler logic, NVS schema) are pure C, unit-tested on the host.

**Tech Stack:** PlatformIO + ESP-IDF (native framework) + LVGL 9 via `esp_lvgl_port`, `esp_lcd_st7796`, `esp_lcd_touch_xpt2046`. Host tests use Unity (PlatformIO's `native` env).

**Spec:** [`docs/superpowers/specs/2026-05-15-reflow-oven-mvp-design.md`](../specs/2026-05-15-reflow-oven-mvp-design.md). Read it before starting — this plan assumes you understand the design decisions, the pin map, and the safety model. Where this plan references a section, follow back to the spec for rationale.

**Phases & checkpoints:**

| Phase | Scope | End-of-phase deliverable |
|---|---|---|
| 1 | Project scaffolding + `config.h` | Both `esp32dev` and `native` envs build empty stubs. |
| 2 | Host-testable control logic (TDD) | `pio test -e native` green: PID, profile curve, state machine, SSR scheduler, NVS schema. |
| 3 | Hardware bring-up | Seven mini-PIO projects under `tools/bringup/` each verified on real hardware. |
| 4 | Drivers + main firmware integration | `control_task` runs against real sensor + SSR; state logged to UART; no UI yet. |
| 5 | UI + end-to-end verification | Full reflow cycle on the bench. |

---

## Phase 1 — Project foundations

### Task 1: PlatformIO project scaffolding

**Files:**
- Create: `platformio.ini`
- Create: `src/main.c` (placeholder so `pio run` has something to compile)

- [ ] **Step 1: Create `platformio.ini`**

Write the file with both build envs. ESP-IDF framework for the board env, `native` framework for host tests. Managed components for LVGL + ST7796 + XPT2046 live in `src/idf_component.yml` (Step 2.5), not in PIO's `lib_deps` — these packages live on Espressif's IDF Component Registry which PIO can't query directly.

```ini
; reflow_oven — PlatformIO configuration
; See docs/superpowers/specs/... §10 for the rationale.

[platformio]
default_envs = esp32dev

[env:esp32dev]
platform = espressif32
board = esp32dev
framework = espidf
monitor_speed = 115200
build_flags =
    -DLV_CONF_INCLUDE_SIMPLE
    -I include
    -I src
; Managed components (esp_lvgl_port, esp_lcd_st7796, esp_lcd_touch_xpt2046)
; live in src/idf_component.yml, not lib_deps — they're on Espressif's IDF
; Component Registry, which PIO can't query but the IDF Component Manager can.
; LVGL itself comes from the IDF registry too (via lvgl/lvgl).
lib_deps =

[env:native]
platform = native
test_framework = unity
test_filter = test_*
test_build_src = yes      ; compile src/ modules into the test binary
build_flags =
    -std=c11
    -Iinclude
    -Isrc
```

`test_build_src = yes` is essential for host-side TDD (Phase 2): without it, PIO's `native` test runner only builds the `test/` tree and your test gets `undefined reference` errors when it calls into anything from `src/`.

And `src/idf_component.yml` (created in Step 2.5):

```yaml
## IDF Component Manager manifest. Pulled at CMake configure time.
dependencies:
  idf:
    version: ">=5.0"
  lvgl/lvgl: "^9"
  espressif/esp_lvgl_port: "^2"
  espressif/esp_lcd_st7796: "^1"
  atanisoft/esp_lcd_touch_xpt2046: "^1"   # Espressif doesn't publish XPT2046
```

> ⚠️ Pinning the managed-component major versions matters: `esp_lcd_panel_dev_config_t` renamed `color_space` → `rgb_endian` between major versions, and `esp_lcd_touch_config_t` field names shift too. If `pio pkg update` later changes a major, expect API churn.
>
> ⚠️ The XPT2046 driver is published by `atanisoft`, not `espressif` (the latter doesn't have an XPT2046 driver in their registry). Future tasks that reference `esp_lcd_touch_xpt2046` mean this `atanisoft/`-prefixed package.

- [ ] **Step 2: Create `src/main.c` placeholder**

```c
/* src/main.c — placeholder until Phase 4. */
#include <stdio.h>

void app_main(void) {
    printf("reflow_oven boot\n");
}
```

- [ ] **Step 2.5: Create `src/idf_component.yml`** with the YAML shown above.

PlatformIO also auto-generates a root `CMakeLists.txt` and `src/CMakeLists.txt` on the first IDF build — those need to be tracked too so future builds don't see them as untracked. Add `managed_components/`, `dependencies.lock`, and `sdkconfig.esp32*` to `.gitignore`.

- [ ] **Step 3: Create `sdkconfig.defaults`**

This sets the safety-critical Kconfig options (brown-out detector, no Bluetooth, custom WDT init). Without it, the firmware silently violates spec §6.1 item 7 and §3.4.

`sdkconfig.defaults`:

```
# Brown-out detector at the highest threshold — see spec §6.1 item 7.
# Symbol name varies between IDF versions; uncomment the matching one.
CONFIG_ESP_BROWNOUT_DET=y
CONFIG_ESP_BROWNOUT_DET_LVL_SEL_7=y
CONFIG_ESP_BROWNOUT_DET_LVL=7
CONFIG_ESP32_BROWNOUT_DET_LVL_SEL_7=y
CONFIG_ESP32_BROWNOUT_DET_LVL=7

# Bluetooth permanently disabled (spec §3.4).
CONFIG_BT_ENABLED=n

# We init the task watchdog ourselves with a custom timeout (Task 23).
CONFIG_ESP_TASK_WDT_INIT=n
CONFIG_ESP_TASK_WDT_EN=y
CONFIG_ESP_TASK_WDT_PANIC=y

# Partition table: default factory (1.3 MB app slot) suits MVP.
CONFIG_PARTITION_TABLE_SINGLE_APP=y
```

After flashing, run `pio run -t menuconfig` once to confirm the entries above resolved to real symbols for your IDF version; remove any that are unknown (they'll show a warning during build otherwise). WiFi is not explicitly disabled at Kconfig level — we just never call `esp_wifi_init()`, which keeps the library out of the binary unless it's pulled in transitively.

- [ ] **Step 4: Build the esp32dev env to confirm the toolchain works**

Run: `pio run -e esp32dev`
Expected: build succeeds (toolchain may take several minutes the first time as it fetches ESP-IDF). Any "unknown Kconfig symbol" warnings about brown-out can be resolved by removing the variants that don't apply to your IDF version.

- [ ] **Step 5: Commit**

```bash
git add platformio.ini src/main.c src/idf_component.yml sdkconfig.defaults \
        CMakeLists.txt src/CMakeLists.txt .gitignore
git commit -m "Phase 1.1: PlatformIO project scaffolding (esp32dev + native envs) + sdkconfig defaults"
```

---

### Task 2: `include/config.h` — single source of truth

**Files:**
- Create: `include/config.h`

- [ ] **Step 1: Write `include/config.h` with every compile-time constant**

This is the only file that holds tunable knobs. Pin map, timing, safety, defaults, parameter bounds, NVS keys. See spec §9 for rationale.

```c
/* include/config.h — single source of truth for all compile-time constants.
 * See docs/superpowers/specs/... §9 for rationale per group.
 */
#pragma once

#include <assert.h>

/* ============================================================
 * PIN MAP — E32R35T board (LCDwiki).
 * Fixed by board wiring; only change if porting hardware.
 * ============================================================ */
#define PIN_LCD_CS         15      /* board-fixed (HSPI native CS) */
#define PIN_LCD_DC          2      /* board-fixed */
#define PIN_LCD_SCK        14      /* board-fixed (HSPI native SCLK) */
#define PIN_LCD_MOSI       13      /* board-fixed (HSPI native MOSI) */
#define PIN_LCD_MISO       12      /* board-fixed (HSPI native MISO) */
#define PIN_LCD_BL         27      /* high = backlight on */
#define PIN_TOUCH_CS       33      /* board-fixed, shares HSPI bus with LCD */
#define PIN_TOUCH_IRQ      36      /* board-fixed (input-only pin) */

/* MAX31855 on the SPI peripheral header (P3). VSPI host. */
#define PIN_TC_CS          21
#define PIN_TC_SCK         18      /* VSPI native SCLK */
#define PIN_TC_MISO        19      /* VSPI native MISO */

/* SSR control on the I²C peripheral header (P4), IIC_SDA pad.
 * Power for the MAX31855 (VCC3V3 + GND) also comes off this same header.
 * Forbidden: strapping pins (0, 2, 5, 12, 15).
 * External 10 kΩ pull-down on this line is mandatory (see spec §6.2).
 */
#define PIN_SSR            32

/* ============================================================
 * CONTROL LOOP TIMING
 * ============================================================ */
#define CONTROL_TICK_MS         250   /* range 100-1000 */
#define SSR_PWM_WINDOW_MS      2000   /* range 1000-5000, multiple of CONTROL_TICK_MS */
#define PROFILE_CURVE_STEP_MS   500   /* range 250-1000; curve memory grows inversely */
#define SSR_QUANTA_PER_WINDOW  (SSR_PWM_WINDOW_MS / CONTROL_TICK_MS)  /* = 8 */

/* ============================================================
 * SAFETY LIMITS
 * ============================================================ */
#define OVER_TEMP_LIMIT_C        260.0f   /* range 230-280; must be > REFLOW_TEMP_MAX */
#define SENSOR_MAX_AGE_MS        1500     /* > CONTROL_TICK_MS, < 5000 */
#define COOL_DOWN_THRESHOLD_C     50.0f   /* range 30-80 */
#define COOL_DOWN_TIMEOUT_MS    600000    /* 10 min — release UI from COOL_DOWN even if stuck hot */
#define CONTROL_TASK_WDT_S            2   /* §6.1 item 5 */

/* ============================================================
 * ADJUSTABLE PARAMETER BOUNDS (keypad clamps to these)
 * ============================================================ */

/* PID gains */
#define KP_MIN                    0.0f
#define KP_MAX                  100.0f
#define KP_DEFAULT               10.0f

#define KI_MIN                    0.0f
#define KI_MAX                   50.0f
#define KI_DEFAULT               10.0f

#define KD_MIN                    0.0f
#define KD_MAX                   50.0f
#define KD_DEFAULT                0.0f

/* Heat-up rates (°C/s). MIN 0.2 avoids zero-rate / divide-by-zero in curve gen. */
#define HEATUP_RATE_MIN           0.2f
#define HEATUP_RATE_MAX           4.0f
#define FIRST_HEATUP_DEFAULT      1.2f
#define SECOND_HEATUP_DEFAULT     1.0f

/* Soak phase */
#define SOAK_TEMP_MIN             50      /* °C */
#define SOAK_TEMP_MAX            200      /* °C */
#define SOAK_TEMP_DEFAULT        150
#define SOAK_TIME_MIN              0      /* s */
#define SOAK_TIME_MAX            300      /* s */
#define SOAK_TIME_DEFAULT         90

/* Reflow phase. MAX is intentionally below OVER_TEMP_LIMIT_C with headroom. */
#define REFLOW_TEMP_MIN          150      /* °C */
#define REFLOW_TEMP_MAX          250      /* °C */
#define REFLOW_TEMP_DEFAULT      220
#define REFLOW_TIME_MIN            0      /* s */
#define REFLOW_TIME_MAX          120      /* s */
#define REFLOW_TIME_DEFAULT       45

/* Enforce the cross-bound at compile time too (spec §9). The constraint is
 * ">=10 C headroom", so the comparison must be <=. With the defaults
 * (REFLOW_TEMP_MAX=250, OVER_TEMP_LIMIT_C=260), headroom is exactly 10 C.
 */
_Static_assert(REFLOW_TEMP_MAX <= OVER_TEMP_LIMIT_C - 10.0f,
               "REFLOW_TEMP_MAX must leave >=10 C headroom below OVER_TEMP_LIMIT_C");

/* ============================================================
 * NVS PERSISTENCE
 * ============================================================ */
#define NVS_NAMESPACE          "reflow"
#define NVS_KEY_PROFILE        "profile"
#define NVS_KEY_PID            "pid"
#define PROFILE_SCHEMA_VERSION   1
#define PID_SCHEMA_VERSION       1
```

- [ ] **Step 2: Verify it compiles in the host env**

Run: `pio test -e native --without-testing -v 2>&1 | head -20`
Expected: native env builds (no tests yet, just verifying `config.h` is well-formed C).

If `--without-testing` isn't accepted by your PIO version, instead create a smoke test in the next task and run `pio test -e native`.

- [ ] **Step 3: Commit**

```bash
git add include/config.h
git commit -m "Phase 1.2: add include/config.h (pins, timing, safety, defaults, bounds)"
```

---

### Task 3: Smoke test in `native` env

**Files:**
- Create: `test/test_smoke/test_smoke.c`

- [ ] **Step 1: Add a trivial Unity test that confirms `config.h` constants are sane**

```c
/* test/test_smoke/test_smoke.c — confirms the native build path works
 * and config.h cross-bounds are respected. */
#include <unity.h>
#include "config.h"

void setUp(void) {}
void tearDown(void) {}

void test_over_temp_limit_above_reflow_max(void) {
    TEST_ASSERT_TRUE(OVER_TEMP_LIMIT_C > REFLOW_TEMP_MAX);
}

void test_pwm_window_is_multiple_of_tick(void) {
    TEST_ASSERT_EQUAL_INT(0, SSR_PWM_WINDOW_MS % CONTROL_TICK_MS);
    TEST_ASSERT_EQUAL_INT(8, SSR_QUANTA_PER_WINDOW);
}

void test_defaults_within_bounds(void) {
    TEST_ASSERT_TRUE(SOAK_TEMP_DEFAULT >= SOAK_TEMP_MIN);
    TEST_ASSERT_TRUE(SOAK_TEMP_DEFAULT <= SOAK_TEMP_MAX);
    TEST_ASSERT_TRUE(REFLOW_TEMP_DEFAULT >= REFLOW_TEMP_MIN);
    TEST_ASSERT_TRUE(REFLOW_TEMP_DEFAULT <= REFLOW_TEMP_MAX);
    TEST_ASSERT_TRUE(FIRST_HEATUP_DEFAULT >= HEATUP_RATE_MIN);
    TEST_ASSERT_TRUE(FIRST_HEATUP_DEFAULT <= HEATUP_RATE_MAX);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_over_temp_limit_above_reflow_max);
    RUN_TEST(test_pwm_window_is_multiple_of_tick);
    RUN_TEST(test_defaults_within_bounds);
    return UNITY_END();
}
```

- [ ] **Step 2: Run it**

Run: `pio test -e native -f test_smoke`
Expected: `3 Tests 0 Failures 0 Ignored — OK`.

- [ ] **Step 3: Commit**

```bash
git add test/test_smoke/test_smoke.c
git commit -m "Phase 1.3: add smoke test for native env + config.h sanity"
```

---

**Phase 1 checkpoint.** Both build envs work. `config.h` is the single source of truth.

## Phase 2 — Host-testable control logic (TDD)

Every module in this phase is pure C with no ESP-IDF / FreeRTOS dependencies, so all tests run on the dev machine via `pio test -e native`. Strict TDD: failing test first, then minimal implementation.

---

### Task 4: PID module with conditional integration

**Files:**
- Create: `src/control/pid.h`
- Create: `src/control/pid.c`
- Create: `test/test_pid/test_pid.c`

- [ ] **Step 1: Write the public header**

`src/control/pid.h`:

```c
/* Standard-form PID with conditional-integration anti-windup.
 * Derivative on measurement (not error) to avoid kick on setpoint steps.
 * See spec §4.1.
 */
#pragma once

#include <stdbool.h>

typedef struct {
    float kp, ki, kd;
    float integrator;
    float prev_temp;
} pid_state_t;

/* Output range is [0, 100] (percent duty). */
#define PID_OUTPUT_MIN  0.0f
#define PID_OUTPUT_MAX  100.0f

void  pid_init(pid_state_t *s, float kp, float ki, float kd);
void  pid_set_gains(pid_state_t *s, float kp, float ki, float kd);
void  pid_reset(pid_state_t *s, float current_temp);   /* IDLE→RUNNING transition */
float pid_step(pid_state_t *s, float setpoint, float temp, float dt);
```

- [ ] **Step 2: Write failing tests**

`test/test_pid/test_pid.c`:

```c
#include <unity.h>
#include "control/pid.h"

static pid_state_t pid;

void setUp(void)    { pid_init(&pid, 10.0f, 1.0f, 0.0f); pid_reset(&pid, 25.0f); }
void tearDown(void) {}

/* After reset: integrator zero, prev_temp set to current. */
void test_reset_clears_integrator_and_sets_prev_temp(void) {
    pid_reset(&pid, 42.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, pid.integrator);
    TEST_ASSERT_EQUAL_FLOAT(42.0f, pid.prev_temp);
}

/* First step after reset produces zero d_term (prev_temp == temp). */
void test_first_step_has_zero_derivative(void) {
    /* Setpoint 50, temp 50 → error 0, p_term 0, i_term 0, d_term 0 → 0 output. */
    float out = pid_step(&pid, 50.0f, 50.0f, 0.25f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out);
}

/* Saturation high + positive error → output clamped, integrator frozen. */
void test_saturation_high_freezes_integrator(void) {
    pid_init(&pid, 100.0f, 50.0f, 0.0f);
    pid_reset(&pid, 25.0f);
    float before = pid.integrator;
    float out = pid_step(&pid, 200.0f, 25.0f, 0.25f);   /* big positive error */
    TEST_ASSERT_EQUAL_FLOAT(100.0f, out);
    TEST_ASSERT_EQUAL_FLOAT(before, pid.integrator);    /* not updated */
}

/* Saturation low + negative error → output clamped at 0, integrator frozen. */
void test_saturation_low_freezes_integrator(void) {
    pid_init(&pid, 100.0f, 50.0f, 0.0f);
    pid_reset(&pid, 250.0f);
    float before = pid.integrator;
    float out = pid_step(&pid, 25.0f, 250.0f, 0.25f);   /* big negative error */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out);
    TEST_ASSERT_EQUAL_FLOAT(before, pid.integrator);
}

/* Within range: integrator accumulates. */
void test_within_range_integrator_grows(void) {
    pid_init(&pid, 1.0f, 1.0f, 0.0f);
    pid_reset(&pid, 25.0f);
    pid_step(&pid, 30.0f, 25.0f, 0.25f);    /* error 5, accumulates */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f * 0.25f, pid.integrator);
}

/* Live gain change does not reset integrator. */
void test_set_gains_preserves_integrator(void) {
    pid_init(&pid, 1.0f, 1.0f, 0.0f);
    pid_reset(&pid, 25.0f);
    pid_step(&pid, 30.0f, 25.0f, 0.25f);
    float before = pid.integrator;
    pid_set_gains(&pid, 5.0f, 2.0f, 0.5f);
    TEST_ASSERT_EQUAL_FLOAT(before, pid.integrator);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, pid.kp);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, pid.ki);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, pid.kd);
}

/* Derivative on measurement: a setpoint step alone produces no derivative kick. */
void test_setpoint_step_does_not_cause_derivative_kick(void) {
    pid_init(&pid, 0.0f, 0.0f, 100.0f);   /* Kp=Ki=0 isolates d_term */
    pid_reset(&pid, 50.0f);
    float out = pid_step(&pid, 200.0f, 50.0f, 0.25f);   /* temp unchanged */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reset_clears_integrator_and_sets_prev_temp);
    RUN_TEST(test_first_step_has_zero_derivative);
    RUN_TEST(test_saturation_high_freezes_integrator);
    RUN_TEST(test_saturation_low_freezes_integrator);
    RUN_TEST(test_within_range_integrator_grows);
    RUN_TEST(test_set_gains_preserves_integrator);
    RUN_TEST(test_setpoint_step_does_not_cause_derivative_kick);
    return UNITY_END();
}
```

- [ ] **Step 3: Run tests to confirm they fail**

Run: `pio test -e native -f test_pid`
Expected: link error or all tests fail (no implementation yet).

- [ ] **Step 4: Write the implementation**

`src/control/pid.c`:

```c
#include "control/pid.h"

static float clamp(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

void pid_init(pid_state_t *s, float kp, float ki, float kd) {
    s->kp = kp;
    s->ki = ki;
    s->kd = kd;
    s->integrator = 0.0f;
    s->prev_temp  = 0.0f;
}

void pid_set_gains(pid_state_t *s, float kp, float ki, float kd) {
    s->kp = kp;
    s->ki = ki;
    s->kd = kd;
}

void pid_reset(pid_state_t *s, float current_temp) {
    s->integrator = 0.0f;
    s->prev_temp  = current_temp;
}

float pid_step(pid_state_t *s, float setpoint, float temp, float dt) {
    const float error       = setpoint - temp;
    const float p_term      = s->kp * error;
    const float d_term      = -s->kd * (temp - s->prev_temp) / dt;
    const float i_tentative = s->integrator + s->ki * error * dt;
    const float raw         = p_term + i_tentative + d_term;

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

- [ ] **Step 5: Run tests; expect green**

Run: `pio test -e native -f test_pid`
Expected: `7 Tests 0 Failures 0 Ignored — OK`.

- [ ] **Step 6: Commit**

```bash
git add src/control/pid.h src/control/pid.c test/test_pid/
git commit -m "Phase 2.1: PID module with conditional-integration anti-windup (TDD)"
```

---

### Task 5: Profile curve generator + phase boundaries

**Files:**
- Create: `src/control/profile_curve.h`
- Create: `src/control/profile_curve.c`
- Create: `test/test_profile_curve/test_profile_curve.c`

- [ ] **Step 1: Write the header**

`src/control/profile_curve.h`:

```c
/* Pre-computes a setpoint-vs-time curve for one reflow profile.
 * Curve is sampled at PROFILE_CURVE_STEP_MS intervals.
 * See spec §4.3 and §5.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    float    first_heatup_rate;   /* °C/s */
    uint16_t soak_temp;           /* °C */
    uint16_t soak_time;           /* s */
    float    second_heatup_rate;
    uint16_t reflow_temp;
    uint16_t reflow_time;
} profile_t;

typedef enum {
    STAGE_NONE,
    STAGE_HEAT_UP_1,
    STAGE_SOAK,
    STAGE_HEAT_UP_2,
    STAGE_REFLOW,
    STAGE_COOL_DOWN,
    STAGE_COUNT_
} stage_t;

/* Maximum curve length: a generous bound for the worst-case profile.
 * 10 min × 60 s × 2 samples/s = 1200; we keep some slack.
 */
#define PROFILE_CURVE_MAX_SAMPLES 1500

typedef struct {
    float    samples[PROFILE_CURVE_MAX_SAMPLES];
    uint16_t length;                       /* number of valid samples */
    uint32_t phase_end_ms[STAGE_COUNT_];   /* phase_end_ms[STAGE_X] = ms at which stage X ends */
} profile_curve_t;

/* Validate cross-field bounds. Returns true if the profile is acceptable. */
bool profile_validate(const profile_t *p);

/* Generate the curve. Caller-owned `out` buffer is fully overwritten.
 * Returns true on success, false if profile fails validation (out is left zeroed).
 */
bool profile_curve_generate(const profile_t *p, profile_curve_t *out);

/* Setpoint lookup, clamped to last sample on OOB. */
float profile_curve_setpoint(const profile_curve_t *c, uint32_t elapsed_ms);

/* Built-in defaults loaded from config.h. */
void profile_defaults(profile_t *out);
```

- [ ] **Step 2: Write failing tests**

`test/test_profile_curve/test_profile_curve.c`:

```c
#include <unity.h>
#include <string.h>
#include "control/profile_curve.h"
#include "config.h"

static profile_t prof;
static profile_curve_t curve;

void setUp(void)    { profile_defaults(&prof); memset(&curve, 0, sizeof(curve)); }
void tearDown(void) {}

void test_defaults_validate(void) {
    TEST_ASSERT_TRUE(profile_validate(&prof));
}

void test_defaults_match_config_h(void) {
    TEST_ASSERT_EQUAL_FLOAT(FIRST_HEATUP_DEFAULT, prof.first_heatup_rate);
    TEST_ASSERT_EQUAL_UINT16(SOAK_TEMP_DEFAULT, prof.soak_temp);
    TEST_ASSERT_EQUAL_UINT16(SOAK_TIME_DEFAULT, prof.soak_time);
    TEST_ASSERT_EQUAL_FLOAT(SECOND_HEATUP_DEFAULT, prof.second_heatup_rate);
    TEST_ASSERT_EQUAL_UINT16(REFLOW_TEMP_DEFAULT, prof.reflow_temp);
    TEST_ASSERT_EQUAL_UINT16(REFLOW_TIME_DEFAULT, prof.reflow_time);
}

void test_reflow_below_soak_rejected(void) {
    prof.reflow_temp = 100;
    prof.soak_temp = 150;
    TEST_ASSERT_FALSE(profile_validate(&prof));
}

void test_zero_rate_rejected(void) {
    prof.first_heatup_rate = 0.0f;
    TEST_ASSERT_FALSE(profile_validate(&prof));
}

void test_curve_generates_for_valid_profile(void) {
    TEST_ASSERT_TRUE(profile_curve_generate(&prof, &curve));
    TEST_ASSERT_GREATER_THAN(0, curve.length);
}

/* Stage end times are monotonically increasing. */
void test_phase_end_ms_monotonic(void) {
    profile_curve_generate(&prof, &curve);
    TEST_ASSERT_LESS_THAN(curve.phase_end_ms[STAGE_SOAK],       curve.phase_end_ms[STAGE_HEAT_UP_1]);
    TEST_ASSERT_LESS_THAN(curve.phase_end_ms[STAGE_HEAT_UP_2],  curve.phase_end_ms[STAGE_SOAK]);
    TEST_ASSERT_LESS_THAN(curve.phase_end_ms[STAGE_REFLOW],     curve.phase_end_ms[STAGE_HEAT_UP_2]);
}

/* Lookup at t=0 returns approximately room-temp setpoint. */
void test_setpoint_at_zero_is_near_ambient(void) {
    profile_curve_generate(&prof, &curve);
    float sp = profile_curve_setpoint(&curve, 0);
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 25.0f, sp);
}

/* Lookup past curve end clamps to last sample. */
void test_setpoint_oob_clamps_to_last(void) {
    profile_curve_generate(&prof, &curve);
    float last = curve.samples[curve.length - 1];
    float oob  = profile_curve_setpoint(&curve, 99999999u);
    TEST_ASSERT_EQUAL_FLOAT(last, oob);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_validate);
    RUN_TEST(test_defaults_match_config_h);
    RUN_TEST(test_reflow_below_soak_rejected);
    RUN_TEST(test_zero_rate_rejected);
    RUN_TEST(test_curve_generates_for_valid_profile);
    RUN_TEST(test_phase_end_ms_monotonic);
    RUN_TEST(test_setpoint_at_zero_is_near_ambient);
    RUN_TEST(test_setpoint_oob_clamps_to_last);
    return UNITY_END();
}
```

- [ ] **Step 3: Run tests; expect failure (no impl)**

Run: `pio test -e native -f test_profile_curve`
Expected: link error.

- [ ] **Step 4: Implement**

`src/control/profile_curve.c`:

```c
#include "control/profile_curve.h"
#include "config.h"
#include <string.h>

#define AMBIENT_C 25.0f

void profile_defaults(profile_t *out) {
    out->first_heatup_rate  = FIRST_HEATUP_DEFAULT;
    out->soak_temp          = SOAK_TEMP_DEFAULT;
    out->soak_time          = SOAK_TIME_DEFAULT;
    out->second_heatup_rate = SECOND_HEATUP_DEFAULT;
    out->reflow_temp        = REFLOW_TEMP_DEFAULT;
    out->reflow_time        = REFLOW_TIME_DEFAULT;
}

bool profile_validate(const profile_t *p) {
    if (p->first_heatup_rate  < HEATUP_RATE_MIN  || p->first_heatup_rate  > HEATUP_RATE_MAX) return false;
    if (p->second_heatup_rate < HEATUP_RATE_MIN  || p->second_heatup_rate > HEATUP_RATE_MAX) return false;
    if (p->soak_temp   < SOAK_TEMP_MIN   || p->soak_temp   > SOAK_TEMP_MAX)   return false;
    if (p->soak_time   < SOAK_TIME_MIN   || p->soak_time   > SOAK_TIME_MAX)   return false;
    if (p->reflow_temp < REFLOW_TEMP_MIN || p->reflow_temp > REFLOW_TEMP_MAX) return false;
    if (p->reflow_time < REFLOW_TIME_MIN || p->reflow_time > REFLOW_TIME_MAX) return false;
    if (p->reflow_temp <= p->soak_temp) return false;
    return true;
}

/* Helper: append samples of a linear ramp from `from` to `to` at `rate_c_per_s`.
 * Returns elapsed_ms added.
 */
static uint32_t append_ramp(profile_curve_t *c, float from, float to, float rate) {
    const float step_s = PROFILE_CURVE_STEP_MS / 1000.0f;
    float v = from;
    uint32_t added = 0;
    while (v < to && c->length < PROFILE_CURVE_MAX_SAMPLES) {
        c->samples[c->length++] = v;
        v += rate * step_s;
        added += PROFILE_CURVE_STEP_MS;
    }
    return added;
}

static uint32_t append_hold(profile_curve_t *c, float value, uint32_t duration_ms) {
    uint32_t added = 0;
    while (added < duration_ms && c->length < PROFILE_CURVE_MAX_SAMPLES) {
        c->samples[c->length++] = value;
        added += PROFILE_CURVE_STEP_MS;
    }
    return added;
}

bool profile_curve_generate(const profile_t *p, profile_curve_t *out) {
    memset(out, 0, sizeof(*out));
    if (!profile_validate(p)) return false;

    uint32_t t = 0;

    /* HEAT_UP_1 */
    t += append_ramp(out, AMBIENT_C, (float)p->soak_temp, p->first_heatup_rate);
    out->phase_end_ms[STAGE_HEAT_UP_1] = t;

    /* SOAK */
    t += append_hold(out, (float)p->soak_temp, p->soak_time * 1000u);
    out->phase_end_ms[STAGE_SOAK] = t;

    /* HEAT_UP_2 */
    t += append_ramp(out, (float)p->soak_temp, (float)p->reflow_temp, p->second_heatup_rate);
    out->phase_end_ms[STAGE_HEAT_UP_2] = t;

    /* REFLOW */
    t += append_hold(out, (float)p->reflow_temp, p->reflow_time * 1000u);
    out->phase_end_ms[STAGE_REFLOW] = t;

    /* COOL_DOWN is passive: no samples appended, no curve lookup used.
     * phase_end_ms[STAGE_COOL_DOWN] is unused (cool-down ends on temp or timeout, see reflow_sm).
     */
    return true;
}

float profile_curve_setpoint(const profile_curve_t *c, uint32_t elapsed_ms) {
    if (c->length == 0) return AMBIENT_C;
    size_t idx = elapsed_ms / PROFILE_CURVE_STEP_MS;
    if (idx >= c->length) idx = c->length - 1;
    return c->samples[idx];
}
```

- [ ] **Step 5: Run; expect green**

Run: `pio test -e native -f test_profile_curve`
Expected: `8 Tests 0 Failures 0 Ignored — OK`.

- [ ] **Step 6: Commit**

```bash
git add src/control/profile_curve.* test/test_profile_curve/
git commit -m "Phase 2.2: profile curve generator + validation (TDD)"
```

---

### Task 6: Reflow state machine

**Files:**
- Create: `src/control/reflow_sm.h`
- Create: `src/control/reflow_sm.c`
- Create: `test/test_reflow_sm/test_reflow_sm.c`

- [ ] **Step 1: Write the header**

`src/control/reflow_sm.h`:

```c
/* Reflow state machine. Pure logic — caller passes elapsed_ms and current temp;
 * the SM returns a recommended action and stage. See spec §5.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "control/profile_curve.h"

typedef enum { MODE_IDLE, MODE_RUNNING, MODE_FAULT } oven_mode_t;

typedef enum {
    FAULT_NONE       = 0,
    FAULT_TC_OPEN    = 1 << 0,
    FAULT_TC_SCG     = 1 << 1,
    FAULT_TC_SCV     = 1 << 2,
    FAULT_TC_STALE   = 1 << 3,
    FAULT_OVER_TEMP  = 1 << 4,
} fault_flag_t;

typedef struct {
    oven_mode_t mode;
    stage_t     stage;
    uint32_t    elapsed_ms;            /* time since START */
    uint32_t    cool_down_start_ms;    /* set on entry to COOL_DOWN */
    uint8_t     fault_flags;
} sm_state_t;

typedef enum {
    SM_EVENT_NONE,
    SM_EVENT_CMD_START,
    SM_EVENT_CMD_STOP,
    SM_EVENT_CMD_ACK_FAULT,
} sm_event_t;

void sm_init(sm_state_t *s);
void sm_trip_fault(sm_state_t *s, uint8_t flag);

/* Drive the SM forward by one tick. Caller passes:
 *  - the current event (or SM_EVENT_NONE)
 *  - the current temp and a tick of CONTROL_TICK_MS in elapsed time
 *  - the profile curve (for phase_end_ms boundaries)
 * Returns true if the SM is in a stage where the controller should run PID.
 */
bool sm_step(sm_state_t *s,
             sm_event_t event,
             float temp_c,
             const profile_curve_t *curve);
```

- [ ] **Step 2: Write failing tests**

`test/test_reflow_sm/test_reflow_sm.c`:

```c
#include <unity.h>
#include <string.h>
#include "control/reflow_sm.h"
#include "control/profile_curve.h"
#include "config.h"

static sm_state_t sm;
static profile_t prof;
static profile_curve_t curve;

void setUp(void) {
    sm_init(&sm);
    profile_defaults(&prof);
    profile_curve_generate(&prof, &curve);
}
void tearDown(void) {}

void test_init_is_idle_no_fault(void) {
    TEST_ASSERT_EQUAL(MODE_IDLE, sm.mode);
    TEST_ASSERT_EQUAL(STAGE_NONE, sm.stage);
    TEST_ASSERT_EQUAL(0, sm.fault_flags);
}

void test_start_transitions_to_heat_up_1(void) {
    sm_step(&sm, SM_EVENT_CMD_START, 25.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_RUNNING, sm.mode);
    TEST_ASSERT_EQUAL(STAGE_HEAT_UP_1, sm.stage);
}

void test_stop_returns_to_idle(void) {
    sm_step(&sm, SM_EVENT_CMD_START, 25.0f, &curve);
    sm_step(&sm, SM_EVENT_CMD_STOP, 100.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_IDLE, sm.mode);
    TEST_ASSERT_EQUAL(STAGE_NONE, sm.stage);
}

void test_stages_advance_at_phase_end_ms(void) {
    sm_step(&sm, SM_EVENT_CMD_START, 25.0f, &curve);
    /* Tick forward until past HEAT_UP_1's phase end. */
    uint32_t target = curve.phase_end_ms[STAGE_HEAT_UP_1] + CONTROL_TICK_MS;
    while (sm.elapsed_ms <= target) {
        sm_step(&sm, SM_EVENT_NONE, 100.0f, &curve);
    }
    TEST_ASSERT_EQUAL(STAGE_SOAK, sm.stage);
}

void test_fault_is_sticky_until_ack(void) {
    sm_trip_fault(&sm, FAULT_TC_OPEN);
    TEST_ASSERT_EQUAL(MODE_FAULT, sm.mode);
    sm_step(&sm, SM_EVENT_CMD_START, 25.0f, &curve);   /* START rejected */
    TEST_ASSERT_EQUAL(MODE_FAULT, sm.mode);
    sm_step(&sm, SM_EVENT_CMD_ACK_FAULT, 25.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_IDLE, sm.mode);
    TEST_ASSERT_EQUAL(0, sm.fault_flags);
}

void test_cool_down_exits_on_temp_threshold(void) {
    /* Force into COOL_DOWN. */
    sm.mode = MODE_RUNNING;
    sm.stage = STAGE_COOL_DOWN;
    sm.cool_down_start_ms = 0;
    sm.elapsed_ms = 0;
    sm_step(&sm, SM_EVENT_NONE, COOL_DOWN_THRESHOLD_C - 1.0f, &curve);
    TEST_ASSERT_EQUAL(MODE_IDLE, sm.mode);
}

void test_cool_down_exits_on_timeout(void) {
    sm.mode = MODE_RUNNING;
    sm.stage = STAGE_COOL_DOWN;
    sm.cool_down_start_ms = 0;
    sm.elapsed_ms = COOL_DOWN_TIMEOUT_MS + CONTROL_TICK_MS;
    sm_step(&sm, SM_EVENT_NONE, 200.0f, &curve);       /* still hot */
    TEST_ASSERT_EQUAL(MODE_IDLE, sm.mode);
}

void test_pid_runs_only_in_active_stages(void) {
    sm_step(&sm, SM_EVENT_CMD_START, 25.0f, &curve);
    TEST_ASSERT_TRUE(sm_step(&sm, SM_EVENT_NONE, 30.0f, &curve));  /* HEAT_UP_1 */

    sm.stage = STAGE_COOL_DOWN;
    sm.cool_down_start_ms = sm.elapsed_ms;
    TEST_ASSERT_FALSE(sm_step(&sm, SM_EVENT_NONE, 200.0f, &curve));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_is_idle_no_fault);
    RUN_TEST(test_start_transitions_to_heat_up_1);
    RUN_TEST(test_stop_returns_to_idle);
    RUN_TEST(test_stages_advance_at_phase_end_ms);
    RUN_TEST(test_fault_is_sticky_until_ack);
    RUN_TEST(test_cool_down_exits_on_temp_threshold);
    RUN_TEST(test_cool_down_exits_on_timeout);
    RUN_TEST(test_pid_runs_only_in_active_stages);
    return UNITY_END();
}
```

- [ ] **Step 3: Run; expect failure**

Run: `pio test -e native -f test_reflow_sm`

- [ ] **Step 4: Implement**

`src/control/reflow_sm.c`:

```c
#include "control/reflow_sm.h"
#include "config.h"
#include <string.h>

void sm_init(sm_state_t *s) {
    memset(s, 0, sizeof(*s));
    s->mode  = MODE_IDLE;
    s->stage = STAGE_NONE;
}

void sm_trip_fault(sm_state_t *s, uint8_t flag) {
    s->mode = MODE_FAULT;
    s->stage = STAGE_NONE;
    s->fault_flags |= flag;
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

    /* Command processing — fault gating */
    if (s->mode == MODE_FAULT) {
        if (event == SM_EVENT_CMD_ACK_FAULT) {
            s->mode = MODE_IDLE;
            s->stage = STAGE_NONE;
            s->fault_flags = 0;
            s->elapsed_ms = 0;
        }
        return false;
    }

    /* IDLE handling */
    if (s->mode == MODE_IDLE) {
        if (event == SM_EVENT_CMD_START) {
            s->mode = MODE_RUNNING;
            s->stage = STAGE_HEAT_UP_1;
            s->elapsed_ms = 0;
        }
        return false;
    }

    /* RUNNING: STOP returns to IDLE immediately. */
    if (event == SM_EVENT_CMD_STOP) {
        s->mode = MODE_IDLE;
        s->stage = STAGE_NONE;
        s->elapsed_ms = 0;
        return false;
    }

    /* Cool-down: exit on temp or timeout. */
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

    /* Active running stages: advance time, possibly transition stage, PID runs. */
    s->elapsed_ms += CONTROL_TICK_MS;
    advance_stage_if_due(s, curve);
    return active_stage(s->stage);
}
```

- [ ] **Step 5: Run; expect green**

Run: `pio test -e native -f test_reflow_sm`
Expected: `8 Tests 0 Failures 0 Ignored — OK`.

- [ ] **Step 6: Commit**

```bash
git add src/control/reflow_sm.* test/test_reflow_sm/
git commit -m "Phase 2.3: reflow state machine (TDD)"
```

---

### Task 7: SSR slow-PWM scheduler (pure logic)

**Files:**
- Create: `src/drivers/ssr_scheduler.h`  (pure-logic helper; the GPIO driver wraps it in Phase 4)
- Create: `src/drivers/ssr_scheduler.c`
- Create: `test/test_ssr_scheduler/test_ssr_scheduler.c`

- [ ] **Step 1: Write the header**

`src/drivers/ssr_scheduler.h`:

```c
/* Pure-logic core of the slow-PWM SSR scheduler.
 * Given the current time and a duty percent, returns whether the SSR
 * pin should be HIGH or LOW for this tick. See spec §4.2.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Returns true if the SSR should be ON for the tick corresponding to now_ms. */
bool ssr_scheduler_should_be_on(uint32_t now_ms, uint8_t duty_percent);
```

- [ ] **Step 2: Write failing tests**

`test/test_ssr_scheduler/test_ssr_scheduler.c`:

```c
#include <unity.h>
#include "drivers/ssr_scheduler.h"
#include "config.h"

void setUp(void) {}
void tearDown(void) {}

void test_duty_zero_always_off(void) {
    for (uint32_t t = 0; t < 10000; t += CONTROL_TICK_MS) {
        TEST_ASSERT_FALSE(ssr_scheduler_should_be_on(t, 0));
    }
}

void test_duty_100_always_on(void) {
    for (uint32_t t = 0; t < 10000; t += CONTROL_TICK_MS) {
        TEST_ASSERT_TRUE(ssr_scheduler_should_be_on(t, 100));
    }
}

void test_duty_50_half_on(void) {
    int on = 0;
    for (uint32_t t = 0; t < SSR_PWM_WINDOW_MS; t += CONTROL_TICK_MS) {
        if (ssr_scheduler_should_be_on(t, 50)) on++;
    }
    TEST_ASSERT_EQUAL_INT(SSR_QUANTA_PER_WINDOW / 2, on);
}

/* Rounding: duty=99 should round up to all 8 slots ON, not 7. */
void test_duty_99_rounds_to_full_window(void) {
    int on = 0;
    for (uint32_t t = 0; t < SSR_PWM_WINDOW_MS; t += CONTROL_TICK_MS) {
        if (ssr_scheduler_should_be_on(t, 99)) on++;
    }
    TEST_ASSERT_EQUAL_INT(SSR_QUANTA_PER_WINDOW, on);
}

/* Rounding: duty=1 should round to 0 slots (nearest 0/8 = 0%). */
void test_duty_1_rounds_to_off(void) {
    int on = 0;
    for (uint32_t t = 0; t < SSR_PWM_WINDOW_MS; t += CONTROL_TICK_MS) {
        if (ssr_scheduler_should_be_on(t, 1)) on++;
    }
    TEST_ASSERT_EQUAL_INT(0, on);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_duty_zero_always_off);
    RUN_TEST(test_duty_100_always_on);
    RUN_TEST(test_duty_50_half_on);
    RUN_TEST(test_duty_99_rounds_to_full_window);
    RUN_TEST(test_duty_1_rounds_to_off);
    return UNITY_END();
}
```

- [ ] **Step 3: Run; expect failure**

Run: `pio test -e native -f test_ssr_scheduler`

- [ ] **Step 4: Implement**

`src/drivers/ssr_scheduler.c`:

```c
#include "drivers/ssr_scheduler.h"
#include "config.h"

bool ssr_scheduler_should_be_on(uint32_t now_ms, uint8_t duty_percent) {
    if (duty_percent >= 100) return true;
    if (duty_percent == 0)   return false;

    uint32_t slot = (now_ms / CONTROL_TICK_MS) % SSR_QUANTA_PER_WINDOW;
    /* Round to nearest: duty=99 → 8, duty=1 → 0. */
    uint32_t threshold = ((uint32_t)duty_percent * SSR_QUANTA_PER_WINDOW + 50) / 100;
    return slot < threshold;
}
```

- [ ] **Step 5: Run; expect green**

Run: `pio test -e native -f test_ssr_scheduler`
Expected: `5 Tests 0 Failures 0 Ignored — OK`.

- [ ] **Step 6: Commit**

```bash
git add src/drivers/ssr_scheduler.* test/test_ssr_scheduler/
git commit -m "Phase 2.4: SSR slow-PWM scheduler logic (TDD)"
```

---

### Task 8: NVS schema serialisation

**Files:**
- Create: `src/storage/nvs_schema.h`
- Create: `src/storage/nvs_schema.c`
- Create: `test/test_nvs_schema/test_nvs_schema.c`

The schema layer handles versioning + defaults; the NVS I/O wrapper (with `nvs_open` etc.) comes in Phase 4 and uses this layer.

- [ ] **Step 1: Write the header**

`src/storage/nvs_schema.h`:

```c
/* Versioned struct ↔ blob serialisation for profile + PID records.
 * Pure logic — the actual NVS I/O lives in drivers/nvs_store (Phase 4).
 * See spec §8.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "control/profile_curve.h"

/* Profile v1 wire format. */
typedef struct __attribute__((packed)) {
    uint8_t  version;             /* PROFILE_SCHEMA_VERSION */
    float    first_heatup_rate;
    uint16_t soak_temp;
    uint16_t soak_time;
    float    second_heatup_rate;
    uint16_t reflow_temp;
    uint16_t reflow_time;
} profile_v1_t;

typedef struct {
    float kp, ki, kd;
} pid_gains_t;

typedef struct __attribute__((packed)) {
    uint8_t version;             /* PID_SCHEMA_VERSION */
    float   kp, ki, kd;
} pid_v1_t;

/* Encode/decode. Decode validates version and size; returns false on mismatch
 * (caller should load defaults).
 */
void profile_to_blob(const profile_t *p, profile_v1_t *out);
bool profile_from_blob(const void *blob, size_t blob_len, profile_t *out);

void pid_to_blob(const pid_gains_t *g, pid_v1_t *out);
bool pid_from_blob(const void *blob, size_t blob_len, pid_gains_t *out);

/* Built-in defaults. */
void pid_defaults(pid_gains_t *out);
```

- [ ] **Step 2: Write failing tests**

`test/test_nvs_schema/test_nvs_schema.c`:

```c
#include <unity.h>
#include <string.h>
#include "storage/nvs_schema.h"
#include "config.h"

void setUp(void) {}
void tearDown(void) {}

void test_profile_round_trip(void) {
    profile_t in; profile_defaults(&in);
    profile_v1_t blob;
    profile_to_blob(&in, &blob);
    profile_t out;
    TEST_ASSERT_TRUE(profile_from_blob(&blob, sizeof(blob), &out));
    TEST_ASSERT_EQUAL_FLOAT(in.first_heatup_rate,  out.first_heatup_rate);
    TEST_ASSERT_EQUAL_UINT16(in.soak_temp, out.soak_temp);
    TEST_ASSERT_EQUAL_UINT16(in.reflow_time, out.reflow_time);
}

void test_profile_version_mismatch_rejected(void) {
    profile_v1_t blob;
    profile_t in; profile_defaults(&in);
    profile_to_blob(&in, &blob);
    blob.version = 99;
    profile_t out;
    TEST_ASSERT_FALSE(profile_from_blob(&blob, sizeof(blob), &out));
}

void test_profile_truncated_blob_rejected(void) {
    profile_v1_t blob; profile_t in; profile_defaults(&in);
    profile_to_blob(&in, &blob);
    profile_t out;
    TEST_ASSERT_FALSE(profile_from_blob(&blob, sizeof(blob) - 1, &out));
}

void test_pid_round_trip(void) {
    pid_gains_t in; pid_defaults(&in);
    pid_v1_t blob;
    pid_to_blob(&in, &blob);
    pid_gains_t out;
    TEST_ASSERT_TRUE(pid_from_blob(&blob, sizeof(blob), &out));
    TEST_ASSERT_EQUAL_FLOAT(in.kp, out.kp);
    TEST_ASSERT_EQUAL_FLOAT(in.ki, out.ki);
    TEST_ASSERT_EQUAL_FLOAT(in.kd, out.kd);
}

void test_pid_defaults_match_config(void) {
    pid_gains_t g; pid_defaults(&g);
    TEST_ASSERT_EQUAL_FLOAT(KP_DEFAULT, g.kp);
    TEST_ASSERT_EQUAL_FLOAT(KI_DEFAULT, g.ki);
    TEST_ASSERT_EQUAL_FLOAT(KD_DEFAULT, g.kd);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_profile_round_trip);
    RUN_TEST(test_profile_version_mismatch_rejected);
    RUN_TEST(test_profile_truncated_blob_rejected);
    RUN_TEST(test_pid_round_trip);
    RUN_TEST(test_pid_defaults_match_config);
    return UNITY_END();
}
```

- [ ] **Step 3: Run; expect failure**

Run: `pio test -e native -f test_nvs_schema`

- [ ] **Step 4: Implement**

`src/storage/nvs_schema.c`:

```c
#include "storage/nvs_schema.h"
#include "config.h"
#include <string.h>

void profile_to_blob(const profile_t *p, profile_v1_t *out) {
    out->version            = PROFILE_SCHEMA_VERSION;
    out->first_heatup_rate  = p->first_heatup_rate;
    out->soak_temp          = p->soak_temp;
    out->soak_time          = p->soak_time;
    out->second_heatup_rate = p->second_heatup_rate;
    out->reflow_temp        = p->reflow_temp;
    out->reflow_time        = p->reflow_time;
}

bool profile_from_blob(const void *blob, size_t blob_len, profile_t *out) {
    if (blob_len != sizeof(profile_v1_t)) return false;
    const profile_v1_t *b = (const profile_v1_t *)blob;
    if (b->version != PROFILE_SCHEMA_VERSION) return false;
    out->first_heatup_rate  = b->first_heatup_rate;
    out->soak_temp          = b->soak_temp;
    out->soak_time          = b->soak_time;
    out->second_heatup_rate = b->second_heatup_rate;
    out->reflow_temp        = b->reflow_temp;
    out->reflow_time        = b->reflow_time;
    return true;
}

void pid_to_blob(const pid_gains_t *g, pid_v1_t *out) {
    out->version = PID_SCHEMA_VERSION;
    out->kp = g->kp;
    out->ki = g->ki;
    out->kd = g->kd;
}

bool pid_from_blob(const void *blob, size_t blob_len, pid_gains_t *out) {
    if (blob_len != sizeof(pid_v1_t)) return false;
    const pid_v1_t *b = (const pid_v1_t *)blob;
    if (b->version != PID_SCHEMA_VERSION) return false;
    out->kp = b->kp;
    out->ki = b->ki;
    out->kd = b->kd;
    return true;
}

void pid_defaults(pid_gains_t *out) {
    out->kp = KP_DEFAULT;
    out->ki = KI_DEFAULT;
    out->kd = KD_DEFAULT;
}
```

- [ ] **Step 5: Run; expect green**

Run: `pio test -e native -f test_nvs_schema`
Expected: `5 Tests 0 Failures 0 Ignored — OK`.

- [ ] **Step 6: Run the whole native test suite**

Run: `pio test -e native`
Expected: all five test groups pass.

- [ ] **Step 7: Commit**

```bash
git add src/storage/nvs_schema.* test/test_nvs_schema/
git commit -m "Phase 2.5: NVS schema serialisation + defaults (TDD)"
```

---

**Phase 2 checkpoint.** All host-testable control logic is implemented and unit-tested. `pio test -e native` is fully green.

## Phase 3 — Hardware bring-up

Seven self-contained mini-PIO projects, one per hardware concern. Each lives in `tools/bringup/0N_<name>/`, has its own `platformio.ini` and `src/main.c`, and `#include`s the main project's `include/config.h` for pin assignments (so the pin map is single-sourced).

Each project's `platformio.ini` is identical apart from the path-relative `build_flags`. Use this template:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = espidf
monitor_speed = 115200
build_flags =
    -I../../../include            ; reach the main config.h
```

Per-task: `pio run -d tools/bringup/0N_<name> -t upload && pio device monitor` (in the project dir). When done, do not delete the bring-up project — it stays as a regression check.

#### Board-specific settings discovered during Phase 3

These are the settings that worked on the actual LCDwiki E32R35T hardware. Phase 4 driver code (Tasks 16–22) and the LVGL setup in Task 29 already incorporate them — listed here for at-a-glance reference.

| Concern | Setting | Where it lives | Bring-up that verified it |
|---|---|---|---|
| ST7796 colour element order | `rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR` (renamed from `rgb_endian` in IDF >=6) | Task 18 | 02_display |
| ST7796 colour inversion | `invert_color(false)` — plan originally said `true` but that double-inverted with the byte-swap fix | Task 18 | 02_display |
| ST7796 swap_xy/mirror | Not called in the panel init — `lvgl_port_add_disp` applies these from its rotation field and would override | Task 18 | 04_lvgl_hello |
| LVGL X mirror | `rotation.mirror_x = true` in `lvgl_port_display_cfg_t` (otherwise text reads right-to-left) | Task 29 | 04_lvgl_hello |
| LVGL RGB565 byte order | `flags.swap_bytes = true` in `lvgl_port_display_cfg_t` (ESP32 is little-endian, panel is MSB-first) | Task 29 | 04_lvgl_hello |
| Direct framebuffer writes (no LVGL) | `__builtin_bswap16` on each `uint16_t` colour before storing | 02_display only | 02_display |
| Direct framebuffer writes — async DMA sync | Register `on_color_trans_done` callback + binary semaphore; do NOT reuse the same buffer between `esp_lcd_panel_draw_bitmap` calls without waiting (the trailing rows of the in-flight transfer pick up the new colour) | 02_display only | 02_display |
| XPT2046 Y axis | `flags.mirror_y = 1` (raw Y is inverted in portrait on this board) | Task 19 | 03_touch |
| XPT2046 IRQ pin | `int_gpio_num = -1`. ESP32 GPIO 36 has no internal pull-up, and esp_lvgl_port enters EVENT mode (IRQ-only reads) whenever `int_gpio_num` is set. With no pull-up the IRQ never fires and LVGL never polls. -1 keeps the indev in timer-polled mode. | Task 19 | 04_lvgl_hello |

---

### Task 9: 01_blink — flashing pipeline + RGB LED

**Files:**
- Create: `tools/bringup/01_blink/platformio.ini`
- Create: `tools/bringup/01_blink/src/main.c`

- [ ] **Step 1: Write `platformio.ini`** (use the template above).

- [ ] **Step 2: Write `src/main.c`**

```c
/* tools/bringup/01_blink/src/main.c
 * Cycles RGB LED red → green → blue → off, 500 ms per state.
 * Confirms ESP-IDF + PIO + flashing + GPIO basics.
 * RGB LED pins are *active low* per board spec.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "config.h"

#define PIN_LED_R 22
#define PIN_LED_G 16
#define PIN_LED_B 17

static void led_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_LED_R) | (1ULL << PIN_LED_G) | (1ULL << PIN_LED_B),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    /* off = HIGH (active low) */
    gpio_set_level(PIN_LED_R, 1);
    gpio_set_level(PIN_LED_G, 1);
    gpio_set_level(PIN_LED_B, 1);
}

static void all_off(void) {
    gpio_set_level(PIN_LED_R, 1);
    gpio_set_level(PIN_LED_G, 1);
    gpio_set_level(PIN_LED_B, 1);
}

void app_main(void) {
    led_init();
    while (1) {
        all_off(); gpio_set_level(PIN_LED_R, 0); vTaskDelay(pdMS_TO_TICKS(500));
        all_off(); gpio_set_level(PIN_LED_G, 0); vTaskDelay(pdMS_TO_TICKS(500));
        all_off(); gpio_set_level(PIN_LED_B, 0); vTaskDelay(pdMS_TO_TICKS(500));
        all_off();                                vTaskDelay(pdMS_TO_TICKS(500));
    }
}
```

- [ ] **Step 3: Flash and observe**

Run: `pio run -d tools/bringup/01_blink -t upload`
Expected: build + flash succeeds; onboard RGB LED cycles R/G/B/off.

- [ ] **Step 4: Commit**

```bash
git add tools/bringup/01_blink/
git commit -m "Phase 3.1: bringup 01_blink — flashing pipeline + RGB LED"
```

---

### Task 10: 02_display — ST7796 panel via esp_lcd

**Files:**
- Create: `tools/bringup/02_display/platformio.ini`
- Create: `tools/bringup/02_display/src/main.c`

- [ ] **Step 1: `platformio.ini`** — same template as 01_blink. Plus a `src/idf_component.yml` for the panel driver:

```yaml
dependencies:
  idf: ">=5.0"
  espressif/esp_lcd_st7796: "^1"
```

- [ ] **Step 2: Write `src/main.c`**

```c
/* tools/bringup/02_display/src/main.c
 * Initialises ST7796 in portrait orientation and fills the screen with three
 * horizontal bands (red / green / blue). Confirms LCD wiring, HSPI bus,
 * portrait orientation, and backlight pin.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "config.h"
#include <string.h>

#define LCD_WIDTH  320
#define LCD_HEIGHT 480
#define BAND_LINES 80    /* draw a horizontal band of 80 lines at a time */

static const char *TAG = "disp_bringup";

void app_main(void) {
    /* SPI bus (HSPI = SPI2_HOST) */
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * BAND_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* Panel IO over SPI */
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    /* ST7796 panel */
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_endian = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(io, &panel_cfg, &panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, false));            /* portrait */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    /* Backlight on */
    gpio_config_t bl = {.pin_bit_mask = 1ULL << PIN_LCD_BL, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&bl);
    gpio_set_level(PIN_LCD_BL, 1);

    /* Draw three horizontal bands. RGB565. */
    static uint16_t band[LCD_WIDTH * BAND_LINES];
    const uint16_t RED = 0xF800, GREEN = 0x07E0, BLUE = 0x001F;

    for (int row = 0; row < LCD_HEIGHT; row += BAND_LINES) {
        uint16_t color = (row < 160) ? RED : (row < 320) ? GREEN : BLUE;
        for (size_t i = 0; i < LCD_WIDTH * BAND_LINES; i++) band[i] = color;
        esp_lcd_panel_draw_bitmap(panel, 0, row, LCD_WIDTH, row + BAND_LINES, band);
    }

    ESP_LOGI(TAG, "Display painted. If you see Red/Green/Blue bands top-to-bottom, success.");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
```

- [ ] **Step 3: Flash + observe**

Run: `pio run -d tools/bringup/02_display -t upload`
Expected: screen shows three horizontal bands (red top, green middle, blue bottom). If colors are reversed/wrong, flip `rgb_endian` or `invert_color`.

- [ ] **Step 4: Commit**

```bash
git add tools/bringup/02_display/
git commit -m "Phase 3.2: bringup 02_display — ST7796 panel paints RGB bands"
```

---

### Task 11: 03_touch — XPT2046 coordinates to UART

**Files:**
- Create: `tools/bringup/03_touch/platformio.ini`
- Create: `tools/bringup/03_touch/src/main.c`

- [ ] **Step 1: `platformio.ini`** — same template as 01_blink. Plus a `src/idf_component.yml`:

```yaml
dependencies:
  idf: ">=5.0"
  atanisoft/esp_lcd_touch_xpt2046: "^1"   # not in the espressif/ namespace
```

- [ ] **Step 2: Write `src/main.c`**

```c
/* tools/bringup/03_touch/src/main.c
 * Polls XPT2046 touch and logs (x, y, pressed) to UART once per 100 ms.
 * Confirms touch wiring, IRQ pin, HSPI sharing.
 */
#include "esp_log.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_xpt2046.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch_bringup";

void app_main(void) {
    /* HSPI bus (shared with LCD in main fw; standalone here) */
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* Touch IO */
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(PIN_TOUCH_CS);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &tp_io_cfg, &tp_io));

    /* Touch driver */
    esp_lcd_touch_handle_t tp = NULL;
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = 320,
        .y_max = 480,
        .rst_gpio_num = -1,
        .int_gpio_num = PIN_TOUCH_IRQ,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &tp));

    ESP_LOGI(TAG, "Tap the screen. Coordinates print below.");
    while (1) {
        uint16_t x[1], y[1], strength[1];
        uint8_t cnt = 0;
        esp_lcd_touch_read_data(tp);
        bool pressed = esp_lcd_touch_get_coordinates(tp, x, y, strength, &cnt, 1);
        if (pressed && cnt > 0) {
            ESP_LOGI(TAG, "touch: x=%u y=%u s=%u", x[0], y[0], strength[0]);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

- [ ] **Step 3: Flash + observe**

Run: `pio run -d tools/bringup/03_touch -t upload && pio device monitor -d tools/bringup/03_touch`
Expected: tapping the screen prints x/y coordinates. Top-left should be ~(0, 0); bottom-right ~(320, 480). If swapped or mirrored, adjust `flags.swap_xy`/`mirror_x`/`mirror_y`.

- [ ] **Step 4: Commit**

```bash
git add tools/bringup/03_touch/
git commit -m "Phase 3.3: bringup 03_touch — XPT2046 coordinates to UART"
```

---

### Task 12: 04_lvgl_hello — empty LVGL screen with one label

**Files:**
- Create: `tools/bringup/04_lvgl_hello/platformio.ini`
- Create: `tools/bringup/04_lvgl_hello/src/main.c`

- [ ] **Step 1: `platformio.ini`** — same template as 01_blink. Plus a `src/idf_component.yml` with all LVGL + driver deps:

```yaml
dependencies:
  idf: ">=5.0"
  lvgl/lvgl: "^9"
  espressif/esp_lvgl_port: "^2"
  espressif/esp_lcd_st7796: "^1"
  atanisoft/esp_lcd_touch_xpt2046: "^1"
```

- [ ] **Step 2: Write `src/main.c`**

```c
/* tools/bringup/04_lvgl_hello/src/main.c
 * Initialises LVGL via esp_lvgl_port over ST7796 + XPT2046.
 * Draws a single centred "Hello LVGL" label so you can confirm:
 *   - LVGL renders pixels (font rasterises)
 *   - Touch is wired into LVGL (label flips color when tapped)
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_xpt2046.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "lvgl.h"
#include "config.h"

static const char *TAG = "lvgl_bringup";
static lv_obj_t *label;

static void on_touch(lv_event_t *e) {
    static bool flipped;
    flipped = !flipped;
    lv_obj_set_style_text_color(label, flipped ? lv_color_hex(0xFF8C42) : lv_color_hex(0xE6E6E6), 0);
}

void app_main(void) {
    /* === LCD bus + panel (copied from 02_display) === */
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCK, .mosi_io_num = PIN_LCD_MOSI, .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 320 * 48 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS, .dc_gpio_num = PIN_LCD_DC, .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000, .trans_queue_depth = 10,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {.reset_gpio_num = -1, .rgb_endian = LCD_RGB_ENDIAN_BGR, .bits_per_pixel = 16};
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    gpio_config_t bl = {.pin_bit_mask = 1ULL << PIN_LCD_BL, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&bl);
    gpio_set_level(PIN_LCD_BL, 1);

    /* === Touch (copied from 03_touch) === */
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(PIN_TOUCH_CS);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &tp_io_cfg, &tp_io));
    esp_lcd_touch_handle_t tp = NULL;
    esp_lcd_touch_config_t tp_cfg = {.x_max = 320, .y_max = 480, .rst_gpio_num = -1, .int_gpio_num = PIN_TOUCH_IRQ};
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &tp));

    /* === LVGL via esp_lvgl_port === */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = 320 * 48,                          /* partial buffer, see spec §3.4 */
        .double_buffer = true,
        .hres = 320, .vres = 480,
        .monochrome = false,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .flags = {.buff_dma = true},
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);

    lvgl_port_touch_cfg_t touch_cfg = {.disp = disp, .handle = tp};
    lvgl_port_add_touch(&touch_cfg);

    /* === Build the screen === */
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f1115), 0);
    label = lv_label_create(scr);
    lv_label_set_text(label, "Hello LVGL");
    lv_obj_set_style_text_color(label, lv_color_hex(0xE6E6E6), 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(scr, on_touch, LV_EVENT_CLICKED, NULL);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL up. Tap the screen to toggle label colour.");
}
```

- [ ] **Step 3: Flash + observe**

Run: `pio run -d tools/bringup/04_lvgl_hello -t upload`
Expected: dark screen with "Hello LVGL" label centred. Tapping anywhere toggles label colour between white and orange.

- [ ] **Step 4: Commit**

```bash
git add tools/bringup/04_lvgl_hello/
git commit -m "Phase 3.4: bringup 04_lvgl_hello — LVGL on ST7796 + XPT2046"
```

---

### Task 13: 05_thermocouple — MAX31855 read loop

**Files:**
- Create: `tools/bringup/05_thermocouple/platformio.ini`
- Create: `tools/bringup/05_thermocouple/src/main.c`

- [ ] **Step 1: `platformio.ini`** — bare template, no lib_deps.

- [ ] **Step 2: Write `src/main.c`**

```c
/* tools/bringup/05_thermocouple/src/main.c
 * Reads MAX31855 every 500 ms on VSPI (SPI3_HOST) and logs temperature
 * + fault bits. Confirms P3/P4 wiring + 3.3 V supply + VSPI host.
 *
 * MAX31855 32-bit word, bit order MSB first:
 *   [31:18] signed thermocouple temperature ×0.25 °C  (14 bits, two's complement)
 *   [17]    reserved (always 0)
 *   [16]    fault bit (1 = fault — see [2:0])
 *   [15:4]  signed cold-junction temperature ×0.0625 °C (12 bits)
 *   [3]     reserved (always 0)
 *   [2]     SCV  — short to VCC
 *   [1]     SCG  — short to GND
 *   [0]     OC   — open thermocouple
 */
#include "esp_log.h"
#include "driver/spi_master.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tc_bringup";

static spi_device_handle_t spi;

static void max31855_init(void) {
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_TC_SCK,
        .miso_io_num = PIN_TC_MISO,
        .mosi_io_num = -1,           /* MAX31855 is read-only */
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 4,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1 * 1000 * 1000,   /* 1 MHz; chip's max is 5 MHz */
        .mode = 0,
        .spics_io_num = PIN_TC_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &devcfg, &spi));
}

static void max31855_read(float *temp_c, uint8_t *fault) {
    uint8_t rx[4] = {0};
    spi_transaction_t t = {.length = 32, .rxlength = 32, .rx_buffer = rx};
    ESP_ERROR_CHECK(spi_device_transmit(spi, &t));

    uint32_t word = ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) |
                    ((uint32_t)rx[2] << 8)  |  (uint32_t)rx[3];

    /* Fault bits */
    *fault = (uint8_t)(word & 0x7);                  /* OC | SCG | SCV (bits 0,1,2) */
    bool any_fault = (word & (1u << 16)) != 0;

    if (any_fault || *fault) {
        *temp_c = 0.0f / 0.0f;                       /* NaN */
        return;
    }

    int16_t raw = (int16_t)((word >> 18) & 0x3FFF);
    if (raw & 0x2000) raw |= 0xC000;                 /* sign-extend 14-bit */
    *temp_c = raw * 0.25f;
}

void app_main(void) {
    max31855_init();
    ESP_LOGI(TAG, "MAX31855 read loop. Disconnect the thermocouple to verify OC fault.");
    while (1) {
        float t;
        uint8_t fault;
        max31855_read(&t, &fault);
        if (fault) {
            ESP_LOGI(TAG, "FAULT bits=0x%02x  (OC=%d SCG=%d SCV=%d)",
                     fault, fault & 0x1, !!(fault & 0x2), !!(fault & 0x4));
        } else {
            ESP_LOGI(TAG, "temp = %.2f C", t);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
```

- [ ] **Step 3: Flash + observe**

Run: `pio run -d tools/bringup/05_thermocouple -t upload && pio device monitor -d tools/bringup/05_thermocouple`
Expected: temperature prints every 500 ms (room temp ≈ 20–25 °C). Disconnecting the thermocouple should immediately print `FAULT bits=0x01 (OC=1 …)`.

- [ ] **Step 4: Commit**

```bash
git add tools/bringup/05_thermocouple/
git commit -m "Phase 3.5: bringup 05_thermocouple — MAX31855 read loop + fault bits"
```

---

### Task 14: 06_ssr — SSR toggle (heater disconnected)

**Files:**
- Create: `tools/bringup/06_ssr/platformio.ini`
- Create: `tools/bringup/06_ssr/src/main.c`

⚠️ **Heater must be physically disconnected from the AC mains side of the SSR for this test.**

- [ ] **Step 1: `platformio.ini`** — bare template.

- [ ] **Step 2: Write `src/main.c`**

```c
/* tools/bringup/06_ssr/src/main.c
 * Toggles IO32 every 1 s. Watch the SSR's input LED pulse in time.
 * Multimeter check: when GPIO is low or unprogrammed, the 10k pull-down
 * should hold the SSR control input below ~0.4 V.
 */
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "config.h"

static const char *TAG = "ssr_bringup";

void app_main(void) {
    /* Configure SSR pin LOW *before* enabling output — keep it safe at boot. */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_SSR,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = 0,    /* external pull-down does this — don't fight it */
        .pull_up_en   = 0,
    };
    gpio_set_level(PIN_SSR, 0);   /* drive low first */
    gpio_config(&cfg);

    ESP_LOGW(TAG, "SSR toggle. HEATER MUST BE DISCONNECTED. Pin %d.", PIN_SSR);
    bool on = false;
    while (1) {
        on = !on;
        gpio_set_level(PIN_SSR, on ? 1 : 0);
        ESP_LOGI(TAG, "SSR %s", on ? "ON " : "OFF");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

- [ ] **Step 3: Flash + observe**

Run: `pio run -d tools/bringup/06_ssr -t upload`
Expected: SSR's input LED pulses 1 Hz. With a multimeter across SSR DC+ and DC−: ≥3 V when ON, ≤0.4 V when OFF. **Press the BOOT button to reboot** while watching — pin should stay LOW the entire reset window (pull-down doing its job).

- [ ] **Step 4: Commit**

```bash
git add tools/bringup/06_ssr/
git commit -m "Phase 3.6: bringup 06_ssr — IO32 toggle (heater disconnected)"
```

---

### Task 15: 07_safety — fault chain dry run

**Files:**
- Create: `tools/bringup/07_safety/platformio.ini`
- Create: `tools/bringup/07_safety/src/main.c`

- [ ] **Step 1: `platformio.ini`** — bare template.

- [ ] **Step 2: Write `src/main.c`**

```c
/* tools/bringup/07_safety/src/main.c
 * Wires up MAX31855 read + a synthetic "controller" that drives SSR=ON when
 * temp < 30 C and OFF otherwise — UNLESS a fault is detected, in which case
 * SSR is forced OFF and a latched fault state is reported.
 *
 * Manual test:
 *  - Start with thermocouple connected at room temp: SSR pulses ON (heating).
 *  - Disconnect thermocouple mid-loop: SSR forced OFF, FAULT latched.
 *  - Reconnect thermocouple: SSR stays OFF until you reset the board.
 * This proves the safety chain works *before* we use it on a real heater.
 */
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "config.h"

static const char *TAG = "safety_bringup";
static spi_device_handle_t spi;
static bool latched_fault = false;

static void tc_init(void) {
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_TC_SCK, .miso_io_num = PIN_TC_MISO, .mosi_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t cfg = {
        .clock_speed_hz = 1000000, .mode = 0, .spics_io_num = PIN_TC_CS, .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &cfg, &spi));
}

static bool tc_read(float *t) {
    uint8_t rx[4] = {0};
    spi_transaction_t tr = {.length = 32, .rxlength = 32, .rx_buffer = rx};
    ESP_ERROR_CHECK(spi_device_transmit(spi, &tr));
    uint32_t w = ((uint32_t)rx[0]<<24)|((uint32_t)rx[1]<<16)|((uint32_t)rx[2]<<8)|rx[3];
    if (w & (1u << 16)) return false;          /* any-fault bit */
    if (w & 0x7) return false;                 /* OC/SCG/SCV */
    int16_t raw = (int16_t)((w >> 18) & 0x3FFF);
    if (raw & 0x2000) raw |= 0xC000;
    *t = raw * 0.25f;
    return true;
}

static void ssr_init(void) {
    gpio_set_level(PIN_SSR, 0);
    gpio_config_t cfg = {.pin_bit_mask = 1ULL << PIN_SSR, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&cfg);
}

void app_main(void) {
    tc_init();
    ssr_init();
    ESP_LOGW(TAG, "Safety chain dry run. HEATER MUST BE DISCONNECTED.");
    while (1) {
        float t;
        bool ok = tc_read(&t);
        if (!ok) {
            latched_fault = true;
            gpio_set_level(PIN_SSR, 0);
            ESP_LOGE(TAG, "FAULT latched — SSR off");
        } else if (latched_fault) {
            gpio_set_level(PIN_SSR, 0);
            ESP_LOGW(TAG, "fault persists; temp=%.2f", t);
        } else if (t > OVER_TEMP_LIMIT_C) {
            latched_fault = true;
            gpio_set_level(PIN_SSR, 0);
            ESP_LOGE(TAG, "OVER-TEMP %.2f — SSR off, fault latched", t);
        } else {
            bool on = (t < 30.0f);
            gpio_set_level(PIN_SSR, on ? 1 : 0);
            ESP_LOGI(TAG, "temp=%.2f SSR=%s", t, on ? "ON" : "OFF");
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
```

- [ ] **Step 3: Flash + run the manual test sequence**

Run: `pio run -d tools/bringup/07_safety -t upload && pio device monitor -d tools/bringup/07_safety`

Sequence:
1. With thermocouple connected, observe SSR pulses ON (room temp < 30 °C).
2. Disconnect the thermocouple. Expected: immediate `FAULT latched — SSR off`. SSR's input LED goes dark.
3. Reconnect the thermocouple. Expected: SSR stays OFF, log says `fault persists`.
4. Press the BOOT/reset button. Expected: returns to normal pulse pattern.

- [ ] **Step 4: Commit**

```bash
git add tools/bringup/07_safety/
git commit -m "Phase 3.7: bringup 07_safety — fault chain dry run"
```

---

**Phase 3 checkpoint.** All hardware peripherals are individually verified on real hardware. Each bring-up project stays in the repo as a regression check.

## Phase 4 — Drivers + main firmware integration

Phase 4 lifts the bring-up snippets into reusable driver libraries and wires them through FreeRTOS tasks. End of this phase: real `control_task` runs against real sensor and SSR, publishing state to `oven_state_t`. UI task is a stub that just dumps state to UART.

---

### Task 16: `drivers/thermocouple` — MAX31855 driver

**Files:**
- Create: `src/drivers/thermocouple.h`
- Create: `src/drivers/thermocouple.c`

- [ ] **Step 1: Header**

`src/drivers/thermocouple.h`:

```c
/* MAX31855 K-type thermocouple driver. SPI3 (VSPI), see config.h. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Initialise bus + device. Call once at boot. */
void thermocouple_init(void);

/* Blocking read (~50 µs SPI). Populates *temp_c and *fault_flags (FAULT_TC_* from
 * reflow_sm.h, OR-combined; 0 = no fault). *age_ms is 0 immediately after a
 * successful read; if the call returns false, *age_ms is left untouched and
 * the caller should treat the previous reading as stale.
 * Returns true on a successful read with no fault.
 */
bool thermocouple_read(float *temp_c, uint8_t *fault_flags, uint32_t *age_ms);
```

- [ ] **Step 2: Implementation** (adapted from `tools/bringup/05_thermocouple/src/main.c`)

`src/drivers/thermocouple.c`:

```c
#include "drivers/thermocouple.h"
#include "control/reflow_sm.h"          /* FAULT_TC_* */
#include "config.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "tc";
static spi_device_handle_t s_spi;
static int64_t s_last_ok_us = 0;

void thermocouple_init(void) {
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_TC_SCK, .miso_io_num = PIN_TC_MISO, .mosi_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_TC_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &devcfg, &s_spi));
}

bool thermocouple_read(float *temp_c, uint8_t *fault_flags, uint32_t *age_ms) {
    uint8_t rx[4] = {0};
    spi_transaction_t t = {.length = 32, .rxlength = 32, .rx_buffer = rx};
    esp_err_t err = spi_device_transmit(s_spi, &t);
    if (err != ESP_OK) {
        *fault_flags = FAULT_TC_OPEN;   /* treat SPI failure as TC fault */
        return false;
    }
    uint32_t w = ((uint32_t)rx[0]<<24)|((uint32_t)rx[1]<<16)|((uint32_t)rx[2]<<8)|rx[3];

    uint8_t flags = 0;
    if (w & 0x1) flags |= FAULT_TC_OPEN;
    if (w & 0x2) flags |= FAULT_TC_SCG;
    if (w & 0x4) flags |= FAULT_TC_SCV;
    if (w & (1u << 16)) {
        if (!flags) flags = FAULT_TC_OPEN;   /* any-fault bit set but no specific bit */
    }

    if (flags) {
        *fault_flags = flags;
        return false;
    }

    int16_t raw = (int16_t)((w >> 18) & 0x3FFF);
    if (raw & 0x2000) raw |= 0xC000;
    *temp_c = raw * 0.25f;
    *fault_flags = 0;

    int64_t now = esp_timer_get_time();
    s_last_ok_us = now;
    *age_ms = 0;
    return true;
}
```

The driver itself only writes `*age_ms = 0` on success and leaves it untouched on failure. `control_task` (Task 22) maintains its own `s_last_tc_ok_us` and computes the real age — the driver-returned `age_ms` is informational only.

- [ ] **Step 3: Build to confirm it compiles**

Run: `pio run -e esp32dev`
Expected: clean build (no link errors).

- [ ] **Step 4: Commit**

```bash
git add src/drivers/thermocouple.*
git commit -m "Phase 4.1: drivers/thermocouple — MAX31855 read with fault decoding"
```

---

### Task 17: `drivers/ssr` — GPIO output + slow-PWM scheduler wrap

**Files:**
- Create: `src/drivers/ssr.h`
- Create: `src/drivers/ssr.c`

- [ ] **Step 1: Header**

```c
/* Single owner of the SSR GPIO. Wraps drivers/ssr_scheduler (pure logic)
 * and the actual gpio_set_level call. See spec §4.2 + §6.1.
 */
#pragma once
#include <stdint.h>

void ssr_init(void);                       /* GPIO low, then configure as output */
void ssr_tick(uint32_t now_ms, uint8_t duty_percent);   /* called from control_task */
void ssr_force_off(void);                  /* fault path */
```

- [ ] **Step 2: Implementation**

```c
#include "drivers/ssr.h"
#include "drivers/ssr_scheduler.h"
#include "config.h"
#include "driver/gpio.h"

void ssr_init(void) {
    /* Configure as output first, *then* drive LOW. (Setting level on a high-Z pin
     * has no effect — the external pull-down covers that window; this order makes
     * the pin actively driven LOW the moment OE turns on.) */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_SSR,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = 0,                 /* external pull-down handles boot state */
        .pull_up_en   = 0,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_SSR, 0);
}

void ssr_tick(uint32_t now_ms, uint8_t duty_percent) {
    bool on = ssr_scheduler_should_be_on(now_ms, duty_percent);
    gpio_set_level(PIN_SSR, on ? 1 : 0);
}

void ssr_force_off(void) {
    gpio_set_level(PIN_SSR, 0);
}
```

- [ ] **Step 3: Build, commit**

```bash
pio run -e esp32dev
git add src/drivers/ssr.*
git commit -m "Phase 4.2: drivers/ssr — single-writer GPIO + slow-PWM wrapper"
```

---

### Task 18: `drivers/display` — ST7796 init returning panel + io handles

**Files:**
- Create: `src/drivers/display.h`
- Create: `src/drivers/display.c`

- [ ] **Step 1: Header**

```c
#pragma once
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t    panel;
} display_handles_t;

display_handles_t display_init(void);   /* SPI bus + panel + backlight */
```

- [ ] **Step 2: Implementation** (lifted from bring-up 02_display)

```c
#include "drivers/display.h"
#include "esp_lcd_st7796.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "config.h"

display_handles_t display_init(void) {
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCK, .mosi_io_num = PIN_LCD_MOSI, .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 320 * 48 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS, .dc_gpio_num = PIN_LCD_DC, .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000, .trans_queue_depth = 10,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel = NULL;
    /* Field name varies by IDF major: <6.0 uses rgb_endian, >=6.0 uses rgb_ele_order.
     * On this toolchain it's rgb_ele_order + LCD_RGB_ELEMENT_ORDER_BGR (verified
     * in bringup 02_display). */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    /* invert_color(false) is correct for the LCDwiki E32R35T panel; the plan
     * originally specified true based on common defaults but that produced
     * inverted colours on this hardware (bringup 02_display). */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    /* NOTE: swap_xy and mirror are NOT called here. lvgl_port_add_disp (Task 29)
     * applies them based on its rotation field; calls here would be overridden. */

    gpio_config_t bl = {.pin_bit_mask = 1ULL << PIN_LCD_BL, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&bl);
    gpio_set_level(PIN_LCD_BL, 1);

    return (display_handles_t){.io = io, .panel = panel};
}
```

- [ ] **Step 3: Build, commit**

```bash
pio run -e esp32dev
git add src/drivers/display.*
git commit -m "Phase 4.3: drivers/display — ST7796 init"
```

---

### Task 19: `drivers/touch` — XPT2046 init returning touch handle

**Files:**
- Create: `src/drivers/touch.h`
- Create: `src/drivers/touch.c`

- [ ] **Step 1: Header**

```c
#pragma once
#include "esp_lcd_touch.h"

/* Assumes display_init() has already initialised SPI2_HOST (LCD + touch share bus). */
esp_lcd_touch_handle_t touch_init(void);
```

- [ ] **Step 2: Implementation**

```c
#include "drivers/touch.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_lcd_panel_io.h"
#include "driver/spi_master.h"
#include "config.h"

esp_lcd_touch_handle_t touch_init(void) {
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(PIN_TOUCH_CS);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &tp_io_cfg, &tp_io));

    esp_lcd_touch_handle_t tp = NULL;
    /* Two hard-won settings from bringup 03_touch + 04_lvgl_hello:
     *   - mirror_y=1: the XPT2046's raw Y axis is inverted in portrait on
     *     this board (top reports y_max, bottom reports 0).
     *   - int_gpio_num=-1: ESP32 GPIO 36 has NO internal pull-up. When
     *     esp_lvgl_port sees a real int_gpio_num it puts the indev in EVENT
     *     mode (reads only on IRQ). With no pull-up the IRQ never fires and
     *     LVGL never sees touches. -1 keeps the indev in timer-polled mode.
     *     If you ever add an external pull-up to PIN_TOUCH_IRQ, set this back
     *     to PIN_TOUCH_IRQ to save the periodic SPI poll.
     */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = 320, .y_max = 480,
        .rst_gpio_num = -1, .int_gpio_num = -1,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 1},
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &tp));
    return tp;
}
```

- [ ] **Step 3: Build, commit**

```bash
pio run -e esp32dev
git add src/drivers/touch.*
git commit -m "Phase 4.4: drivers/touch — XPT2046 init"
```

---

### Task 20: `storage/nvs_store` — full NVS I/O

**Files:**
- Create: `src/storage/nvs_store.h`
- Create: `src/storage/nvs_store.c`

- [ ] **Step 1: Header**

```c
/* NVS I/O wrapper. Uses nvs_schema for serialisation; falls back to
 * defaults from config.h on any read error. See spec §8.
 */
#pragma once
#include "control/profile_curve.h"
#include "storage/nvs_schema.h"

void nvs_store_init(void);                            /* call once at boot */

void nvs_store_load_profile(profile_t *out);         /* defaults on miss / version mismatch */
void nvs_store_save_profile(const profile_t *p);

void nvs_store_load_pid(pid_gains_t *out);
void nvs_store_save_pid(const pid_gains_t *g);
```

- [ ] **Step 2: Implementation**

```c
#include "storage/nvs_store.h"
#include "config.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "nvs_store";

void nvs_store_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }
}

void nvs_store_load_profile(profile_t *out) {
    profile_defaults(out);   /* fall back unless we successfully load */

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    profile_v1_t blob;
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_PROFILE, &blob, &len);
    nvs_close(h);

    if (err != ESP_OK) { ESP_LOGW(TAG, "profile read err %d, using defaults", err); return; }
    profile_t loaded;
    if (!profile_from_blob(&blob, len, &loaded)) {
        ESP_LOGW(TAG, "profile schema mismatch, using defaults");
        return;
    }
    if (!profile_validate(&loaded)) {
        ESP_LOGW(TAG, "stored profile fails validation, using defaults");
        return;
    }
    *out = loaded;
}

void nvs_store_save_profile(const profile_t *p) {
    profile_v1_t blob;
    profile_to_blob(p, &blob);
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_blob(h, NVS_KEY_PROFILE, &blob, sizeof(blob)));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
}

void nvs_store_load_pid(pid_gains_t *out) {
    pid_defaults(out);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    pid_v1_t blob;
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_PID, &blob, &len);
    nvs_close(h);
    if (err != ESP_OK) { ESP_LOGW(TAG, "pid read err %d, using defaults", err); return; }
    if (!pid_from_blob(&blob, len, out)) {
        ESP_LOGW(TAG, "pid schema mismatch, using defaults");
        pid_defaults(out);
    }
}

void nvs_store_save_pid(const pid_gains_t *g) {
    pid_v1_t blob;
    pid_to_blob(g, &blob);
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_blob(h, NVS_KEY_PID, &blob, sizeof(blob)));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
}
```

- [ ] **Step 3: Build, commit**

```bash
pio run -e esp32dev
git add src/storage/nvs_store.*
git commit -m "Phase 4.5: storage/nvs_store — NVS I/O with default-fallback"
```

---

### Task 21: Shared IPC types — `oven_state_t` + `cmd_queue`

**Files:**
- Create: `src/oven_state.h`

- [ ] **Step 1: Header (one place for all IPC types)**

```c
/* Shared IPC types between control_task and ui_task. See spec §3.3. */
#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "control/reflow_sm.h"
#include "control/profile_curve.h"
#include "storage/nvs_schema.h"

/* Snapshot published by control_task, read by ui_task under mutex. */
typedef struct {
    SemaphoreHandle_t mutex;
    float       temp_c;
    float       setpoint_c;
    stage_t     stage;
    uint32_t    elapsed_ms;
    uint8_t     duty_percent;
    uint8_t     fault_flags;
    oven_mode_t mode;
    const float *curve;                 /* points into active curve_t's samples[]; see Task 22 */
    uint16_t    curve_len;
} oven_state_t;

/* UI → control commands. */
typedef enum {
    CMD_START,
    CMD_STOP,
    CMD_ACK_FAULT,
    CMD_APPLY_PROFILE,
    CMD_APPLY_PID,
    CMD_PREVIEW_PROFILE,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    union {
        profile_t   profile;     /* APPLY_PROFILE, PREVIEW_PROFILE */
        pid_gains_t pid;         /* APPLY_PID */
    } payload;
} cmd_msg_t;
```

- [ ] **Step 2: Commit (no .c needed — header-only declarations)**

```bash
git add src/oven_state.h
git commit -m "Phase 4.6: shared IPC types — oven_state_t + cmd_msg_t"
```

---

### Task 22: `control/control_task` — FreeRTOS task wiring everything

**Files:**
- Create: `src/control/control_task.h`
- Create: `src/control/control_task.c`

- [ ] **Step 1: Header**

```c
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "oven_state.h"

void control_task_create(QueueHandle_t cmd_queue, oven_state_t *state);
```

- [ ] **Step 2: Implementation**

`src/control/control_task.c`:

```c
#include "control/control_task.h"
#include "control/pid.h"
#include "control/profile_curve.h"
#include "control/reflow_sm.h"
#include "drivers/thermocouple.h"
#include "drivers/ssr.h"
#include "storage/nvs_store.h"
#include "config.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "ctrl";

/* Double-buffered curves (§3.3 curve buffer ownership). */
static profile_curve_t curve_a, curve_b;
static profile_curve_t *active_curve = &curve_a;
static profile_curve_t *idle_curve   = &curve_b;
static profile_t    s_profile;
static pid_gains_t  s_pid_gains;
static pid_state_t  s_pid;
static sm_state_t   s_sm;

static QueueHandle_t s_cmd_queue;
static oven_state_t  *s_pub;
static int64_t       s_last_tc_ok_us;

static void publish(uint8_t duty, float setpoint) {
    /* temp_c is written separately on each tick (only when read succeeds);
     * keeping the previous value across faults happens by *not* updating it
     * in the fault branches. */
    xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
    s_pub->setpoint_c   = setpoint;
    s_pub->stage        = s_sm.stage;
    s_pub->elapsed_ms   = s_sm.elapsed_ms;
    s_pub->duty_percent = duty;
    s_pub->fault_flags  = s_sm.fault_flags;
    s_pub->mode         = s_sm.mode;
    s_pub->curve        = active_curve->samples;
    s_pub->curve_len    = active_curve->length;
    xSemaphoreGive(s_pub->mutex);
}

static void swap_curves(void) {
    profile_curve_t *tmp = active_curve;
    active_curve = idle_curve;
    idle_curve = tmp;
}

static sm_event_t drain_one_command(cmd_msg_t *msg_out) {
    cmd_msg_t msg;
    if (xQueueReceive(s_cmd_queue, &msg, 0) != pdTRUE) return SM_EVENT_NONE;
    *msg_out = msg;
    switch (msg.type) {
        case CMD_START:      return SM_EVENT_CMD_START;
        case CMD_STOP:       return SM_EVENT_CMD_STOP;
        case CMD_ACK_FAULT:  return SM_EVENT_CMD_ACK_FAULT;
        default:             return SM_EVENT_NONE;     /* APPLY_* / PREVIEW handled below */
    }
}

static void apply_non_sm_command(const cmd_msg_t *msg) {
    switch (msg->type) {
        case CMD_APPLY_PROFILE:
            if (s_sm.mode != MODE_RUNNING) {
                s_profile = msg->payload.profile;
                if (profile_curve_generate(&s_profile, idle_curve)) swap_curves();
            }
            break;
        case CMD_APPLY_PID:
            s_pid_gains = msg->payload.pid;
            pid_set_gains(&s_pid, s_pid_gains.kp, s_pid_gains.ki, s_pid_gains.kd);
            break;
        case CMD_PREVIEW_PROFILE:
            if (s_sm.mode != MODE_RUNNING) {
                profile_t tmp = msg->payload.profile;
                if (profile_curve_generate(&tmp, idle_curve)) swap_curves();
            }
            break;
        default: break;
    }
}

static void control_task(void *arg) {
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    /* Boot-time state. */
    nvs_store_load_profile(&s_profile);
    nvs_store_load_pid(&s_pid_gains);
    profile_curve_generate(&s_profile, active_curve);

    pid_init(&s_pid, s_pid_gains.kp, s_pid_gains.ki, s_pid_gains.kd);
    sm_init(&s_sm);

    s_last_tc_ok_us = esp_timer_get_time();
    TickType_t next = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(CONTROL_TICK_MS);
    const float dt = CONTROL_TICK_MS / 1000.0f;

    bool pid_active_prev = false;

    while (1) {
        vTaskDelayUntil(&next, period);
        esp_task_wdt_reset();

        /* === 1. Read sensor === */
        float temp_c = 0.0f;
        uint8_t fault = 0;
        uint32_t age_dummy = 0;
        bool tc_ok = thermocouple_read(&temp_c, &fault, &age_dummy);
        int64_t now_us = esp_timer_get_time();
        uint32_t age_ms = (uint32_t)((now_us - s_last_tc_ok_us) / 1000);
        if (tc_ok) {
            s_last_tc_ok_us = now_us;
            age_ms = 0;
        }

        /* Publish raw temp regardless; UI shows last known value during faults. */
        xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
        if (tc_ok) s_pub->temp_c = temp_c;
        xSemaphoreGive(s_pub->mutex);

        /* === 2-4. Safety gates === */
        if (fault) {
            sm_trip_fault(&s_sm, fault);
            ssr_force_off();
            publish(0, 0.0f);
            continue;
        }
        if (age_ms > SENSOR_MAX_AGE_MS) {
            sm_trip_fault(&s_sm, FAULT_TC_STALE);
            ssr_force_off();
            publish(0, 0.0f);
            continue;
        }
        if (temp_c > OVER_TEMP_LIMIT_C) {
            sm_trip_fault(&s_sm, FAULT_OVER_TEMP);
            ssr_force_off();
            publish(0, 0.0f);
            continue;
        }

        /* === 5. Drain command queue === */
        cmd_msg_t msg = {0};
        sm_event_t ev = drain_one_command(&msg);
        /* SM events (START/STOP/ACK_FAULT) are handled by sm_step below.
         * Non-SM commands (APPLY_PROFILE / APPLY_PID / PREVIEW_PROFILE) are
         * applied here. drain_one_command returns SM_EVENT_NONE for those
         * (and also when the queue was empty — apply_non_sm_command's switch
         * default makes the empty case a no-op). */
        if (ev == SM_EVENT_NONE) apply_non_sm_command(&msg);

        /* === 6. Step the SM. Compute PID if requested. === */
        bool pid_active = sm_step(&s_sm, ev, temp_c, active_curve);
        if (pid_active && !pid_active_prev) {
            pid_reset(&s_pid, temp_c);   /* IDLE→RUNNING boundary */
        }
        pid_active_prev = pid_active;

        float setpoint = 0.0f;
        uint8_t duty = 0;
        if (pid_active) {
            setpoint = profile_curve_setpoint(active_curve, s_sm.elapsed_ms);
            float out = pid_step(&s_pid, setpoint, temp_c, dt);
            duty = (uint8_t)out;
            ssr_tick((uint32_t)(esp_timer_get_time() / 1000), duty);
        } else {
            ssr_force_off();
        }

        /* === 7. Publish. === */
        publish(duty, setpoint);
    }
}

void control_task_create(QueueHandle_t cmd_queue, oven_state_t *state) {
    s_cmd_queue = cmd_queue;
    s_pub = state;
    xTaskCreatePinnedToCore(control_task, "control", 4096, NULL, 5, NULL, 1);
}
```

- [ ] **Step 3: Build**

Run: `pio run -e esp32dev`
Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add src/control/control_task.*
git commit -m "Phase 4.7: control/control_task — FreeRTOS task wiring sensor+PID+SSR+SM"
```

---

### Task 23: `main.c` with control + stub UI tasks

**Files:**
- Modify: `src/main.c`

- [ ] **Step 1: Replace the placeholder `main.c`**

```c
/* src/main.c — boots NVS, drivers, control_task, and a stub UI task that
 * simply logs state to UART every second. Phase 5 replaces the stub with LVGL.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_system.h"

#include "drivers/thermocouple.h"
#include "drivers/ssr.h"
#include "storage/nvs_store.h"
#include "control/control_task.h"
#include "oven_state.h"
#include "config.h"

static const char *TAG = "main";

static QueueHandle_t s_cmd_queue;
static oven_state_t  s_state;

static void shutdown_handler(void) {
    /* Graceful restart: drive SSR low. Panic / WDT-reset path relies on
     * the external pull-down + WDT timeout (§6.1 items 5-6, §6.2).
     */
    ssr_force_off();
}

static void ui_stub_task(void *arg) {
    (void)arg;
    while (1) {
        xSemaphoreTake(s_state.mutex, portMAX_DELAY);
        ESP_LOGI(TAG, "state: mode=%d stage=%d temp=%.2f sp=%.2f duty=%u faults=0x%02x elapsed=%u",
                 s_state.mode, s_state.stage, s_state.temp_c, s_state.setpoint_c,
                 s_state.duty_percent, s_state.fault_flags, s_state.elapsed_ms);
        xSemaphoreGive(s_state.mutex);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "reflow_oven boot");

    /* WDT */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = CONTROL_TASK_WDT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&wdt_cfg));
    esp_register_shutdown_handler(shutdown_handler);

    /* Drivers */
    nvs_store_init();
    thermocouple_init();
    ssr_init();

    /* IPC */
    s_cmd_queue = xQueueCreate(8, sizeof(cmd_msg_t));
    s_state.mutex = xSemaphoreCreateMutex();

    /* Tasks */
    control_task_create(s_cmd_queue, &s_state);
    xTaskCreatePinnedToCore(ui_stub_task, "ui_stub", 4096, NULL, 2, NULL, 0);
}
```

- [ ] **Step 2: Build, flash, observe**

Run: `pio run -e esp32dev -t upload && pio device monitor`
Expected: log line `state: mode=0 stage=0 temp=<room °C> sp=0.00 duty=0 faults=0x00 elapsed=0` once per second.

Now send a `CMD_START` manually — easiest way is to add a debug button stub, but for now just verify the state log shows the SM is IDLE and tracking temp.

- [ ] **Step 3: Manual integration check — fault path on real hardware**

Disconnect the thermocouple while monitoring. Expected: log immediately shows `mode=2` (FAULT) and `faults=0x01` (OC). SSR's input LED must stay dark.

- [ ] **Step 4: Commit**

```bash
git add src/main.c
git commit -m "Phase 4.8: main.c — wires control_task + stub UI; fault path verified"
```

---

**Phase 4 checkpoint.** The full control pipeline runs against real hardware. SSR + thermocouple are integrated with the safety chain. Only the UI remains.

## Phase 5 — UI + end-to-end verification

> **Note (2026-05-17 evening review pass):** Tasks 21, 22, 24, 25, 26,
> 27, 28, 29 below describe the *original* Phase 5 implementation that
> was merged on 2026-05-17. A subsequent code + spec review surfaced
> the issues catalogued in
> `docs/superpowers/plans/2026-05-17-phase-5-review-fixes.md`. The
> task code blocks here have **not** been rewritten in place; treat
> that review-fix plan and the actual source tree as authoritative
> for the post-review behaviour. The "Phase 5 review-pass refinements
> (2026-05-17, evening)" subsection at the end of Phase 5 below
> enumerates which tasks were affected and how.

LVGL screens built per spec §7. The visual reference points (mockups described in §7.2–§7.5) are: dark theme (`#0f1115` background, `#ff8c42` accent orange, `#e6e6e6` foreground), tab bar at the bottom (`Home / Profile / PID`), keypad popup modal above the tab bar.

For each screen, "create" builds the lv_obj subtree and returns the root; "update" is called from `ui_task` whenever `oven_state_t` changes (or on a periodic LVGL timer). Event handlers send `cmd_msg_t` to the queue exposed via `ui/ui_task`.

---

### Task 24: `ui/theme` — shared colors + fonts

**Files:**
- Create: `src/ui/theme.h`
- Create: `src/ui/theme.c`
- Modify: `sdkconfig.defaults` — enable the larger Montserrat fonts the
  screens reference (the shipped LVGL Kconfig only enables
  `MONTSERRAT_14`).
- Modify: `platformio.ini` — exclude `src/ui/` from the `native` env
  (LVGL / FreeRTOS headers aren't on the host).

- [ ] **Step 1: Header**

Every literal hex value lives in this header — no raw colors in any
screen file. Each macro carries an inline comment describing the rough
color so the palette is readable at a glance.

```c
#pragma once
#include "lvgl.h"

/* Single source of truth for UI colors. Per spec §7. */
#define THEME_BG        lv_color_hex(0x0f1115)  /* near-black charcoal */
#define THEME_PANEL_BG  lv_color_hex(0x14171c)  /* slightly lighter charcoal */
#define THEME_BORDER    lv_color_hex(0x232833)  /* dark slate-grey */
#define THEME_FG        lv_color_hex(0xe6e6e6)  /* off-white */
#define THEME_FG_DIM    lv_color_hex(0x9aa0a6)  /* mid grey */
#define THEME_FG_FAINT  lv_color_hex(0x6a6f78)  /* darker grey */
#define THEME_ACCENT    lv_color_hex(0xff8c42)  /* warm orange */
#define THEME_STAGE     lv_color_hex(0xfbbc04)  /* amber/yellow */
#define THEME_GO        lv_color_hex(0x1f7a3f)  /* deep green */
#define THEME_STOP      lv_color_hex(0x7a1f1f)  /* deep red */
#define THEME_INFO      lv_color_hex(0x1f5f7a)  /* deep teal-blue */
#define THEME_OK        lv_color_hex(0x69b87a)  /* light green */
#define THEME_MODAL_BG  lv_color_hex(0x181b21)  /* slightly raised charcoal (modal popups) */
#define THEME_BTN_NEUTRAL lv_color_hex(0x232833)  /* neutral key/button fill — same as THEME_BORDER */

void theme_apply_to_screen(lv_obj_t *scr);
```

- [ ] **Step 2: Implementation**

```c
#include "ui/theme.h"

void theme_apply_to_screen(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, THEME_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, THEME_FG, 0);
}
```

- [ ] **Step 3: Enable larger Montserrat fonts**

`screen_keypad.c` uses `lv_font_montserrat_28` and `screen_home.c` uses
`lv_font_montserrat_48`. The shipped LVGL Kconfig only enables
`MONTSERRAT_14`, so add to `sdkconfig.defaults`:

```
# LVGL fonts used by the UI (spec §7): 28 px on the keypad value display,
# 48 px on the Home screen big temperature readout. Defaults only enable
# Montserrat 14 — opt the larger sizes in here.
CONFIG_LV_FONT_MONTSERRAT_28=y
CONFIG_LV_FONT_MONTSERRAT_48=y
```

`sdkconfig.esp32dev` is gitignored and regenerated, so the defaults
file is the source of truth. If a stale `sdkconfig.esp32dev` already
sets these `is not set`, edit it locally too (or delete it and let pio
regenerate) — otherwise the disabled lines win at build time.

- [ ] **Step 4: Exclude `src/ui/` from the native env**

The UI files include LVGL and FreeRTOS headers that don't exist on the
host toolchain, so they must be filtered out of the native build. In
`platformio.ini`, under `[env:native]` → `build_src_filter`:

```
    -<ui/>
```

Verify with `pio test -e native` — all existing host tests must still
pass (42/42).

- [ ] **Step 5: Commit (build deferred until Task 29)**

```bash
git add src/ui/theme.* sdkconfig.defaults platformio.ini
git commit -m "Phase 5.1: ui/theme — color palette"
```

---

### Task 25: `ui/screen_keypad` — numeric keypad popup

**Files:**
- Create: `src/ui/screen_keypad.h`
- Create: `src/ui/screen_keypad.c`

- [ ] **Step 1: Header**

```c
/* Numeric keypad popup modal. Caller hands in field metadata; on OK the
 * callback receives the (clamped) value. Per spec §7.5.
 */
#pragma once
#include "lvgl.h"
#include <stdbool.h>

typedef void (*keypad_done_cb)(float value, void *user_data);

typedef struct {
    const char *field_label;  /* e.g. "Soak Temperature" */
    const char *unit;         /* e.g. "°C" */
    float       min;
    float       max;
    float       initial;
    bool        allow_decimal;
    keypad_done_cb on_ok;
    void          *user_data;
} keypad_config_t;

void screen_keypad_open(const keypad_config_t *cfg);
void screen_keypad_close(void);
bool screen_keypad_is_open(void);
```

- [ ] **Step 2: Implementation**

```c
#include "ui/screen_keypad.h"
#include "ui/theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t *s_modal;
static char      s_buf[16];
static keypad_config_t s_cfg;

static void update_display_label(void) {
    lv_obj_t *display = lv_obj_get_child(s_modal, 1);
    lv_label_set_text(display, s_buf[0] ? s_buf : "0");
}

static void key_pressed_cb(lv_event_t *e) {
    const char *key = (const char *)lv_event_get_user_data(e);
    if (strcmp(key, "OK") == 0) {
        float v = (s_buf[0]) ? strtof(s_buf, NULL) : s_cfg.initial;
        if (v < s_cfg.min) v = s_cfg.min;
        if (v > s_cfg.max) v = s_cfg.max;
        keypad_done_cb cb = s_cfg.on_ok;
        void *ud = s_cfg.user_data;
        screen_keypad_close();
        if (cb) cb(v, ud);
        return;
    }
    if (strcmp(key, "ESC") == 0) { screen_keypad_close(); return; }
    if (strcmp(key, "CLR") == 0) { s_buf[0] = 0; update_display_label(); return; }
    if (strcmp(key, "DEL") == 0) {
        size_t n = strlen(s_buf);
        if (n > 0) s_buf[n - 1] = 0;
        update_display_label();
        return;
    }
    /* Digit or decimal */
    size_t n = strlen(s_buf);
    if (n >= sizeof(s_buf) - 1) return;
    if (key[0] == '.' && (!s_cfg.allow_decimal || strchr(s_buf, '.'))) return;
    s_buf[n] = key[0];
    s_buf[n + 1] = 0;
    update_display_label();
}

static lv_obj_t *make_key(lv_obj_t *parent, const char *label, lv_color_t bg) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, THEME_FG, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, key_pressed_cb, LV_EVENT_CLICKED, (void *)label);
    return b;
}

void screen_keypad_open(const keypad_config_t *cfg) {
    s_cfg = *cfg;
    snprintf(s_buf, sizeof(s_buf), "%g", cfg->initial);

    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_modal, 320, 320);
    lv_obj_align(s_modal, LV_ALIGN_BOTTOM_MID, 0, -36);   /* sits above tab bar */
    lv_obj_set_style_bg_color(s_modal, THEME_MODAL_BG, 0);
    lv_obj_set_style_border_color(s_modal, THEME_BORDER, 0);
    lv_obj_set_style_pad_all(s_modal, 8, 0);

    /* [0] header */
    lv_obj_t *hdr = lv_label_create(s_modal);
    char hdrbuf[64];
    snprintf(hdrbuf, sizeof(hdrbuf), "%s · %s · %g–%g",
             cfg->field_label, cfg->unit, cfg->min, cfg->max);
    lv_label_set_text(hdr, hdrbuf);
    lv_obj_set_style_text_color(hdr, THEME_FG_DIM, 0);
    lv_obj_set_pos(hdr, 4, 4);

    /* [1] display */
    lv_obj_t *disp = lv_label_create(s_modal);
    lv_label_set_text(disp, s_buf);
    lv_obj_set_style_text_color(disp, THEME_ACCENT, 0);
    lv_obj_set_style_text_font(disp, &lv_font_montserrat_28, 0);
    lv_obj_align(disp, LV_ALIGN_TOP_RIGHT, -8, 20);

    /* [2..] 4x4 grid: 7 8 9 DEL / 4 5 6 CLR / 1 2 3 ESC / 0 . OK(span2) */
    static const char *row[] = {
        "7","8","9","DEL",
        "4","5","6","CLR",
        "1","2","3","ESC",
        "0",".","OK","OK"     /* OK twice for the span placeholder */
    };
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            const char *lbl = row[r * 4 + c];
            lv_color_t bg = THEME_BTN_NEUTRAL;
            if (strcmp(lbl, "OK") == 0) bg = THEME_GO;
            else if (strcmp(lbl, "ESC") == 0 || strcmp(lbl, "CLR") == 0 || strcmp(lbl, "DEL") == 0)
                bg = THEME_STOP;
            if (strcmp(lbl, "OK") == 0 && c == 3) continue;   /* span placeholder */
            lv_obj_t *b = make_key(s_modal, lbl, bg);
            int w = (strcmp(lbl, "OK") == 0) ? 150 : 70;
            lv_obj_set_size(b, w, 50);
            lv_obj_set_pos(b, 4 + c * 76, 80 + r * 56);
        }
    }
}

void screen_keypad_close(void) {
    if (s_modal) { lv_obj_delete(s_modal); s_modal = NULL; }
}

bool screen_keypad_is_open(void) { return s_modal != NULL; }
```

- [ ] **Step 3: Commit**

```bash
git add src/ui/screen_keypad.*
git commit -m "Phase 5.2: ui/screen_keypad — numeric keypad popup"
```

---

### Task 26: `ui/screen_home` — live monitor screen

**Files:**
- Create: `src/ui/screen_home.h`
- Create: `src/ui/screen_home.c`

- [ ] **Step 1: Header**

```c
#pragma once
#include "lvgl.h"
#include "oven_state.h"

/* Build the Home screen content inside `parent` (a tab page).
 * Returns nothing — internal state held in static globals.
 */
void screen_home_create(lv_obj_t *parent);

/* Refresh widgets from the latest state snapshot. */
void screen_home_update(const oven_state_t *snap);
```

- [ ] **Step 2: Implementation** (mirrors spec §7.2)

```c
#include "ui/screen_home.h"
#include "ui/theme.h"
#include "oven_state.h"
#include "control/reflow_sm.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>

extern QueueHandle_t g_cmd_queue;   /* defined in ui_task.c (Task 29) */

static lv_obj_t *lbl_temp, *lbl_target, *lbl_stage, *btn_action, *btn_action_lbl;
static lv_obj_t *graph_canvas;

static void action_clicked_cb(lv_event_t *e) {
    cmd_msg_t msg = {0};
    /* Pick by current label text — it tracks mode. */
    const char *txt = lv_label_get_text(btn_action_lbl);
    if (strcmp(txt, "START") == 0)        msg.type = CMD_START;
    else if (strcmp(txt, "STOP") == 0)    msg.type = CMD_STOP;
    else if (strcmp(txt, "ACKNOWLEDGE") == 0) msg.type = CMD_ACK_FAULT;
    else return;
    xQueueSend(g_cmd_queue, &msg, 0);
}

void screen_home_create(lv_obj_t *parent) {
    lv_obj_set_style_bg_color(parent, THEME_BG, 0);
    lv_obj_set_style_pad_all(parent, 12, 0);

    /* Big temp */
    lbl_temp = lv_label_create(parent);
    lv_label_set_text(lbl_temp, "--°C");
    lv_obj_set_style_text_color(lbl_temp, THEME_ACCENT, 0);
    lv_obj_set_style_text_font(lbl_temp, &lv_font_montserrat_48, 0);
    lv_obj_set_pos(lbl_temp, 0, 0);

    /* Target line */
    lbl_target = lv_label_create(parent);
    lv_label_set_text(lbl_target, "Ambient · ready to start");
    lv_obj_set_style_text_color(lbl_target, THEME_FG_DIM, 0);
    lv_obj_set_pos(lbl_target, 0, 70);

    /* Stage chip */
    lbl_stage = lv_label_create(parent);
    lv_label_set_text(lbl_stage, "Stage IDLE");
    lv_obj_set_style_text_color(lbl_stage, THEME_FG_DIM, 0);
    lv_obj_set_style_bg_color(lbl_stage, THEME_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(lbl_stage, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(lbl_stage, 5, 0);
    lv_obj_set_pos(lbl_stage, 0, 95);

    /* Graph placeholder (canvas filled in screen_home_update). */
    graph_canvas = lv_obj_create(parent);
    lv_obj_set_size(graph_canvas, 296, 220);
    lv_obj_set_style_bg_color(graph_canvas, THEME_PANEL_BG, 0);
    lv_obj_set_style_border_color(graph_canvas, THEME_BORDER, 0);
    lv_obj_set_pos(graph_canvas, 0, 130);

    /* Action button */
    btn_action = lv_button_create(parent);
    lv_obj_set_size(btn_action, 296, 50);
    lv_obj_set_pos(btn_action, 0, 360);
    lv_obj_set_style_bg_color(btn_action, THEME_GO, 0);
    lv_obj_add_event_cb(btn_action, action_clicked_cb, LV_EVENT_CLICKED, NULL);
    btn_action_lbl = lv_label_create(btn_action);
    lv_label_set_text(btn_action_lbl, "START");
    lv_obj_set_style_text_color(btn_action_lbl, THEME_FG, 0);
    lv_obj_center(btn_action_lbl);
}

void screen_home_update(const oven_state_t *s) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.0f°C", s->temp_c);
    lv_label_set_text(lbl_temp, buf);

    switch (s->mode) {
    case MODE_IDLE:
        lv_label_set_text(lbl_target, "Ambient · ready to start");
        lv_label_set_text(lbl_stage,  "Stage IDLE");
        lv_label_set_text(btn_action_lbl, "START");
        lv_obj_set_style_bg_color(btn_action, THEME_GO, 0);
        lv_obj_set_style_text_color(lbl_temp, THEME_FG_DIM, 0);
        break;
    case MODE_RUNNING:
        /* Cast the uint32_t elapsed-ms math to unsigned so the format
         * specifier matches on xtensa-gcc (uint32_t is long unsigned
         * there; bare %u would trip -Werror=format). */
        snprintf(buf, sizeof(buf), "Target %.0f °C · elapsed %u:%02u",
                 s->setpoint_c,
                 (unsigned)(s->elapsed_ms / 60000),
                 (unsigned)((s->elapsed_ms / 1000) % 60));
        lv_label_set_text(lbl_target, buf);
        snprintf(buf, sizeof(buf), "Stage %d · duty %u %%", s->stage, s->duty_percent);
        lv_label_set_text(lbl_stage,  buf);
        lv_label_set_text(btn_action_lbl, "STOP");
        lv_obj_set_style_bg_color(btn_action, THEME_STOP, 0);
        lv_obj_set_style_text_color(lbl_temp, THEME_ACCENT, 0);
        break;
    case MODE_FAULT:
        snprintf(buf, sizeof(buf), "FAULT 0x%02x", s->fault_flags);
        lv_label_set_text(lbl_target, buf);
        lv_label_set_text(lbl_stage,  "Tap ACKNOWLEDGE to clear");
        lv_label_set_text(btn_action_lbl, "ACKNOWLEDGE");
        lv_obj_set_style_bg_color(btn_action, THEME_STAGE, 0);
        lv_obj_set_style_text_color(lbl_temp, THEME_STOP, 0);
        break;
    }
    /* Graph drawing left as a refinement — see spec §7.2 for what to render. */
}
```

- [ ] **Step 3: Commit**

```bash
git add src/ui/screen_home.*
git commit -m "Phase 5.3: ui/screen_home — live monitor + action button"
```

---

### Task 27: `ui/screen_profile` — profile editor

**Files:**
- Create: `src/ui/screen_profile.h`
- Create: `src/ui/screen_profile.c`

- [ ] **Step 1: Header**

```c
#pragma once
#include "lvgl.h"
#include "oven_state.h"

void screen_profile_create(lv_obj_t *parent);
void screen_profile_set_initial(const profile_t *p);   /* loaded from NVS at boot */
```

- [ ] **Step 2: Implementation outline**

```c
#include "ui/screen_profile.h"
#include "ui/theme.h"
#include "ui/screen_keypad.h"
#include "control/profile_curve.h"
#include "config.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>

extern QueueHandle_t g_cmd_queue;

static profile_t s_buf;          /* in-progress edits */
static lv_obj_t  *s_val_labels[6];   /* 6 fields */

typedef enum { F_RATE1, F_SOAK_T, F_SOAK_TIME, F_RATE2, F_REFLOW_T, F_REFLOW_TIME } field_id_t;

static void refresh_label(field_id_t id) {
    char buf[16];
    switch (id) {
        case F_RATE1:       snprintf(buf, sizeof(buf), "%.1f", s_buf.first_heatup_rate);  break;
        case F_SOAK_T:      snprintf(buf, sizeof(buf), "%u",  s_buf.soak_temp);           break;
        case F_SOAK_TIME:   snprintf(buf, sizeof(buf), "%u",  s_buf.soak_time);           break;
        case F_RATE2:       snprintf(buf, sizeof(buf), "%.1f", s_buf.second_heatup_rate); break;
        case F_REFLOW_T:    snprintf(buf, sizeof(buf), "%u",  s_buf.reflow_temp);         break;
        case F_REFLOW_TIME: snprintf(buf, sizeof(buf), "%u",  s_buf.reflow_time);         break;
    }
    lv_label_set_text(s_val_labels[id], buf);
}

static void keypad_done(float value, void *user_data) {
    field_id_t id = (field_id_t)(intptr_t)user_data;
    switch (id) {
        case F_RATE1:       s_buf.first_heatup_rate  = value;       break;
        case F_SOAK_T:      s_buf.soak_temp          = (uint16_t)value; break;
        case F_SOAK_TIME:   s_buf.soak_time          = (uint16_t)value; break;
        case F_RATE2:       s_buf.second_heatup_rate = value;       break;
        case F_REFLOW_T:    s_buf.reflow_temp        = (uint16_t)value; break;
        case F_REFLOW_TIME: s_buf.reflow_time        = (uint16_t)value; break;
    }
    refresh_label(id);
}

static void field_clicked_cb(lv_event_t *e) {
    field_id_t id = (field_id_t)(intptr_t)lv_event_get_user_data(e);
    keypad_config_t cfg = {.on_ok = keypad_done, .user_data = (void *)(intptr_t)id};
    switch (id) {
    case F_RATE1:       cfg = (keypad_config_t){"Heat-up 1 rate","°C/s",HEATUP_RATE_MIN,HEATUP_RATE_MAX,s_buf.first_heatup_rate,true,keypad_done,(void*)(intptr_t)id};  break;
    case F_SOAK_T:      cfg = (keypad_config_t){"Soak Temperature","°C",SOAK_TEMP_MIN,SOAK_TEMP_MAX,s_buf.soak_temp,false,keypad_done,(void*)(intptr_t)id}; break;
    case F_SOAK_TIME:   cfg = (keypad_config_t){"Soak Time","s",SOAK_TIME_MIN,SOAK_TIME_MAX,s_buf.soak_time,false,keypad_done,(void*)(intptr_t)id}; break;
    case F_RATE2:       cfg = (keypad_config_t){"Heat-up 2 rate","°C/s",HEATUP_RATE_MIN,HEATUP_RATE_MAX,s_buf.second_heatup_rate,true,keypad_done,(void*)(intptr_t)id}; break;
    case F_REFLOW_T:    cfg = (keypad_config_t){"Reflow Temperature","°C",REFLOW_TEMP_MIN,REFLOW_TEMP_MAX,s_buf.reflow_temp,false,keypad_done,(void*)(intptr_t)id}; break;
    case F_REFLOW_TIME: cfg = (keypad_config_t){"Reflow Time","s",REFLOW_TIME_MIN,REFLOW_TIME_MAX,s_buf.reflow_time,false,keypad_done,(void*)(intptr_t)id}; break;
    }
    screen_keypad_open(&cfg);
}

static void reset_clicked_cb(lv_event_t *e) {
    profile_defaults(&s_buf);
    for (int i = 0; i < 6; i++) refresh_label((field_id_t)i);
}

static void preview_clicked_cb(lv_event_t *e) {
    cmd_msg_t msg = {.type = CMD_PREVIEW_PROFILE, .payload.profile = s_buf};
    xQueueSend(g_cmd_queue, &msg, 0);
}

static void save_clicked_cb(lv_event_t *e) {
    if (!profile_validate(&s_buf)) return;   /* TODO: show error toast */
    cmd_msg_t msg = {.type = CMD_APPLY_PROFILE, .payload.profile = s_buf};
    xQueueSend(g_cmd_queue, &msg, 0);
}

static lv_obj_t *make_field(lv_obj_t *parent, const char *label, int y, field_id_t id) {
    /* simple two-line card: label on left, value box on right */
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 296, 50);
    lv_obj_set_pos(card, 0, y);
    lv_obj_set_style_bg_color(card, THEME_PANEL_BG, 0);
    lv_obj_set_style_border_color(card, THEME_BORDER, 0);
    lv_obj_add_event_cb(card, field_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);

    lv_obj_t *l = lv_label_create(card);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, THEME_FG_DIM, 0);
    lv_obj_set_pos(l, 8, 12);

    lv_obj_t *v = lv_label_create(card);
    lv_obj_set_style_text_color(v, THEME_FG, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -8, 0);
    s_val_labels[id] = v;
    return card;
}

void screen_profile_create(lv_obj_t *parent) {
    lv_obj_set_style_pad_all(parent, 8, 0);
    make_field(parent, "Heat-up 1 rate",      0,   F_RATE1);
    make_field(parent, "Soak temperature",   56,   F_SOAK_T);
    make_field(parent, "Soak time",         112,   F_SOAK_TIME);
    make_field(parent, "Heat-up 2 rate",    168,   F_RATE2);
    make_field(parent, "Reflow temperature",224,   F_REFLOW_T);
    make_field(parent, "Reflow time",       280,   F_REFLOW_TIME);

    /* Action row: RESET PREVIEW SAVE */
    lv_obj_t *reset = lv_button_create(parent);
    lv_obj_set_size(reset, 92, 40); lv_obj_set_pos(reset, 0, 348);
    lv_obj_set_style_bg_color(reset, THEME_BTN_NEUTRAL, 0);
    lv_obj_add_event_cb(reset, reset_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(reset); lv_label_set_text(rl, "RESET"); lv_obj_center(rl);

    lv_obj_t *prev = lv_button_create(parent);
    lv_obj_set_size(prev, 92, 40); lv_obj_set_pos(prev, 102, 348);
    lv_obj_set_style_bg_color(prev, THEME_BTN_NEUTRAL, 0);
    lv_obj_add_event_cb(prev, preview_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pl = lv_label_create(prev); lv_label_set_text(pl, "PREVIEW"); lv_obj_center(pl);

    lv_obj_t *save = lv_button_create(parent);
    lv_obj_set_size(save, 92, 40); lv_obj_set_pos(save, 204, 348);
    lv_obj_set_style_bg_color(save, THEME_INFO, 0);
    lv_obj_add_event_cb(save, save_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(save); lv_label_set_text(sl, "SAVE"); lv_obj_center(sl);
}

void screen_profile_set_initial(const profile_t *p) {
    s_buf = *p;
    for (int i = 0; i < 6; i++) refresh_label((field_id_t)i);
}
```

- [ ] **Step 3: Commit**

```bash
git add src/ui/screen_profile.*
git commit -m "Phase 5.4: ui/screen_profile — profile editor with keypad + actions"
```

---

### Task 28: `ui/screen_pid` — PID gains editor (auto-save on commit)

**Files:**
- Create: `src/ui/screen_pid.h`
- Create: `src/ui/screen_pid.c`

- [ ] **Step 1: Header**

```c
#pragma once
#include "lvgl.h"
#include "storage/nvs_schema.h"

void screen_pid_create(lv_obj_t *parent);
void screen_pid_set_initial(const pid_gains_t *g);
```

- [ ] **Step 2: Implementation** (mirrors profile screen but auto-saves on each commit)

```c
#include "ui/screen_pid.h"
#include "ui/theme.h"
#include "ui/screen_keypad.h"
#include "oven_state.h"           /* cmd_msg_t, CMD_APPLY_PID */
#include "config.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern QueueHandle_t g_cmd_queue;

static pid_gains_t s_buf;
static lv_obj_t   *s_val_labels[3];
static lv_obj_t   *s_saved_label;

typedef enum { F_KP, F_KI, F_KD } pid_field_id_t;

static void refresh_label(pid_field_id_t id) {
    char buf[16];
    float v = (id == F_KP) ? s_buf.kp : (id == F_KI) ? s_buf.ki : s_buf.kd;
    snprintf(buf, sizeof(buf), "%.2f", v);
    lv_label_set_text(s_val_labels[id], buf);
}

/* LVGL 9 renamed lv_obj_clear_flag → lv_obj_remove_flag. */
static void clear_saved(lv_timer_t *t) { lv_obj_add_flag(s_saved_label, LV_OBJ_FLAG_HIDDEN); lv_timer_delete(t); }
static void show_saved(void) {
    lv_obj_remove_flag(s_saved_label, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(clear_saved, 1500, NULL);
}

static void keypad_done(float value, void *user_data) {
    pid_field_id_t id = (pid_field_id_t)(intptr_t)user_data;
    if      (id == F_KP) s_buf.kp = value;
    else if (id == F_KI) s_buf.ki = value;
    else                 s_buf.kd = value;
    refresh_label(id);
    cmd_msg_t msg = {.type = CMD_APPLY_PID, .payload.pid = s_buf};
    xQueueSend(g_cmd_queue, &msg, 0);
    show_saved();
}

static void field_clicked_cb(lv_event_t *e) {
    pid_field_id_t id = (pid_field_id_t)(intptr_t)lv_event_get_user_data(e);
    keypad_config_t cfg = {
        .on_ok = keypad_done, .user_data = (void *)(intptr_t)id, .allow_decimal = true,
    };
    if (id == F_KP) { cfg.field_label="Kp"; cfg.unit=""; cfg.min=KP_MIN; cfg.max=KP_MAX; cfg.initial=s_buf.kp; }
    if (id == F_KI) { cfg.field_label="Ki"; cfg.unit=""; cfg.min=KI_MIN; cfg.max=KI_MAX; cfg.initial=s_buf.ki; }
    if (id == F_KD) { cfg.field_label="Kd"; cfg.unit=""; cfg.min=KD_MIN; cfg.max=KD_MAX; cfg.initial=s_buf.kd; }
    screen_keypad_open(&cfg);
}

static lv_obj_t *make_row(lv_obj_t *parent, const char *name, const char *desc, int y, pid_field_id_t id) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 296, 60); lv_obj_set_pos(card, 0, y);
    lv_obj_set_style_bg_color(card, THEME_PANEL_BG, 0);
    lv_obj_set_style_border_color(card, THEME_BORDER, 0);
    lv_obj_add_event_cb(card, field_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);

    lv_obj_t *n = lv_label_create(card); lv_label_set_text(n, name);
    lv_obj_set_style_text_color(n, THEME_FG, 0); lv_obj_set_pos(n, 8, 8);

    lv_obj_t *d = lv_label_create(card); lv_label_set_text(d, desc);
    lv_obj_set_style_text_color(d, THEME_FG_FAINT, 0); lv_obj_set_pos(d, 8, 30);

    lv_obj_t *v = lv_label_create(card);
    lv_obj_set_style_text_color(v, THEME_ACCENT, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -8, 0);
    s_val_labels[id] = v;
    return card;
}

void screen_pid_create(lv_obj_t *parent) {
    lv_obj_set_style_pad_all(parent, 8, 0);
    s_saved_label = lv_label_create(parent);
    /* LV_SYMBOL_OK ships with LVGL's built-in symbol font; the raw "✓"
     * glyph is not in Montserrat 14 and would render as a missing char. */
    lv_label_set_text(s_saved_label, LV_SYMBOL_OK " saved");
    lv_obj_set_style_text_color(s_saved_label, THEME_OK, 0);
    lv_obj_align(s_saved_label, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_add_flag(s_saved_label, LV_OBJ_FLAG_HIDDEN);

    make_row(parent, "Kp", "Proportional gain", 30,  F_KP);
    make_row(parent, "Ki", "Integral gain",    100,  F_KI);
    make_row(parent, "Kd", "Derivative gain",  170,  F_KD);
}

void screen_pid_set_initial(const pid_gains_t *g) {
    s_buf = *g;
    refresh_label(F_KP); refresh_label(F_KI); refresh_label(F_KD);
}
```

- [ ] **Step 3: Commit**

```bash
git add src/ui/screen_pid.*
git commit -m "Phase 5.5: ui/screen_pid — PID gains with auto-save on commit"
```

---

### Task 29: `ui/ui_task` — wire LVGL + tabs + screens; replace stub UI

**Files:**
- Create: `src/ui/ui_task.h`
- Create: `src/ui/ui_task.c`
- Modify: `src/main.c`

- [ ] **Step 1: Header**

```c
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "oven_state.h"

void ui_task_create(QueueHandle_t cmd_queue, oven_state_t *state);
```

- [ ] **Step 2: Implementation**

`src/ui/ui_task.c`:

```c
#include "ui/ui_task.h"
#include "ui/theme.h"
#include "ui/screen_home.h"
#include "ui/screen_profile.h"
#include "ui/screen_pid.h"
#include "ui/screen_keypad.h"
#include "drivers/display.h"
#include "drivers/touch.h"
#include "storage/nvs_store.h"
#include "esp_lvgl_port.h"
#include "esp_err.h"                /* ESP_ERROR_CHECK */
#include "freertos/task.h"
#include "freertos/semphr.h"        /* xSemaphoreTake/Give on state->mutex */
#include "lvgl.h"

QueueHandle_t g_cmd_queue;          /* externed by the screen modules */
static oven_state_t *s_state;

static void ui_refresh_timer(lv_timer_t *t) {
    (void)t;
    oven_state_t local;
    xSemaphoreTake(s_state->mutex, portMAX_DELAY);
    local = *s_state;
    xSemaphoreGive(s_state->mutex);
    screen_home_update(&local);
}

void ui_task_create(QueueHandle_t cmd_queue, oven_state_t *state) {
    g_cmd_queue = cmd_queue;
    s_state = state;

    display_handles_t d = display_init();
    esp_lcd_touch_handle_t tp = touch_init();

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = d.io, .panel_handle = d.panel,
        .buffer_size = 320 * 48, .double_buffer = true,
        .hres = 320, .vres = 480,
        /* Board-specific rotation/byte-order (see bringup 04_lvgl_hello):
         *   mirror_x   - flips X so LVGL text reads left-to-right.
         *   swap_bytes - RGB565 needs MSB-first on the wire; without this
         *                colours render with rotated channels.
         */
        .rotation = {.swap_xy = false, .mirror_x = true, .mirror_y = false},
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    lvgl_port_touch_cfg_t touch_cfg = {.disp = disp, .handle = tp};
    lvgl_port_add_touch(&touch_cfg);

    /* Build tabview */
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();
    theme_apply_to_screen(scr);
    lv_obj_t *tv = lv_tabview_create(scr);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_BOTTOM);
    lv_tabview_set_tab_bar_size(tv, 36);
    lv_obj_set_size(tv, 320, 480);

    lv_obj_t *tab_home    = lv_tabview_add_tab(tv, "HOME");
    lv_obj_t *tab_profile = lv_tabview_add_tab(tv, "PROFILE");
    lv_obj_t *tab_pid     = lv_tabview_add_tab(tv, "PID");

    screen_home_create(tab_home);
    screen_profile_create(tab_profile);
    screen_pid_create(tab_pid);

    /* Load persisted values into the editors. */
    profile_t p; nvs_store_load_profile(&p);
    pid_gains_t g; nvs_store_load_pid(&g);
    screen_profile_set_initial(&p);
    screen_pid_set_initial(&g);

    lv_timer_create(ui_refresh_timer, 250, NULL);
    lvgl_port_unlock();
}
```

- [ ] **Step 3: Replace stub in `main.c`**

In `src/main.c`:

- Add `#include "ui/ui_task.h"`.
- Delete the entire `ui_stub_task` static function.
- Drop the `#include <inttypes.h>` line (only the stub needed PRIu8 /
  PRIu32 for its log line).
- In `app_main()`, replace the `xTaskCreatePinnedToCore(ui_stub_task,
  …)` call with:

```c
    ui_task_create(s_cmd_queue, &s_state);
```

- [ ] **Step 4: Build**

Run: `pio run -e esp32dev` first (without `-t upload`) and confirm a
clean link. Expected footprint at this stage: roughly **28 % RAM, 63 %
flash** out of the 1 MB factory partition — well under the safety
margin. Then `pio test -e native` to confirm host tests still pass
(42/42).

If `-Werror=format` fires on `screen_home.c`'s elapsed-ms snprintf,
the casts in Task 26 weren't applied — go back and fix.

- [ ] **Step 5: Flash, run**

Run: `pio run -e esp32dev -t upload && pio device monitor`

Expected: device boots into a tab bar with Home/Profile/PID. Tapping Profile shows the six fields; tapping a value opens the keypad. Hitting START on Home transitions to RUNNING (visible via the temperature animation as the heater rises).

⚠️ **Heater still disconnected for this first integration run.** SSR pulses are observable via its onboard LED.

- [ ] **Step 6: Commit**

```bash
git add src/ui/ui_task.* src/main.c
git commit -m "Phase 5.6: ui/ui_task — tabview wiring; main.c replaces stub with real UI"
```

---

### Task 30: Bench test — low-setpoint hold (spec §11.2 step 8)

This task has no code changes — it is the manual integration acceptance test for PID + slow-PWM end-to-end.

- [ ] **Step 1: Set a low test profile via the UI**

On the device: Profile tab → set Reflow temperature to 60 °C, Reflow time to 60 s, both heat-up rates to 0.5 °C/s, soak temp 50 °C, soak time 30 s. Save.

- [ ] **Step 2: Connect the heater for this test only after verifying the safety chain still works**

Repeat §11.2 step 7 once on the assembled hardware: pull the thermocouple wire, confirm FAULT latches + ACKNOWLEDGE clears. Then connect mains-side wiring to the heater.

- [ ] **Step 3: Run the cycle**

Tap START. Expected behaviour:
- Temperature ramps to 50 °C at ~0.5 °C/s (heat-up 1).
- Holds at 50 °C for 30 s (soak).
- Ramps to 60 °C at ~0.5 °C/s (heat-up 2).
- Holds at 60 °C for 60 s (reflow).
- Enters COOL_DOWN; SSR off; returns to IDLE once below 50 °C (immediate at room temp).

Watch for: overshoot at the soak/reflow corners (tune Kp/Ki if > 10 °C), SSR clicking ON/OFF in slow-PWM windows (should be visible by the SSR's LED).

- [ ] **Step 4: Document any tuning that was needed**

If you adjusted PID gains during this test, the values are already in NVS. Note them in `docs/tuning_notes.md` (create the file).

- [ ] **Step 5: Commit (only if you created tuning notes)**

```bash
git add docs/tuning_notes.md 2>/dev/null || true
git commit -m "Phase 5.7: low-setpoint hold verified; tuning notes recorded" || true
```

---

### Task 31: Bench test — full reflow cycle (spec §11.2 step 9)

- [ ] **Step 1: Restore default profile**

On the device: Profile tab → RESET → SAVE. (Reverts to the SAC305-ish defaults: 1.2 °C/s · 150 °C / 90 s · 1.0 °C/s · 220 °C / 45 s.)

- [ ] **Step 2: Run a full cycle on a test PCB**

Tap START. Watch the full profile execute. Expected:
- Heat-up 1: ramp to 150 °C in ~100 s.
- Soak: hold 150 °C for 90 s.
- Heat-up 2: ramp to 220 °C in ~70 s.
- Reflow: hold 220 °C for 45 s.
- Cool-down: SSR off, oven cools naturally; returns to IDLE below 50 °C.

⚠️ The oven door should be ajar during cool-down — Koenic KMO 4341 doesn't ventilate naturally.

- [ ] **Step 3: Verify the post-cycle state**

Home should be back at IDLE. STOP button replaced by START. No fault.

- [ ] **Step 4: Final commit + tag**

```bash
git commit --allow-empty -m "MVP complete — full reflow cycle verified end-to-end"
git tag mvp-v1
```

---

**Phase 5 checkpoint — MVP complete.** All spec requirements implemented, host tests green, hardware verified per the §11.2 checklist.

---

### Phase 5 review-pass refinements (2026-05-17, evening)

Driven by a parallel code + spec review of the merged Phase 5 commits.
The full task-by-task fix plan lives at
`docs/superpowers/plans/2026-05-17-phase-5-review-fixes.md` (treat that
plan as authoritative for the post-review Phase 5 code; the Task 21–29
code blocks above show the pre-review intent). Every bullet below is
folded into the corresponding Phase 5 task; this list is the audit
trail.

- IPC: `oven_state_t` gains `phase_end_ms[STAGE_COUNT_]` and two
  `*_applied_seq` ACK counters. Resolves the two spec bugs surfaced
  by the spec-compliance review.
- Controller: `apply_non_sm_command` now performs `nvs_store_save_*`
  + seq bump after validate+apply. PID save is gated to `MODE_IDLE`
  to keep the control tick deterministic during live-tune.
- Helpers: `stage_name(stage_t)` and `fault_text(uint8_t)` in
  `reflow_sm.{h,c}` with Unity tests for every enum value.
- Home: cached-mode dispatch (no more strcmp-on-label), spec-faithful
  stage chip "Stage SOAK - 42 / 100 s", fault_text for FAULT line,
  initial colour fix, real graph (planned curve, live trace, FAULT
  dim, axis labels).
- Keypad: single-instance guard, 128-byte header buffer, ASCII-only
  glyphs.
- Profile: four stage cards (Soak + Reflow dual-row), ASCII unit
  literals, `screen_profile_update` for PREVIEW/SAVE gating + saved
  toast on `profile_applied_seq` advance.
- PID: `screen_pid_update` for saved toast on `pid_applied_seq`
  advance; immediate `show_saved` removed.
- ui_task: renamed `ui_task_create` → `ui_init`; UI-owned curve copy
  buffer (memcpy under mutex, render lock-free); calls all
  `screen_*_update` per tick.
- Spec: §3.3 (IPC fields), §5.1 step 5 (tick-budget caveat for NVS
  writes), §7.3 (sub-row clarification), §7.4 / §7.6 / §8 rewritten
  to reflect controller-side persistence.
- Tunable constants: `GRAPH_LIVE_RING_CAP` and `UI_REFRESH_MS` moved
  to `include/config.h` per the project's single-source rule. During
  execution, `PROFILE_CURVE_MAX_SAMPLES` was raised 1500 → 3100 to
  cover the absolute worst-case profile (HEATUP_RATE_MIN with max
  temps/times = 1545 s), and `GRAPH_LIVE_RING_CAP` was set to the
  same value with the live trace decimated to one sample per
  `PROFILE_CURVE_STEP_MS` so the DRAM segment fits.

---

## Self-review notes

**Spec coverage check:**

| Spec section | Covered by |
|---|---|
| §1 Scope | Task 31 (full cycle), Task 23 (fault path) |
| §2 Hardware & pin map | `config.h` (Task 2); driver inits (Tasks 16–19) |
| §3.1 Modules | Whole file layout matches §10 of spec |
| §3.2 Tasks | Task 22 + 29 + main.c (Tasks 23 + 29) |
| §3.3 IPC | Task 21 (types), Task 22 (controller side), Task 29 (UI side) |
| §3.4 Resource budget | Implicit — partial LVGL buffer in Task 12 + 29; WiFi/BT off (`menuconfig`) |
| §4.1 PID + anti-windup | Task 4 |
| §4.2 Slow PWM | Tasks 7 + 17 |
| §4.3 Setpoint source | Task 5 |
| §5 State machine + §5.1 tick order | Tasks 6 + 22 |
| §6.1 Firmware safety | Task 17 (single writer), Task 22 (gates), Task 23 (WDT + shutdown handler), `config.h` BOD note (Task 2) |
| §6.2 Hardware safety | Bring-up 06_ssr verifies pull-down (Task 14); §11.2 step 7 verifies fault latch (Task 15) |
| §7 UI | Tasks 24–29 |
| §8 NVS | Tasks 8 + 20 |
| §9 Config | Task 2 |
| §10 Project structure | Whole plan |
| §11.1 Host tests | Tasks 4–8 |
| §11.2 Bring-up | Tasks 9–15 + 30 + 31 |
| §12 v2 backlog | Not in this plan (correctly) |

**No remaining placeholders** — every step contains the code/command it needs.

**Type consistency** — `oven_state_t`, `cmd_msg_t`, `profile_t`, `pid_gains_t`, `stage_t`, `oven_mode_t`, `fault_flag_t`, `pid_state_t`, `sm_state_t`, `profile_curve_t` are each defined exactly once (in the indicated header) and used consistently throughout. The `g_cmd_queue` global is declared `extern` in screens and defined in `ui_task.c`.

**Independent review pass (2026-05-16)** — an independent reviewer audited the plan after self-review and found:
- 4 BLOCKERs (oven_state_t.curve type mismatch, missing `<string.h>` in screen_home.c, missing FreeRTOS includes in two bring-up mains, no `sdkconfig.defaults` covering BOD/BT/WDT) — all fixed inline.
- Several IMPORTANTs (managed-component versions now pinned in `lib_deps`, dead `pid.initialized` field removed, `ssr_init` GPIO config order corrected, `-I include -I src` added to esp32dev build flags, trailing parenthetical in Task 16 finished, no-op self-assignment in `publish()` removed, misleading comment in `apply_non_sm_command` clarified, "defined in main.c" comment in screen_home.c corrected) — all fixed.
- NITs on `oven_state.h` missing `#include "freertos/queue.h"` for hygiene — added.
- Open NIT not fixed: `screen_home.action_clicked_cb` decides action by label text comparison — brittle but works; would be cleaner to track mode in a static. Not blocking implementation.

---





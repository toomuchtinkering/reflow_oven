# Audio Signals Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add three event-driven audio signals (stage transitions, run completion, fault alarm) plus a Home-screen mute toggle, driving the onboard FM8002E amp from DAC2/IO26 and the AUDIO_EN line on IO4.

**Architecture:** A `drivers/audio.c` layer owns the DAC cosine-wave generator and the AUDIO_EN GPIO. A FreeRTOS `audio_task` consumes an `audio_event_t` queue and plays melody tables. `control_task` diffs successive `(mode, stage, fault_flags)` snapshots through a pure host-testable helper (`audio_detect_events`) and posts to the queue. UI exposes a single mute toggle persisted to NVS.

**Tech Stack:** ESP-IDF (`driver/dac_cosine.h`, `driver/gpio.h`), FreeRTOS (queues, tasks), LVGL 9 (icon button), Unity (host tests via `[env:native]`).

**Spec:** [docs/superpowers/specs/2026-05-23-audio-signals-design.md](../specs/2026-05-23-audio-signals-design.md)

---

## File map

**New:**
- `src/drivers/audio.h` — driver API (init, amp enable, tone, silence)
- `src/drivers/audio.c` — DAC + IO4 implementation (hardware-only)
- `src/audio/audio_events.h` — `audio_event_t`, `audio_state_snapshot_t`, `audio_detect_events`
- `src/audio/audio_events.c` — pure transition logic (host-testable)
- `src/audio/audio_melodies.h` — note table types + declarations
- `src/audio/audio_melodies.c` — note table definitions
- `src/audio/audio_task.h` — task API (start, set_muted, is_muted, post_event)
- `src/audio/audio_task.c` — task body, queue, atomic mute
- `test/test_audio_events/test_audio_events.c` — Unity tests for the helper
- `test/test_audio_melodies/test_audio_melodies.c` — Unity tests for melody tables
- `tools/bringup/09_audio/CMakeLists.txt`
- `tools/bringup/09_audio/platformio.ini`
- `tools/bringup/09_audio/src/CMakeLists.txt`
- `tools/bringup/09_audio/src/main.c`

**Modified:**
- `include/config.h` — pin defines, fault loop count, fault gap, NVS_KEY_MUTE
- `platformio.ini` — exclude `audio_task.c` and `drivers/audio.c` from the native build
- `src/storage/nvs_store.h` / `.c` — `nvs_store_get_mute` / `_set_mute`
- `src/main.c` — boot order: `audio_init()` then `audio_task_start()`
- `src/control/control_task.c` — keep `s_prev_audio_state`, post detected events
- `src/ui/screen_home.c` — speaker icon button + mute callback
- `docs/superpowers/specs/2026-05-15-reflow-oven-mvp-design.md` — remove "Buzzer feedback" from §1 deferred, back-reference this spec

---

### Task 1: Add config.h entries

**Files:**
- Modify: `include/config.h` (append a new section near the bottom)

- [ ] **Step 1: Add the audio block**

Append after the `DIAGNOSTICS` section in `include/config.h`:

```c
/* ============================================================
 * AUDIO — FM8002E onboard amplifier (spec 2026-05-23 §2)
 * ============================================================ */
/* IO26 = ESP32 DAC channel 1 (DAC_CHAN_1). Drives FM8002E +IN. */
#define PIN_AUDIO_DAC          26
/* Active-low FM8002E SHUTDOWN. Drive 0 to enable amp, 1 to shut down. */
#define PIN_AUDIO_EN            4

/* Fault alarm: 15 bursts × (~540 ms tone + 1500 ms gap) ≈ 30 s self-bounded. */
#define AUDIO_FAULT_LOOP_COUNT   15
#define AUDIO_FAULT_GAP_MS     1500

#define NVS_KEY_MUTE          "mute"
```

- [ ] **Step 2: Verify it builds for host (no regressions)**

```bash
pio test -e native --without-uploading --without-testing
```

Expected: build succeeds (we haven't added any source files yet — this is purely a header check).

- [ ] **Step 3: Commit**

```bash
git add include/config.h
git commit -m "config: add audio pins, fault-loop constants, NVS mute key"
```

---

### Task 2: `audio_events` — types and host test (failing)

**Files:**
- Create: `src/audio/audio_events.h`
- Create: `test/test_audio_events/test_audio_events.c`

- [ ] **Step 1: Create the header (types only, no impl yet)**

`src/audio/audio_events.h`:

```c
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
    oven_mode_t mode;
    stage_t     stage;
    uint8_t     fault_flags;
} audio_state_snapshot_t;

/* Diff prev → curr and write events into out[]. Returns the number written
 * (0..4). Rules:
 *   - curr.stage != prev.stage and curr.stage in {HEAT_UP_1..COOL_DOWN}
 *     → STAGE_TRANSITION
 *   - prev.mode==RUNNING and curr.mode==IDLE → RUN_COMPLETE
 *   - prev.fault_flags==0 and curr.fault_flags!=0 → FAULT
 *   - prev.mode==FAULT and curr.mode!=FAULT → FAULT_ACK
 * Priority: when FAULT is emitted this tick, STAGE_TRANSITION is suppressed.
 */
size_t audio_detect_events(audio_state_snapshot_t prev,
                           audio_state_snapshot_t curr,
                           audio_event_t out[4]);
```

- [ ] **Step 2: Write the failing Unity test**

`test/test_audio_events/test_audio_events.c`:

```c
#include <unity.h>
#include "audio/audio_events.h"

static audio_event_t out[4];

void setUp(void)    {}
void tearDown(void) {}

static audio_state_snapshot_t snap(oven_mode_t m, stage_t s, uint8_t f) {
    audio_state_snapshot_t r = { .mode = m, .stage = s, .fault_flags = f };
    return r;
}

void test_no_change_emits_nothing(void) {
    audio_state_snapshot_t s = snap(MODE_IDLE, STAGE_NONE, 0);
    TEST_ASSERT_EQUAL_size_t(0, audio_detect_events(s, s, out));
}

void test_start_emits_stage_transition(void) {
    audio_state_snapshot_t p = snap(MODE_IDLE,    STAGE_NONE,      0);
    audio_state_snapshot_t c = snap(MODE_RUNNING, STAGE_HEAT_UP_1, 0);
    TEST_ASSERT_EQUAL_size_t(1, audio_detect_events(p, c, out));
    TEST_ASSERT_EQUAL(AUDIO_EVT_STAGE_TRANSITION, out[0]);
}

void test_mid_run_stage_advance_emits_stage_transition(void) {
    audio_state_snapshot_t p = snap(MODE_RUNNING, STAGE_SOAK,      0);
    audio_state_snapshot_t c = snap(MODE_RUNNING, STAGE_HEAT_UP_2, 0);
    TEST_ASSERT_EQUAL_size_t(1, audio_detect_events(p, c, out));
    TEST_ASSERT_EQUAL(AUDIO_EVT_STAGE_TRANSITION, out[0]);
}

void test_cool_down_to_idle_emits_run_complete_not_stage(void) {
    audio_state_snapshot_t p = snap(MODE_RUNNING, STAGE_COOL_DOWN, 0);
    audio_state_snapshot_t c = snap(MODE_IDLE,    STAGE_NONE,      0);
    TEST_ASSERT_EQUAL_size_t(1, audio_detect_events(p, c, out));
    TEST_ASSERT_EQUAL(AUDIO_EVT_RUN_COMPLETE, out[0]);
}

void test_fault_rising_edge_emits_fault(void) {
    audio_state_snapshot_t p = snap(MODE_RUNNING, STAGE_SOAK,  0);
    audio_state_snapshot_t c = snap(MODE_FAULT,   STAGE_SOAK,  FAULT_TC_OPEN);
    TEST_ASSERT_EQUAL_size_t(1, audio_detect_events(p, c, out));
    TEST_ASSERT_EQUAL(AUDIO_EVT_FAULT, out[0]);
}

void test_fault_ack_emits_fault_ack(void) {
    audio_state_snapshot_t p = snap(MODE_FAULT, STAGE_SOAK, FAULT_TC_OPEN);
    audio_state_snapshot_t c = snap(MODE_IDLE,  STAGE_NONE, 0);
    TEST_ASSERT_EQUAL_size_t(1, audio_detect_events(p, c, out));
    TEST_ASSERT_EQUAL(AUDIO_EVT_FAULT_ACK, out[0]);
}

void test_simultaneous_fault_and_stage_change_suppresses_stage(void) {
    /* Over-temp trip exactly at SOAK→HEAT_UP_2 boundary. */
    audio_state_snapshot_t p = snap(MODE_RUNNING, STAGE_SOAK,      0);
    audio_state_snapshot_t c = snap(MODE_FAULT,   STAGE_HEAT_UP_2, FAULT_OVER_TEMP);
    size_t n = audio_detect_events(p, c, out);
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL(AUDIO_EVT_FAULT, out[0]);
}

void test_transition_into_stage_none_emits_nothing(void) {
    /* Defensive: STAGE_NONE is not in the trigger set. */
    audio_state_snapshot_t p = snap(MODE_RUNNING, STAGE_HEAT_UP_1, 0);
    audio_state_snapshot_t c = snap(MODE_RUNNING, STAGE_NONE,      0);
    TEST_ASSERT_EQUAL_size_t(0, audio_detect_events(p, c, out));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_no_change_emits_nothing);
    RUN_TEST(test_start_emits_stage_transition);
    RUN_TEST(test_mid_run_stage_advance_emits_stage_transition);
    RUN_TEST(test_cool_down_to_idle_emits_run_complete_not_stage);
    RUN_TEST(test_fault_rising_edge_emits_fault);
    RUN_TEST(test_fault_ack_emits_fault_ack);
    RUN_TEST(test_simultaneous_fault_and_stage_change_suppresses_stage);
    RUN_TEST(test_transition_into_stage_none_emits_nothing);
    return UNITY_END();
}
```

- [ ] **Step 3: Run test, confirm it fails to link**

```bash
pio test -e native -f test_audio_events
```

Expected: FAIL — `undefined reference to audio_detect_events`.

- [ ] **Step 4: Commit (red)**

```bash
git add src/audio/audio_events.h test/test_audio_events/test_audio_events.c
git commit -m "test: failing tests for audio_detect_events helper"
```

---

### Task 3: `audio_events` — implementation (passing)

**Files:**
- Create: `src/audio/audio_events.c`

- [ ] **Step 1: Implement the function**

`src/audio/audio_events.c`:

```c
#include "audio/audio_events.h"

static int stage_in_trigger_set(stage_t s) {
    return s >= STAGE_HEAT_UP_1 && s <= STAGE_COOL_DOWN;
}

size_t audio_detect_events(audio_state_snapshot_t prev,
                           audio_state_snapshot_t curr,
                           audio_event_t out[4]) {
    size_t n = 0;
    int fault_rising = (prev.fault_flags == 0) && (curr.fault_flags != 0);
    int fault_acked  = (prev.mode == MODE_FAULT) && (curr.mode != MODE_FAULT);
    int run_complete = (prev.mode == MODE_RUNNING) && (curr.mode == MODE_IDLE);
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
```

- [ ] **Step 2: Run tests, confirm they pass**

```bash
pio test -e native -f test_audio_events
```

Expected: PASS — 8 tests pass.

- [ ] **Step 3: Commit (green)**

```bash
git add src/audio/audio_events.c
git commit -m "feat: audio_detect_events transition helper"
```

---

### Task 4: `audio_melodies` — tables and host test

**Files:**
- Create: `src/audio/audio_melodies.h`
- Create: `test/test_audio_melodies/test_audio_melodies.c`

- [ ] **Step 1: Create the melody table header**

`src/audio/audio_melodies.h`:

```c
/* Compile-time melody tables. One {freq, on_ms, gap_ms} note per row.
 * Spec 2026-05-23 §3.5. */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "audio/audio_events.h"

typedef struct {
    uint16_t freq_hz;
    uint16_t on_ms;
    uint16_t gap_after_ms;
} audio_note_t;

typedef struct {
    const audio_note_t *notes;
    size_t              n;
} audio_melody_t;

/* Storage. Definitions in audio_melodies.c. */
extern const audio_note_t   STAGE_TRANSITION_NOTES[1];
extern const audio_note_t   RUN_COMPLETE_NOTES[3];
extern const audio_note_t   FAULT_BURST_NOTES[3];

extern const audio_melody_t MELODY_STAGE_TRANSITION;
extern const audio_melody_t MELODY_RUN_COMPLETE;
extern const audio_melody_t MELODY_FAULT_BURST;
```

- [ ] **Step 2: Create the definitions file**

`src/audio/audio_melodies.c`:

```c
#include "audio/audio_melodies.h"

const audio_note_t STAGE_TRANSITION_NOTES[1] = {
    {1200, 80, 0},
};

const audio_note_t RUN_COMPLETE_NOTES[3] = {
    { 523, 120, 40 },   /* C5 */
    { 659, 120, 40 },   /* E5 */
    { 784, 120,  0 },   /* G5 */
};

const audio_note_t FAULT_BURST_NOTES[3] = {
    { 880, 100, 80 },
    { 880, 100, 80 },
    { 880, 100,  0 },
};

const audio_melody_t MELODY_STAGE_TRANSITION = { STAGE_TRANSITION_NOTES, 1 };
const audio_melody_t MELODY_RUN_COMPLETE     = { RUN_COMPLETE_NOTES,     3 };
const audio_melody_t MELODY_FAULT_BURST      = { FAULT_BURST_NOTES,      3 };
```

- [ ] **Step 3: Write the failing test (will pass once both files compile)**

`test/test_audio_melodies/test_audio_melodies.c`:

```c
#include <unity.h>
#include "audio/audio_melodies.h"

void setUp(void)    {}
void tearDown(void) {}

static void check_melody_bounds(const audio_melody_t *m, uint32_t max_burst_ms) {
    TEST_ASSERT_GREATER_THAN(0, m->n);
    TEST_ASSERT_NOT_NULL(m->notes);
    uint32_t total = 0;
    for (size_t i = 0; i < m->n; i++) {
        const audio_note_t *n = &m->notes[i];
        TEST_ASSERT_GREATER_OR_EQUAL_UINT16(400,  n->freq_hz);
        TEST_ASSERT_LESS_OR_EQUAL_UINT16   (4000, n->freq_hz);
        TEST_ASSERT_GREATER_THAN(0, n->on_ms);
        total += n->on_ms + n->gap_after_ms;
    }
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(max_burst_ms, total);
}

void test_stage_transition_in_bounds(void) {
    check_melody_bounds(&MELODY_STAGE_TRANSITION, 200);
}
void test_run_complete_in_bounds(void) {
    check_melody_bounds(&MELODY_RUN_COMPLETE, 1000);
}
void test_fault_burst_in_bounds(void) {
    check_melody_bounds(&MELODY_FAULT_BURST, 1000);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_stage_transition_in_bounds);
    RUN_TEST(test_run_complete_in_bounds);
    RUN_TEST(test_fault_burst_in_bounds);
    return UNITY_END();
}
```

- [ ] **Step 4: Run tests, confirm they pass**

```bash
pio test -e native -f test_audio_melodies
```

Expected: PASS — 3 tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/audio/audio_melodies.h src/audio/audio_melodies.c \
        test/test_audio_melodies/test_audio_melodies.c
git commit -m "feat: audio melody tables for stage / completion / fault"
```

---

### Task 5: NVS mute storage

**Files:**
- Modify: `src/storage/nvs_store.h`
- Modify: `src/storage/nvs_store.c`

- [ ] **Step 1: Extend the header**

In `src/storage/nvs_store.h`, after `nvs_store_save_pid`, add:

```c
/* Mute flag. Single uint8_t under NVS_KEY_MUTE. */
bool nvs_store_get_mute(void);          /* false on missing key or any error */
void nvs_store_set_mute(bool muted);    /* fire-and-forget; logs on error */
```

The header currently does not include `<stdbool.h>` — add `#include <stdbool.h>` at the top.

- [ ] **Step 2: Implement get_mute / set_mute**

In `src/storage/nvs_store.c`, after `nvs_store_save_pid`, append:

```c
bool nvs_store_get_mute(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, NVS_KEY_MUTE, &v);
    nvs_close(h);
    return (err == ESP_OK) && (v != 0);
}

void nvs_store_set_mute(bool muted) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGW(TAG, "mute open err %d", err); return; }
    err = nvs_set_u8(h, NVS_KEY_MUTE, muted ? 1 : 0);
    if (err != ESP_OK) { ESP_LOGW(TAG, "mute set err %d", err); nvs_close(h); return; }
    err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "mute commit err %d", err);
    nvs_close(h);
}
```

- [ ] **Step 3: Build the firmware target to confirm it compiles**

```bash
pio run -e esp32dev
```

Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/storage/nvs_store.h src/storage/nvs_store.c
git commit -m "feat: NVS get/set helpers for mute flag"
```

---

### Task 6: `drivers/audio.{c,h}` — DAC + AUDIO_EN driver

**Files:**
- Create: `src/drivers/audio.h`
- Create: `src/drivers/audio.c`
- Modify: `platformio.ini` (exclude from native build)

- [ ] **Step 1: Driver header**

`src/drivers/audio.h`:

```c
/* Onboard FM8002E driver. DAC2 (IO26) → amp +IN, IO4 = active-low SHUTDOWN.
 * Spec 2026-05-23 §3.2. Only the audio_task is allowed to call play/silence.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void audio_init(void);                                   /* once at boot */
void audio_set_amp_enabled(bool on);                     /* drives IO4 */
void audio_play_tone(uint32_t freq_hz, uint32_t ms);     /* blocking */
void audio_silence(uint32_t ms);                         /* blocking */
```

- [ ] **Step 2: Driver implementation**

`src/drivers/audio.c`:

```c
#include "drivers/audio.h"
#include "config.h"
#include "driver/dac_cosine.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "audio";

/* Single live cosine channel handle. Allocated per tone, freed after. */
static dac_cosine_handle_t s_cw = NULL;

void audio_init(void) {
    /* AUDIO_EN: drive high immediately = amp in SHUTDOWN. The write-then-
     * configure sequence (matches src/drivers/ssr.c) avoids a 0→1 transient
     * on the output line. */
    gpio_set_level(PIN_AUDIO_EN, 1);
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_AUDIO_EN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_AUDIO_EN, 1);
    ESP_LOGI(TAG, "audio_init: amp shut down (IO%d high)", PIN_AUDIO_EN);
}

void audio_set_amp_enabled(bool on) {
    gpio_set_level(PIN_AUDIO_EN, on ? 0 : 1);
}

void audio_play_tone(uint32_t freq_hz, uint32_t ms) {
    /* The cosine generator only supports one live channel at a time per
     * frequency, so we allocate fresh for each note. Allocation is a few
     * register writes — cheap compared to a 80–120 ms note duration. */
    dac_cosine_config_t cfg = {
        .chan_id = DAC_CHAN_1,                      /* GPIO26 */
        .freq_hz = freq_hz,
        .clk_src = DAC_COSINE_CLK_SRC_DEFAULT,
        .atten   = DAC_COSINE_ATTEN_DEFAULT,
        .phase   = DAC_COSINE_PHASE_0,
        .offset  = 0,
        .flags.force_set_freq = true,
    };
    if (dac_cosine_new_channel(&cfg, &s_cw) != ESP_OK) {
        ESP_LOGW(TAG, "dac_cosine_new_channel failed at %u Hz", (unsigned)freq_hz);
        vTaskDelay(pdMS_TO_TICKS(ms));
        return;
    }
    dac_cosine_start(s_cw);
    vTaskDelay(pdMS_TO_TICKS(ms));
    dac_cosine_stop(s_cw);
    dac_cosine_del_channel(s_cw);
    s_cw = NULL;
}

void audio_silence(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}
```

- [ ] **Step 3: Exclude the driver from the native build**

In `platformio.ini`, replace the `[env:native]` `build_src_filter` block so its final state reads exactly:

```ini
build_src_filter =
    +<*>
    -<main.c>
    -<drivers/thermocouple.c>
    -<drivers/ssr.c>
    -<drivers/display.c>
    -<drivers/touch.c>
    -<drivers/audio.c>
    -<storage/nvs_store.c>
    -<control/control_task.c>
    -<ui/>
    -<audio/audio_task.c>
```

The two new exclusions are `drivers/audio.c` and `audio/audio_task.c`. `audio/audio_events.c` and `audio/audio_melodies.c` stay included — both are pure C with no IDF dependency.

- [ ] **Step 4: Build firmware to confirm driver compiles**

```bash
pio run -e esp32dev
```

Expected: build succeeds.

- [ ] **Step 5: Run host tests to confirm native filter still works**

```bash
pio test -e native
```

Expected: all existing host tests still pass (no audio_task / drivers/audio refs leaked into native).

- [ ] **Step 6: Commit**

```bash
git add src/drivers/audio.h src/drivers/audio.c platformio.ini
git commit -m "feat: drivers/audio — DAC cosine generator + AUDIO_EN gating"
```

---

### Task 7: Bring-up project `tools/bringup/09_audio/`

**Purpose:** isolated firmware that initialises *only* the audio driver and accepts single-char UART commands so the developer can verify hardware by ear before wiring into the main app. Spec §7.2.

**Files:**
- Create: `tools/bringup/09_audio/CMakeLists.txt`
- Create: `tools/bringup/09_audio/platformio.ini`
- Create: `tools/bringup/09_audio/src/CMakeLists.txt`
- Create: `tools/bringup/09_audio/src/main.c`

- [ ] **Step 1: Project root CMakeLists**

`tools/bringup/09_audio/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(09_audio)
```

- [ ] **Step 2: platformio.ini**

`tools/bringup/09_audio/platformio.ini`:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = espidf
monitor_speed = 115200
build_flags =
    -I../../../include
    -I../../../src
```

- [ ] **Step 3: src/CMakeLists pulls in shared driver**

`tools/bringup/09_audio/src/CMakeLists.txt`:

```cmake
# Local main.c + the shared audio driver (kept in sync with the main firmware
# build, same file/two targets).
FILE(GLOB_RECURSE local_sources ${CMAKE_SOURCE_DIR}/src/*.c)
set(shared_sources
    "${CMAKE_SOURCE_DIR}/../../../src/drivers/audio.c"
    "${CMAKE_SOURCE_DIR}/../../../src/audio/audio_melodies.c"
)

idf_component_register(
    SRCS ${local_sources} ${shared_sources}
    INCLUDE_DIRS
        "."
        "${CMAKE_SOURCE_DIR}/../../../src"
        "${CMAKE_SOURCE_DIR}/../../../include"
)
```

- [ ] **Step 4: Bring-up main.c with UART CLI**

`tools/bringup/09_audio/src/main.c`:

```c
/* tools/bringup/09_audio/src/main.c
 * UART CLI for audio driver bring-up.
 *
 * Commands (single-char, no <enter> needed):
 *   s = stage-transition pip (80 ms @ 1200 Hz)
 *   c = run-complete chime  (rising C5–E5–G5)
 *   f = single fault burst  (3× 880 Hz, ~540 ms)
 *   F = full 15-burst alarm (~30 s)
 *   t = ad-hoc tone at 1 kHz for 500 ms
 *   ? = print help
 *
 * Bring-up checklist (spec §7.2):
 *   - Tones audible without distortion.
 *   - No click on amp enable/disable transitions.
 *   - No idle hiss when AUDIO_EN is high (amp shut down).
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include <stdio.h>
#include "drivers/audio.h"
#include "audio/audio_melodies.h"
#include "config.h"

static const char *TAG = "09_audio";

static void play_melody(const audio_melody_t *m) {
    audio_set_amp_enabled(true);
    for (size_t i = 0; i < m->n; i++) {
        audio_play_tone(m->notes[i].freq_hz, m->notes[i].on_ms);
        if (m->notes[i].gap_after_ms) audio_silence(m->notes[i].gap_after_ms);
    }
    audio_set_amp_enabled(false);
}

static void play_help(void) {
    printf("\nCommands:\n"
           "  s = stage pip\n"
           "  c = completion chime\n"
           "  f = one fault burst\n"
           "  F = full fault alarm (~30 s)\n"
           "  t = ad-hoc 1 kHz / 500 ms tone\n"
           "  ? = this help\n\n");
}

void app_main(void) {
    ESP_LOGI(TAG, "audio bring-up starting");
    audio_init();
    play_help();

    while (1) {
        int c = getchar();
        if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        switch (c) {
            case 's': ESP_LOGI(TAG, "stage pip");
                      play_melody(&MELODY_STAGE_TRANSITION); break;
            case 'c': ESP_LOGI(TAG, "completion chime");
                      play_melody(&MELODY_RUN_COMPLETE);     break;
            case 'f': ESP_LOGI(TAG, "one fault burst");
                      play_melody(&MELODY_FAULT_BURST);      break;
            case 'F': ESP_LOGI(TAG, "full fault alarm");
                      for (int i = 0; i < AUDIO_FAULT_LOOP_COUNT; i++) {
                          play_melody(&MELODY_FAULT_BURST);
                          audio_silence(AUDIO_FAULT_GAP_MS);
                      }
                      break;
            case 't': ESP_LOGI(TAG, "ad-hoc 1 kHz tone");
                      audio_set_amp_enabled(true);
                      audio_play_tone(1000, 500);
                      audio_set_amp_enabled(false);
                      break;
            case '?': case '\r': case '\n': play_help(); break;
            default: break;
        }
    }
}
```

- [ ] **Step 5: Build the bring-up**

```bash
cd tools/bringup/09_audio
pio run -e esp32dev
cd ../../..
```

Expected: build succeeds.

- [ ] **Step 6: Flash and hand-verify (operator step)**

```bash
cd tools/bringup/09_audio
pio run -e esp32dev -t upload && pio device monitor
```

In the serial monitor: press `s`, `c`, `f`, `F`, `t`, `?`. Confirm by ear that each plays correctly, no clicks on amp enable/disable, no idle hiss when the amp is shut down.

If the speaker is reversed in polarity (audible "thump" with both phases inverted), swap the SP+/SP- wires — see spec §8 Risk 3.

- [ ] **Step 7: Commit**

```bash
git add tools/bringup/09_audio/
git commit -m "bringup: 09_audio — UART CLI for FM8002E driver verification"
```

---

### Task 8: `audio_task` — task body, queue, mute state

**Files:**
- Create: `src/audio/audio_task.h`
- Create: `src/audio/audio_task.c`

- [ ] **Step 1: Header**

`src/audio/audio_task.h`:

```c
/* Audio playback task. Consumes audio_event_t from g_audio_queue and
 * plays the corresponding melody table. Spec 2026-05-23 §3.3. */
#pragma once

#include <stdbool.h>
#include "audio/audio_events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

extern QueueHandle_t g_audio_queue;       /* depth 4, audio_event_t */

void audio_task_start(void);              /* call once after audio_init */

/* Mute control. set_muted persists to NVS as a side effect.
 * is_muted is safe to call from any task. */
void audio_set_muted(bool muted);
bool audio_is_muted(void);

/* Convenience posting helper. Drop-on-full (timeout=0). Returns true on enqueue. */
bool audio_post_event(audio_event_t evt);
```

- [ ] **Step 2: Implementation**

`src/audio/audio_task.c`:

```c
#include "audio/audio_task.h"
#include "audio/audio_melodies.h"
#include "drivers/audio.h"
#include "storage/nvs_store.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <stdatomic.h>

static const char *TAG = "audio_task";

QueueHandle_t g_audio_queue = NULL;
static atomic_bool s_muted = false;

static void play_melody(const audio_melody_t *m) {
    audio_set_amp_enabled(true);
    for (size_t i = 0; i < m->n; i++) {
        audio_play_tone(m->notes[i].freq_hz, m->notes[i].on_ms);
        if (m->notes[i].gap_after_ms) audio_silence(m->notes[i].gap_after_ms);
    }
    audio_set_amp_enabled(false);
}

/* Returns true if FAULT_ACK arrived mid-loop and we should bail out. */
static bool fault_gap_with_peek(uint32_t ms) {
    /* vTaskDelay tick granularity is 10 ms with the default config; peek
     * every 50 ms so an ACK is responded to within ~50 ms. */
    const uint32_t slice = 50;
    for (uint32_t waited = 0; waited < ms; waited += slice) {
        audio_event_t peek_evt;
        if (xQueuePeek(g_audio_queue, &peek_evt, 0) == pdTRUE &&
            peek_evt == AUDIO_EVT_FAULT_ACK) {
            /* Drain the ACK so the next xQueueReceive doesn't redo it. */
            xQueueReceive(g_audio_queue, &peek_evt, 0);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(slice));
    }
    return false;
}

static void play_fault_alarm(void) {
    for (int i = 0; i < AUDIO_FAULT_LOOP_COUNT; i++) {
        play_melody(&MELODY_FAULT_BURST);
        if (i == AUDIO_FAULT_LOOP_COUNT - 1) break;
        if (fault_gap_with_peek(AUDIO_FAULT_GAP_MS)) {
            ESP_LOGI(TAG, "fault ack received, stopping alarm at burst %d", i + 1);
            return;
        }
    }
    ESP_LOGI(TAG, "fault alarm self-stopped after %d bursts", AUDIO_FAULT_LOOP_COUNT);
}

static void audio_task(void *arg) {
    (void)arg;
    audio_event_t evt;
    for (;;) {
        if (xQueueReceive(g_audio_queue, &evt, portMAX_DELAY) != pdTRUE) continue;
        if (atomic_load(&s_muted)) continue;
        switch (evt) {
            case AUDIO_EVT_STAGE_TRANSITION: play_melody(&MELODY_STAGE_TRANSITION); break;
            case AUDIO_EVT_RUN_COMPLETE:     play_melody(&MELODY_RUN_COMPLETE);     break;
            case AUDIO_EVT_FAULT:            play_fault_alarm();                    break;
            case AUDIO_EVT_FAULT_ACK:        /* no-op when idle; consumed inside the loop */ break;
        }
    }
}

void audio_task_start(void) {
    g_audio_queue = xQueueCreate(4, sizeof(audio_event_t));
    atomic_store(&s_muted, nvs_store_get_mute());
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 1, NULL, 1);
    ESP_LOGI(TAG, "audio_task started, muted=%d", (int)atomic_load(&s_muted));
}

void audio_set_muted(bool muted) {
    atomic_store(&s_muted, muted);
    nvs_store_set_mute(muted);
}

bool audio_is_muted(void) {
    return atomic_load(&s_muted);
}

bool audio_post_event(audio_event_t evt) {
    if (!g_audio_queue) return false;
    return xQueueSend(g_audio_queue, &evt, 0) == pdTRUE;
}
```

- [ ] **Step 3: Build firmware**

```bash
pio run -e esp32dev
```

Expected: build succeeds. The task is not yet started anywhere — that's Task 10.

- [ ] **Step 4: Commit**

```bash
git add src/audio/audio_task.h src/audio/audio_task.c
git commit -m "feat: audio_task — queue consumer + mute state + fault alarm loop"
```

---

### Task 9: `control_task` hook — post detected events

**Files:**
- Modify: `src/control/control_task.c`

- [ ] **Step 1: Add the include and file-static snapshot**

In `src/control/control_task.c`, after the existing `#include` block, add:

```c
#include "audio/audio_events.h"
#include "audio/audio_task.h"
```

Below the other file-statics (near line 22, after `static int64_t s_last_tc_ok_us;`), add:

```c
static audio_state_snapshot_t s_prev_audio = {
    .mode = MODE_IDLE, .stage = STAGE_NONE, .fault_flags = 0,
};
```

- [ ] **Step 2: Add a `publish_and_detect` helper**

The existing loop has three early-exit safety gates that each call `publish(...); goto feed_wdt;`. We need the rising-edge `FAULT` event to fire on the *same* tick the gate trips — so audio detection must run as part of those gates, not after them. Extract publish + detection into a helper.

In `src/control/control_task.c`, immediately after the existing `publish(...)` function definition (before `swap_curves`), add:

```c
static void publish_and_detect(uint8_t duty, float setpoint, const float *temp_c) {
    publish(duty, setpoint, temp_c);

    /* Detect & post audio events. Spec 2026-05-23 §3.4.
     * Drop-on-full (audio_post_event uses timeout 0): audio is non-critical. */
    audio_state_snapshot_t curr = {
        .mode = s_sm.mode, .stage = s_sm.stage, .fault_flags = s_sm.fault_flags,
    };
    audio_event_t evts[4];
    size_t n_evt = audio_detect_events(s_prev_audio, curr, evts);
    for (size_t i = 0; i < n_evt; i++) audio_post_event(evts[i]);
    s_prev_audio = curr;
}
```

- [ ] **Step 3: Replace every `publish(...)` call in the task loop with `publish_and_detect(...)`**

There are four `publish(...)` call sites inside `control_task`:

1. Inside the `if (fault) { ... }` block: `publish(0, 0.0f, temp_ptr);`
2. Inside the `if (age_ms > SENSOR_MAX_AGE_MS) { ... }` block: `publish(0, 0.0f, temp_ptr);`
3. Inside the `if (temp_c > OVER_TEMP_LIMIT_C) { ... }` block: `publish(0, 0.0f, temp_ptr);`
4. The final success-path publish at the bottom: `publish(duty, setpoint, &temp_c);`

Change each `publish(` to `publish_and_detect(` — arguments are identical. Leave the original `publish` function in place (it's still called *inside* `publish_and_detect`).

- [ ] **Step 4: Verify the build**

```bash
pio run -e esp32dev
```

Expected: build succeeds.

- [ ] **Step 5: Commit**

```bash
git add src/control/control_task.c
git commit -m "feat: control_task posts audio events on every snapshot diff"
```

---

### Task 10: Boot integration in `main.c`

**Files:**
- Modify: `src/main.c`

- [ ] **Step 1: Add includes**

After the existing includes in `src/main.c`, add:

```c
#include "drivers/audio.h"
#include "audio/audio_task.h"
```

- [ ] **Step 2: Insert init + task start**

Per spec §6, the order is: `audio_init()` very early (before LVGL), then `nvs_store_init()` (already present), then `audio_task_start()` after the IPC objects exist.

Modify the body of `app_main` so the relevant block reads:

```c
    ESP_LOGI(TAG, "reflow_oven boot");

    /* WDT (unchanged) */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = CONTROL_TASK_WDT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&wdt_cfg));

    /* Audio first — drives IO4 high (amp off) before anything else can
     * race the pin. DAC is allocated per-tone by the audio task later. */
    audio_init();

    /* Drivers */
    nvs_store_init();
    thermocouple_init();
    ssr_init();
    esp_register_shutdown_handler(shutdown_handler);

    /* IPC */
    s_cmd_queue = xQueueCreate(8, sizeof(cmd_msg_t));
    s_state.mutex = xSemaphoreCreateMutex();

    /* Tasks */
    audio_task_start();                 /* before control_task — control posts to its queue */
    control_task_create(s_cmd_queue, &s_state);
    ui_init(s_cmd_queue, &s_state);
```

- [ ] **Step 3: Build**

```bash
pio run -e esp32dev
```

Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/main.c
git commit -m "feat: wire audio_init + audio_task_start into app_main"
```

---

### Task 11: Speaker icon button on Home screen

**Files:**
- Modify: `src/ui/screen_home.c`

- [ ] **Step 1: Add includes and a file-static handle**

Near the top of `src/ui/screen_home.c`, after the existing `#include` block, add:

```c
#include "audio/audio_task.h"
#include "audio/audio_events.h"
```

In the file-static declarations area (around line 14, with `lbl_temp` etc.), add:

```c
static lv_obj_t *btn_mute;
```

- [ ] **Step 2: Add the icon update + click handler**

Above `screen_home_create`, add:

```c
static void mute_redraw(void) {
    lv_obj_t *lbl = lv_obj_get_child(btn_mute, 0);
    lv_label_set_text(lbl, audio_is_muted() ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
}

static void mute_clicked_cb(lv_event_t *e) {
    (void)e;
    bool new_state = !audio_is_muted();
    audio_set_muted(new_state);     /* persists to NVS internally */
    mute_redraw();
    if (!new_state) {
        /* Unmute confirmation: reuse the 80 ms stage pip. */
        audio_post_event(AUDIO_EVT_STAGE_TRANSITION);
    }
}
```

- [ ] **Step 3: Create the button inside `screen_home_create`**

Inside `screen_home_create`, after the action button block (around line 250, just before the closing `}`), add:

```c
    /* Mute toggle — top-right corner. 28 × 28 px tap target. */
    btn_mute = lv_button_create(parent);
    lv_obj_set_size(btn_mute, 28, 28);
    lv_obj_set_pos(btn_mute, 280, 0);
    lv_obj_set_style_bg_opa(btn_mute, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(btn_mute, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(btn_mute, 0, 0);
    lv_obj_add_event_cb(btn_mute, mute_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_mute = lv_label_create(btn_mute);
    lv_obj_set_style_text_color(lbl_mute, THEME_FG, 0);
    lv_obj_center(lbl_mute);
    /* Initial glyph picked from NVS-loaded state by mute_redraw(). */
    mute_redraw();
```

- [ ] **Step 4: Build**

```bash
pio run -e esp32dev
```

Expected: build succeeds.

- [ ] **Step 5: Commit**

```bash
git add src/ui/screen_home.c
git commit -m "feat: speaker icon mute toggle on Home screen"
```

---

### Task 12: Back-reference the MVP spec

**Files:**
- Modify: `docs/superpowers/specs/2026-05-15-reflow-oven-mvp-design.md`

- [ ] **Step 1: Edit the deferred list in §1**

Find the bullet `- Buzzer feedback at stage transitions / completion` in §1's "Out of scope for MVP (deferred to v2)" list. Replace with a note that it's been promoted:

Old text:
```markdown
- Buzzer feedback at stage transitions / completion
```

New text:
```markdown
- ~~Buzzer feedback at stage transitions / completion~~ — implemented 2026-05-23, see [audio-signals design](2026-05-23-audio-signals-design.md).
```

- [ ] **Step 2: Commit**

```bash
git add docs/superpowers/specs/2026-05-15-reflow-oven-mvp-design.md
git commit -m "spec: cross-reference audio-signals from MVP spec deferred list"
```

---

### Task 13: In-system validation (manual)

This task is **not committed code** — it is the operator-facing validation checklist from spec §7.2. Walk through it after Tasks 1–12 are merged.

- [ ] **Step 1: Flash full firmware and reboot**

```bash
pio run -e esp32dev -t upload
pio device monitor
```

- [ ] **Step 2: Confirm boot is silent**

No tones during the ~100–300 ms boot window. If pops are audible, see spec §8 Risk 1: add an external pull-up from IO4 to +5 V.

- [ ] **Step 3: Confirm START pip + stage advances**

Tap START on the Home screen → expect one 80 ms pip immediately. Watch the run advance through SOAK / HEAT_UP_2 / REFLOW / COOL_DOWN — one pip per transition. At cool-down completion (temp < 50 °C or 10 min timeout), expect the 3-note rising chime.

- [ ] **Step 4: Confirm fault alarm self-stops**

Spoof a thermocouple over-temp by physically heating the bead above 260 °C, or temporarily edit `OVER_TEMP_LIMIT_C` to a low value, reflash, and start a run. Expect: alarm beeps in bursts for ~30 s, then stops on its own. Banner stays.

- [ ] **Step 5: Confirm fault ack stops the alarm immediately**

Repeat Step 4 but tap the action button during the alarm (which says "OK" in MODE_FAULT — `CMD_ACK_FAULT`). Expect: the next inter-burst gap is interrupted and the alarm stops within ~50 ms.

- [ ] **Step 6: Confirm mute persistence**

Tap the speaker icon — confirm icon swaps to `LV_SYMBOL_MUTE`, no pip plays. Power-cycle the board. Confirm icon still shows muted after reboot. Tap to unmute — expect the 80 ms unmute confirmation pip and the icon to switch back. Power-cycle again — confirm unmuted state persists.

- [ ] **Step 7: Update the memory note (optional but recommended)**

If everything passes, mention in conversation so the long-running memory `v2_backlog.md` can have the "Buzzer feedback" bullet struck through with the date — exact action handled by the assistant via auto-memory.

---

## Sanity checklist

- [ ] All host tests pass: `pio test -e native`
- [ ] Firmware builds clean: `pio run -e esp32dev`
- [ ] Bring-up `09_audio` builds clean: `cd tools/bringup/09_audio && pio run -e esp32dev`
- [ ] Bring-up manual checks pass (Task 7 Step 6)
- [ ] In-system manual checks pass (Task 13)
- [ ] Spec back-reference committed (Task 12)

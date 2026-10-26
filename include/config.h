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

/* Onboard RGB status LED. Active LOW: drive 0 to light, 1 to extinguish. */
#define PIN_LED_R          22
#define PIN_LED_G          16
#define PIN_LED_B          17

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
#define PROFILE_CURVE_STEP_MS  1000   /* range 250-1000; curve memory grows inversely. 1 s keeps the graph/curve arrays small (see PROFILE_CURVE_MAX_SAMPLES). */
#define SSR_QUANTA_PER_WINDOW  (SSR_PWM_WINDOW_MS / CONTROL_TICK_MS)  /* = 8 */
/* Drying uses a much longer PWM window. At the reflow window (8 quanta) the
 * scheduler's round-to-nearest drops any duty <= 6 % to zero quanta, but a
 * 50 C hold needs ~4.75 %. 120 quanta = a 30 s window = 0.83 % resolution.
 * Slow switching also reduces zero-cross SSR wear. See drying spec 2.1. */
#define DRY_SSR_QUANTA           120
_Static_assert(DRY_SSR_QUANTA >= 1, "SSR window needs at least one quantum");
/* Median window over successive TC reads (one per control tick). 3 rejects
 * any single-sample spike (SSR switching noise coupled into the probe) at
 * the cost of one tick of lag; the MAX31855 converts every ~100 ms, so each
 * 250 ms tick sees a fresh sample. Fixed at 3 — tc_filter.c asserts it. */
#define TC_FILTER_WINDOW_N        3

/* ============================================================
 * SAFETY LIMITS
 * ============================================================ */
#define OVER_TEMP_LIMIT_C        260.0f   /* range 230-280; must be > REFLOW_TEMP_MAX */
#define SENSOR_MAX_AGE_MS        1500     /* > CONTROL_TICK_MS, < 5000 */
/* Temperature at which a run leaves COOL_DOWN and completes (RUN_COMPLETE
 * pip + return to IDLE). Set to 80 °C so the board can be inspected / removed
 * and the archive analysed without a long wait for near-ambient; the planned
 * curve's cool-down tail also ends here. range 30-80. */
#define COOL_DOWN_THRESHOLD_C     80.0f
#define COOL_DOWN_TIMEOUT_MS    600000    /* 10 min — release UI from COOL_DOWN even if stuck hot */
/* A reflow run is refused above this temperature so every run starts from a
 * comparable near-ambient state (reproducible soak preheat) and the ff hold
 * model isn't seeded from a hot oven. Must stay below COOL_DOWN_THRESHOLD_C so
 * a just-completed run must actually cool further before it can be restarted. */
#define RUN_START_MAX_TEMP_C      50.0f
_Static_assert(RUN_START_MAX_TEMP_C < COOL_DOWN_THRESHOLD_C,
               "RUN_START_MAX_TEMP_C must be below COOL_DOWN_THRESHOLD_C");
#define CONTROL_TASK_WDT_S            2   /* §6.1 item 5 */

/* UI liveness supervision. The UI runs inside esp_lvgl_port's own task, which
 * the component never subscribes to the task watchdog and offers no hook to
 * feed it from (esp_lvgl_port 2.8.0 has no WDT support at all). A wedged UI is
 * therefore invisible to the WDT: control_task keeps feeding it and the HTTP
 * server keeps answering while the panel is dead — including its STOP button,
 * while the SSR carries on switching. control_task watches the UI heartbeat
 * instead and reboots when it stops; esp_restart runs the registered shutdown
 * handler, which drives the SSR low, so a reset is the fail-safe response.
 *
 * 10 s is 40 missed UI_REFRESH_MS ticks — far beyond any legitimate stall (OTA
 * flash bursts, a WiFi scan, a briefly held LVGL lock) yet short enough that
 * STOP is never dead for long. Deliberately NOT derived from
 * CONTROL_TASK_WDT_S: that 2 s bound is a safety property of the control loop
 * (§6.2 overshoot estimate) and must not be relaxed to accommodate the UI. */
#define UI_STALL_TIMEOUT_MS       10000u

/* ============================================================
 * FILAMENT DRYING (spec 2026-08-30)
 * ============================================================ */
#define DRY_TEMP_MIN_C            40.0f
#define DRY_TEMP_MAX_C           100.0f
/* Fault above target + this. 10 C keeps PLA at 45 C tripping at 55 C, below
 * its ~60 C glass transition, so a welded SSR does not deform the spool. */
#define DRY_OVER_TEMP_MARGIN_C    10.0f
/* Refuse START above target + this. Must stay below the trip margin or a
 * start accepted at the gate would immediately fault. */
#define DRY_START_MARGIN_C         5.0f
/* HEAT_UP -> HOLD entry band. */
#define DRY_HOLD_BAND_C            2.0f
/* Overshoot after a ramp flattens is approximately rate * L, with L = 32.5 s
 * from the 2026-07-19 step test. 0.05 C/s gives ~1.6 C, well inside the 10 C
 * trip. Worst-case ramp 25->100 C is 25 min, negligible against a 6-12 h hold. */
#define DRY_RAMP_RATE_C_S          0.05f
#define DRY_DUTY_MAX_PERCENT      50.0f
/* Backstop for a target the oven cannot reach (failed element, open door,
 * over-large load): without it a run that never enters the band would heat
 * indefinitely, since the hold timer never starts. Longest legitimate ramp is
 * 25 min. Same role as COOL_DOWN_TIMEOUT_MS. */
#define DRY_HEATUP_TIMEOUT_MS  5400000u          /* 90 min */
#define DRY_HOLD_MIN_MS          60000u          /* 1 min  */
#define DRY_DURATION_MAX_MS   57600000u          /* 16 h   */
#define DRY_PRESET_COUNT             6

/* HOME graph while drying. A 6-16 h hold plotted whole is a flat line
 * carrying no information, so the X axis becomes a rolling window ending at
 * "now" — that is where the ripple and the stability actually show. The
 * cadence is chosen so one full window fits the EXISTING live-trace ring with
 * headroom; adding a second graph screen was not an option (screen_home.c
 * already holds 25.8 KiB of point buffers and every LVGL screen permanently
 * consumes the 64 KB pool). */
#define DRY_GRAPH_WINDOW_MS  1800000u   /* 30 min */
#define DRY_GRAPH_SAMPLE_MS     2000u   /* -> 900 samples per window */
#define DRY_GRAPH_SAMPLES    (DRY_GRAPH_WINDOW_MS / DRY_GRAPH_SAMPLE_MS)
/* Headroom above the target for the drying Y axis, so the trace is not drawn
 * hard against the top rail and a modest overshoot stays visible. */
#define DRY_GRAPH_HEADROOM_C     20.0f

/* Electronics (MAX31855 cold junction) limit, GLOBAL to every mode. The oven
 * vents through the electronics bay; the 2026-07-21 hold staircase reached
 * 60 C after 40 min and was still climbing linearly, and no archived run ever
 * reached equilibrium. ESP32-WROOM-32E is rated to +85 C ambient. */
#define CJ_LIMIT_C                75.0f

_Static_assert(DRY_TEMP_MAX_C + DRY_OVER_TEMP_MARGIN_C <= OVER_TEMP_LIMIT_C,
               "drying trip point must stay below the absolute over-temp limit");
_Static_assert(DRY_TEMP_MIN_C < DRY_TEMP_MAX_C, "drying range inverted");
_Static_assert(DRY_START_MARGIN_C < DRY_OVER_TEMP_MARGIN_C,
               "a start accepted at the gate must not instantly over-temp");
_Static_assert(DRY_HOLD_MIN_MS < DRY_DURATION_MAX_MS, "drying duration range inverted");
_Static_assert(DRY_RAMP_RATE_C_S > 0.0f, "drying ramp must ascend");
_Static_assert(CJ_LIMIT_C < 85.0f,
               "electronics limit must stay below the ESP32-WROOM-32E rated ambient");
_Static_assert(DRY_GRAPH_WINDOW_MS % DRY_GRAPH_SAMPLE_MS == 0,
               "drying graph window must be a whole number of samples");
_Static_assert(DRY_GRAPH_SAMPLES >= 2, "a line needs at least two points");
_Static_assert(DRY_TEMP_MAX_C + DRY_GRAPH_HEADROOM_C <= OVER_TEMP_LIMIT_C,
               "drying graph must not scale past the reflow graph's top rail");

/* Compiled preset defaults. Names are compile-time; only the values are
 * editable and persisted (an editable-name UI needs a text keyboard, the most
 * expensive widget LVGL offers, permanently in the 64 KB pool). */
#define DRY_PRESET_DEFAULTS { \
    { "PLA",   45,  360 }, \
    { "PETG",  65,  360 }, \
    { "TPU",   70,  480 }, \
    { "ASA",   60,  360 }, \
    { "Nylon", 90,  480 }, \
    { "PC",    90,  360 }, \
}

/* ============================================================
 * STEP TEST (open-loop PID identification) — spec 2026-05-31
 * ============================================================ */
#define STEP_TEST_DUTY_MIN          10
#define STEP_TEST_DUTY_MAX          80
#define STEP_TEST_DUTY_DEFAULT      50       /* TUNE */
#define STEP_TEST_CEILING_MIN_C     80
#define STEP_TEST_CEILING_MAX_C    200       /* stays >=60 C below OVER_TEMP_LIMIT_C */
#define STEP_TEST_CEILING_DEFAULT_C 180      /* TUNE */
#define STEP_TEST_MAX_DURATION_MS  600000    /* 10 min backstop; TUNE */

/* Telemetry HTTP API */
#define TELEMETRY_API_VERSION        3   /* v3: drying fields */
#define TELEMETRY_POLL_HINT_MS     1000      /* advisory; browser owns cadence. TUNE */

/* Integer mirror of OVER_TEMP_LIMIT_C (260.0f) for integer-constant-expression
 * guards. Keep in sync with OVER_TEMP_LIMIT_C above. */
#define OVER_TEMP_LIMIT_C_INT 260
_Static_assert((int)(OVER_TEMP_LIMIT_C) == OVER_TEMP_LIMIT_C_INT,
               "OVER_TEMP_LIMIT_C_INT out of sync with OVER_TEMP_LIMIT_C");
_Static_assert(STEP_TEST_CEILING_MAX_C <= OVER_TEMP_LIMIT_C_INT - 60,
               "STEP_TEST_CEILING_MAX_C must stay >=60 C below OVER_TEMP_LIMIT_C");

/* ============================================================
 * ADJUSTABLE PARAMETER BOUNDS (keypad clamps to these)
 * ============================================================ */

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

/* Planned-curve heat-up-1 start. The curve is regenerated at run start with
 * the measured oven temperature so setpoint(0) == temperature (no standing
 * error to wind the integrator up over ramp 1). PROFILE_START_MIN_C floors
 * that start so a cold room can't lengthen ramp 1 past the curve buffer:
 * worst-case active samples = (SOAK_TEMP_MAX - PROFILE_START_MIN_C)/HEATUP_RATE_MIN
 * + SOAK_TIME_MAX + (REFLOW_TEMP_MAX-SOAK_TEMP_MAX)/HEATUP_RATE_MIN + REFLOW_TIME_MAX
 * = 900 + 300 + 250 + 120 = 1570 < PROFILE_CURVE_MAX_SAMPLES (see profile_curve.h).
 * PROFILE_START_DEFAULT_C is the nominal used for the pre-run display curve
 * (before any run captures the real temperature). */
#define PROFILE_START_MIN_C      20.0f
#define PROFILE_START_DEFAULT_C  25.0f

/* Derivative low-pass time constant (s). The PID derivative is a raw
 * sample-to-sample temperature slope; on a noisy thermocouple at the 250 ms
 * control tick that injects large per-sample spikes straight into the SSR
 * (a derivative on ±2 °C noise is hundreds of % duty). A first-order LPF at
 * this tau cuts the per-tick noise while preserving the slow thermal
 * derivative (which changes over tens of seconds). Set 0 to disable. */
#define PID_D_FILTER_TAU_S        2.0f

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

/* Enforce the cross-bound at compile time too (spec §9). */
_Static_assert(REFLOW_TEMP_MAX <= OVER_TEMP_LIMIT_C - 10.0f,
               "REFLOW_TEMP_MAX must leave >=10 C headroom below OVER_TEMP_LIMIT_C");

/* ============================================================
 * UI — graph rendering (spec §7.2)
 * ============================================================ */
/* Live-trace ring buffer depth (number of (time, temperature) samples
 * retained for the live graph trace). The live trace is appended at the
 * planned-curve granularity (one sample every PROFILE_CURVE_STEP_MS,
 * decimated from the UI_REFRESH_MS tick), so the same cap that fits the
 * worst-case planned curve fits the matching live trace too:
 *   PROFILE_CURVE_MAX_SAMPLES = 1600 samples * 1000 ms = 1600 s coverage.
 * Static cost: 1600 * 8 B = 12.8 KB. Matching the planned curve gives
 * identical horizontal resolution and avoids visible jaggies the eye
 * couldn't have resolved at 4 Hz anyway. */
#define GRAPH_LIVE_RING_CAP   PROFILE_CURVE_MAX_SAMPLES

/* UI refresh tick. Independent of CONTROL_TICK_MS; the UI just needs
 * to be fast enough that the user perceives the temperature label as
 * "live". Cheap to lower; expensive to raise much above 100 ms only
 * if LVGL render becomes the bottleneck. */
#define UI_REFRESH_MS         250
_Static_assert(UI_STALL_TIMEOUT_MS > 4u * UI_REFRESH_MS,
               "UI stall timeout must allow several missed UI ticks");

/* Planned cool-down rate bounds. Used by profile_curve_generate to
 * extend the planned curve past reflow down to AMBIENT_C, so the UI
 * can show a full HEAT → SOAK → HEAT → REFLOW → COOL_DOWN line. Real
 * cool-down is exponential (Newton's law), but a linear approximation
 * at a typical convection-oven rate is faithful enough for setpoint
 * generation and visualisation. The state machine still exits
 * cool-down based on COOL_DOWN_THRESHOLD_C / COOL_DOWN_TIMEOUT_MS,
 * not this planned curve. MIN > 0 to avoid divide-by-zero in curve
 * gen; very low rates may truncate at PROFILE_CURVE_MAX_SAMPLES. */
#define COOL_DOWN_RATE_MIN        0.2f
#define COOL_DOWN_RATE_MAX        5.0f
#define COOL_DOWN_RATE_DEFAULT    1.0f

/* ============================================================
 * NVS PERSISTENCE
 * ============================================================ */
#define NVS_NAMESPACE          "reflow"
#define NVS_KEY_PROFILE        "profile"
#define NVS_KEY_PID            "pid"
#define NVS_KEY_DRY_PRESETS    "dry_presets"
#define PROFILE_SCHEMA_VERSION   2   /* v1 -> v2: added cool_down_rate (2026-05-23) */
#define PID_SCHEMA_VERSION       2   /* v2: + k_hold/k_rate (feedforward) */
#define DRY_PRESETS_SCHEMA_VERSION 1 /* v1: 6 x {temp_c, hold_min}, names not stored */

/* ============================================================
 * TOUCH CALIBRATION — affine map from raw 12-bit XPT2046 ADC
 * counts to portrait display pixels:
 *     x_disp = AX*x_raw + BX*y_raw + CX
 *     y_disp = AY*x_raw + BY*y_raw + CY
 * Applied at runtime by drivers/touch.c via the esp_lcd_touch
 * process_coordinates callback. To re-measure, flash
 * tools/bringup/08_touch_cal and paste the printed block.
 *
 * Measured 2026-05-23 on the E32R35T board:
 *   residuals max=2.12 px, rms=1.75 px (well under the 5/3 PASS limits)
 *   negative BY confirms the Y-axis inversion the bring-up's
 *   mirror_y=1 flag used to compensate for.
 * ============================================================ */
#define TOUCH_CAL_AX      0.086941f
#define TOUCH_CAL_BX     -0.000122f
#define TOUCH_CAL_CX       -18.286f
#define TOUCH_CAL_AY     -0.000538f
#define TOUCH_CAL_BY     -0.128704f
#define TOUCH_CAL_CY       511.100f

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

/* FM8002E chip wakeup time TD (datasheet p.3) typ. 100 ms. Held after
 * releasing SHUTDOWN before the first tone of a melody so short pips
 * (e.g. STAGE_TRANSITION = 80 ms) don't finish before the amp wakes. */
#define AUDIO_AMP_WAKEUP_MS     100

#define NVS_KEY_MUTE          "mute"

/* === Networking + OTA NVS keys (new in v2-OTA) === */
#define NVS_KEY_WIFI_SSID  "wifi_ssid"
#define NVS_KEY_WIFI_PASS  "wifi_pass"
#define NVS_KEY_WIFI_SET   "wifi_set"     /* u8: 1 if creds persisted */
#define NVS_KEY_OTA_TOKEN  "ota_token"    /* 32 hex chars (16 bytes) */

#define WIFI_SSID_MAX_LEN     32
#define WIFI_PASS_MAX_LEN     64
#define OTA_TOKEN_HEX_LEN     32

/* The one name this oven answers to. Used for BOTH the DHCP hostname sent to
 * the router (so the client list shows "reflow" rather than the chip default
 * "espressif") and the mDNS name behind reflow.local. Deliberately a single
 * constant: two spellings would put a different name in the router's lease
 * table than the one people type in a browser. */
#define WIFI_HOSTNAME  "reflow"

/* WiFi connection tunables. Retry backoffs apply on disconnect events:
 * first retry after 5 s, second after 10 s, third after 20 s, then give up. */
#define WIFI_CONNECT_TIMEOUT_MS  30000
#define WIFI_CONNECT_RETRY_MAX   3
#define WIFI_RETRY_BACKOFFS_MS   { 5000, 10000, 20000 }

/* OTA tunables */
#define OTA_UPLOAD_CHUNK_SIZE       4096
#define OTA_RECV_TIMEOUT_TOTAL_MS  30000  /* max no-progress stall window before aborting an upload */
#define OTA_SELF_TEST_TIMEOUT_MS    2000  /* per individual self-test */
#define OTA_SELF_TEST_TOTAL_MS      5000  /* total budget for all self-tests */
/* Bounded wait when /version samples the LVGL pool. Long enough that a normal
 * render tick (which holds the lock briefly) does not cause a spurious miss,
 * short enough that a wedged UI task cannot stall the HTTP handler — the point
 * of the readout is to stay reachable when the panel is not. */
#define LVGL_STAT_LOCK_MS            200
/* TC alive-check: one window = 3 samples, OTA_TC_SAMPLE_GAP_MS apart, all
 * fault-free, in range, within OTA_TC_CONSENSUS_TOLERANCE of each other.
 * Boot-time readings wiggle (WiFi TX bursts + LCD common-mode pickup made
 * validation a coin flip on a healthy sensor, 2026-06-10), so the hook tries
 * up to OTA_TC_WINDOW_ATTEMPTS windows, OTA_TC_RETRY_GAP_MS apart, and the
 * tolerance is 4 °C (was 2 °C) — a sensor swinging >4 °C within 150 ms is
 * genuinely unusable, anything less deserves a retry. */
#define OTA_TC_SAMPLE_GAP_MS          50  /* between the 3 samples of one window */
#define OTA_TC_CONSENSUS_TOLERANCE  4.0f  /* °C spread allowed within one window */
#define OTA_TC_WINDOW_ATTEMPTS         5  /* windows tried before declaring failure */
#define OTA_TC_RETRY_GAP_MS          250  /* pause between windows */
/* Worst case must fit the per-test budget: 5×(3×50) + 4×250 = 1750 ms < 2000 ms. */
_Static_assert(OTA_TC_WINDOW_ATTEMPTS * 3 * OTA_TC_SAMPLE_GAP_MS
             + (OTA_TC_WINDOW_ATTEMPTS - 1) * OTA_TC_RETRY_GAP_MS
             <= OTA_SELF_TEST_TIMEOUT_MS,
             "TC alive-check retry schedule exceeds OTA_SELF_TEST_TIMEOUT_MS");

/* Local timezone for status-bar clock (POSIX TZ; Germany w/ DST). */
#define LOCAL_TZ "CET-1CEST,M3.5.0/2,M10.5.0/3"

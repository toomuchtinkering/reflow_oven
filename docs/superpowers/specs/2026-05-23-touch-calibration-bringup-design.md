# 08_touch_cal — Touch Screen Calibration Bring-Up

**Status:** Design approved 2026-05-23
**Scope:** Standalone bring-up tool only. Main-firmware integration is a follow-up task (see §7).

## 1. Purpose

The current touch path ([src/drivers/touch.c](../../../src/drivers/touch.c)) initialises the
XPT2046 with `mirror_y = 1` and the chip's normalised `x_max = 320, y_max = 480`
range, trusting the driver's built-in scaling. On a resistive panel that
normalisation is only as accurate as the chip's factory ADC range matches the
physical panel — and on this board (LCDwiki E32R35T) it doesn't match well
enough for pixel-precise UI.

This tool measures the actual mapping from XPT2046 raw ADC counts to display
pixels, fits a 5-point least-squares affine transform, validates the fit, and
prints the resulting coefficients to UART in a form ready to paste into
`include/config.h`. The output is the deliverable; nothing is persisted to NVS,
and the main firmware is not modified by this task.

## 2. Deliverable

A new bring-up project at [tools/bringup/08_touch_cal/](../../../tools/bringup/08_touch_cal/),
structured identically to the existing bring-ups
([03_touch](../../../tools/bringup/03_touch/),
[04_lvgl_hello](../../../tools/bringup/04_lvgl_hello/)):

```
tools/bringup/08_touch_cal/
├── CMakeLists.txt
├── platformio.ini          # build_flags: -I../../../include
├── sdkconfig.esp32dev
└── src/
    ├── CMakeLists.txt
    ├── idf_component.yml   # lvgl/lvgl, esp_lvgl_port, esp_lcd_st7796,
    │                       # atanisoft/esp_lcd_touch_xpt2046
    └── main.c
```

All hardware pins (`PIN_LCD_*`, `PIN_TOUCH_*`) come from
[include/config.h](../../../include/config.h) — no inlined pin numbers in the
tool, per project convention.

## 3. Architecture

Three internally-decoupled layers, split across two files:

```
src/drivers/touch_cal_math.{c,h}        --- math layer (pure C) ---
  touch_cal_solve(raw_x[], raw_y[], disp_x[], disp_y[], out_m)
                                 3x3 normal-equation solve, both axes
  touch_cal_residual(raw_x[], raw_y[], disp_x[], disp_y[], m, out_r)
                                 max + RMS residual in display pixels

tools/bringup/08_touch_cal/src/main.c    --- input + UI + orchestration ---
  app_main()                     SPI bus + ST7796 panel + XPT2046 + LVGL init
  cal_task()                     FreeRTOS task running the state machine
  capture_target(out_x, out_y)   median of 16 settled samples at one target
  show_instructions()            intro screen
  show_crosshair(idx, x, y)      draws/replaces single crosshair + progress label
  show_result(pass, r, m)        pass / fail result screen
  log_block(...)                 UART output block (spec §6)
```

The math layer has **no LVGL and no ESP-IDF dependencies** — pure float
arithmetic on stack arrays. It lives in `src/drivers/` (not in the bring-up's
`main.c`) so it is reachable from both the bring-up build *and* the existing
`[env:native]` PlatformIO env that hosts the §8.2 Unity test. The bring-up's
`CMakeLists.txt` references it via a cross-directory `SRCS` entry; same source
file, two build targets. This also pre-positions the math for the §7 follow-up
that will wire it into `src/drivers/touch.c` — no later "lift" step is needed.

## 4. State machine

```
IDLE
 └── app_main returns, cal_task running
SHOW_INSTRUCTIONS
 └── any tap → SAMPLING(0)
SAMPLING(i)  for i = 0..4
 ├── valid clean tap → next state
 └── jittery tap (range > 200 ADC counts on either axis) → stay, redraw "tap again"
SAMPLING(4) → COMPUTING
COMPUTING
 ├── pass (max ≤ 5 px AND rms ≤ 3 px) → RESULT_PASS
 └── fail → RESULT_FAIL
RESULT_*  any tap → SHOW_INSTRUCTIONS
```

A single FreeRTOS task drives every transition. UI mutations take
`lvgl_port_lock()`; touch reads happen outside the lock (the chip driver
arbitrates SPI separately).

## 5. Calibration details

### 5.1 Raw input

XPT2046 is initialised with all driver-side transforms disabled:

```
.x_max = 4095, .y_max = 4095,
.flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
.int_gpio_num = -1,                       // GPIO 36 has no internal pull-up
```

This gives raw 12-bit ADC counts. The fitted affine matrix then captures the
entire raw-ADC → display-pixel transformation, including whatever axis
flip/swap this board needs — there is no "and also set mirror_y first" caveat
hidden under the result.

### 5.2 Target layout

5 targets, in this order:

| i | display (x, y) | rationale                |
|---|----------------|--------------------------|
| 0 |  ( 32,  48)    | top-left, 10% inset      |
| 1 |  (288,  48)    | top-right, 10% inset     |
| 2 |  ( 32, 432)    | bottom-left, 10% inset   |
| 3 |  (288, 432)    | bottom-right, 10% inset  |
| 4 |  (160, 240)    | center                   |

10% inset keeps every target inside the panel's linear region while still
covering ~80% of the addressable area. The fit extrapolates only ~10% toward
each edge.

### 5.3 Sampling

Per target:

1. Wait for `pressed == true`.
2. Settle: discard samples for the first 50 ms.
3. Collect 16 samples at 10 ms intervals.
4. Per-axis sanity: if `max(x_samples) - min(x_samples) > 200` ADC counts, or
   same for y, discard all 16 and prompt the user to tap again (crosshair
   stays on screen).
5. Otherwise: record the per-axis **median** as the captured raw point.

Median (not mean) defends against a single outlier sample on touch-down or
just-before-lift-off without needing explicit outlier rejection.

### 5.4 Fit

Solve, independently for X and Y, the over-determined system:

```
x_disp_i = ax · x_raw_i + bx · y_raw_i + cx
y_disp_i = ay · x_raw_i + by · y_raw_i + cy
```

5 equations × 3 unknowns per axis. Normal equations:

```
[ Σx²    Σxy    Σx  ] [ax]   [ Σx · Xd  ]
[ Σxy    Σy²    Σy  ] [bx] = [ Σy · Xd  ]
[ Σx     Σy     n   ] [cx]   [ ΣXd      ]
```

(Identical shape for Y with `Xd → Yd`.) Solved by 3×3 Gaussian elimination
with partial pivoting in `float`. With 5 corner-spanning points the system is
well-conditioned; double precision is not warranted.

### 5.5 Residuals & pass/fail

For each of the 5 measured points, compute predicted display position by
applying the fitted matrix, then the per-point error `sqrt(dx² + dy²)`. Report:

- `max_residual = max over i`
- `rms_residual = sqrt(mean(err_i²))`

**Pass:** `max ≤ 5.0 px` AND `rms ≤ 3.0 px`. The 5 px max is roughly the
smallest UI button edge in the main fw; the dual check catches both "all
points slightly off" and "one wildly bad tap".

## 6. UART output format

On **pass**, the tool logs (via `ESP_LOGI`, tag `touch_cal`):

```
---------- calibration result ----------
targets (display px):   raw (ADC counts):
  ( 32,  48)              ( 312, 3791)
  (288,  48)              (3742, 3805)
  ( 32, 432)              ( 305,  287)
  (288, 432)              (3738,  301)
  (160, 240)              (2027, 2045)
residuals: max=2.31 px, rms=1.42 px  -> PASS

/* paste into include/config.h */
#define TOUCH_CAL_AX   0.07453f
#define TOUCH_CAL_BX  -0.00018f
#define TOUCH_CAL_CX   8.642f
#define TOUCH_CAL_AY  -0.00009f
#define TOUCH_CAL_BY  -0.12211f
#define TOUCH_CAL_CY  513.84f
----------------------------------------
```

The raw-sample table is included so a future debugging session has the actual
taps, not just the fitted matrix — the math can be re-checked offline if
needed.

On **fail**: same target/raw table + residuals + `-> FAIL (max>5px or
rms>3px)`. No coefficient block is logged in the failure case, so the
scrollback contains nothing that could be accidentally pasted into `config.h`.

## 7. Follow-up (not part of this task)

Once the bring-up has been run on the device and the printed coefficients are
trustworthy, a **separate task** will update the main firmware to apply them:

1. Add a new `TOUCH CALIBRATION` section to
   [include/config.h](../../../include/config.h) containing the six
   `TOUCH_CAL_*` floats, with a comment recording when and on which board they
   were measured.
2. Modify [src/drivers/touch.c](../../../src/drivers/touch.c):
   - Set `mirror_x = mirror_y = swap_xy = 0` and `x_max = y_max = 4095` so the
     driver delivers raw ADC, matching what the calibration was fit against.
   - Install a `process_coordinates` callback on the
     `esp_lcd_touch_config_t` that applies the affine transform on every read
     before LVGL sees the point.
3. Smoke-test on device: confirm taps land where pressed across the full
   screen, including the corners and tab-bar buttons.

No NVS schema change is needed — calibration stays in `config.h` (compile-time,
board-specific), consistent with `feedback_config_h_single_source.md`.

## 8. Verification

### 8.1 On-device (manual)

1. `pio run -e esp32dev -t upload` from `tools/bringup/08_touch_cal/`.
2. Open `pio device monitor`. Confirm intro screen renders with the project's
   dark background.
3. Tap intro → first crosshair at (32, 48) appears.
4. Walk through all 5 targets, holding still on each. Confirm each crosshair
   disappears within ~200 ms of touch-down.
5. Confirm the result screen matches the UART block.
6. Sanity check: tap intentionally off-center on one target — confirm
   residuals increase as expected.
7. Sanity check: tap badly on purpose — confirm the FAIL path renders and no
   coefficient block is emitted.

### 8.2 Host-side test

Add one Unity test under `test/test_touch_cal_math.c` running in the existing
`[env:native]` env (no changes to the env's exclusion filter required — the
math file has no ESP-IDF includes):

- Construct a synthetic ground-truth matrix (e.g. `ax=0.075, bx=0, cx=10,
  ay=0, by=-0.125, cy=510`).
- Generate the 5 known raw points by inverting the matrix on the 5 known
  display targets.
- Run the solver.
- Assert all 6 fitted coefficients match ground-truth to within `1e-4`.
- Assert max/rms residual < `0.01` px.

This catches algebra mistakes in the normal-equations solver without needing
the device.

## 9. Out of scope

- Persistence (NVS): explicitly declined — output is UART only.
- Automated regression of the on-device UI flow.
- Re-calibration on profile change, temperature change, or aging: the
  XPT2046's drift is far below the 5 px threshold over the lifetime of this
  build.
- Main-firmware consumption of the matrix — captured as §7 follow-up.

## 10. References

- Existing bring-ups, used as the structural template:
  [03_touch](../../../tools/bringup/03_touch/),
  [04_lvgl_hello](../../../tools/bringup/04_lvgl_hello/).
- Touch driver to be updated by the §7 follow-up:
  [src/drivers/touch.c](../../../src/drivers/touch.c).
- Config conventions (single source of truth):
  [include/config.h](../../../include/config.h),
  `~/.claude/projects/.../memory/feedback_config_h_single_source.md`.

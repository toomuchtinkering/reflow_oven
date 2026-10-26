# Feedforward control + self-describing run files — design spec

**Date:** 2026-07-19
**Status:** implemented (pending hardware verification) — branch feature/feedforward
**Motivation:** Run `2026_07_19_reflow_run_1.json` (gains Kp 1.55 / Ki 0.011 / Kd 16)
showed 15–25 °C ramp lag with duty never saturated, a +12.5 °C soak excursion that
never settled, and ±10 % duty jitter. Root cause: a pure feedback PID must carry the
entire ramp duty (~60 %) in its integrator, which both lags on ramps and overshoots
when a ramp ends. The plant model from the 2026-07-19 step test (L = 32.5 s,
N = 1.12 °C/s at 80 % duty) lets us compute that duty directly.

## 1. Control math (pid.c) — Approach A: feedforward inside the PID

`pid_gains_t` grows from 3 to 5 fields:

| field    | unit      | meaning                                   | default |
|----------|-----------|-------------------------------------------|---------|
| `kp`     | %/°C      | proportional gain                         | 0.44    |
| `ki`     | 1/s       | integral gain (Ki = Kp/Ti)                | 0.0068  |
| `kd`     | s         | derivative gain (Kd = Kp·Td)              | 4.75    |
| `k_hold` | %/°C      | holding duty per °C above ambient         | 0.19    |
| `k_rate` | %·s/°C    | duty per °C/s of commanded ramp (= 1/R)   | 71.0    |

`pid_step()` gains one input, the profile's current setpoint rate (°C/s):

```
ff  = k_hold · max(setpoint − ambient_ref, 0) + k_rate · max(rate, 0)
raw = ff + P + I + D
out = clamp(raw, PID_OUTPUT_MIN, PID_OUTPUT_MAX)
```

Both ff terms clamp at zero — feedforward may never subtract duty (the
rate term because cool-down carries a negative rate; the hold term because
a setpoint below ambient_ref is outside the loss model).

Rules:

- **Hold term uses the setpoint**, not measured temperature — deterministic, no
  noise coupling into the output.
- **`ambient_ref`** is captured in `pid_reset()` from the temperature argument it
  already receives, clamped to `[AMBIENT_REF_MIN_C, AMBIENT_REF_MAX_C]` = [15, 40] °C.
  The clamp keeps the hold term physical when a run starts on a still-warm oven
  (losses scale with temperature above *room*, not above start temperature).
- **Rate is clamped to ≥ 0** inside `pid_step`: the cool-down tail carries a
  negative rate; feedforward must never subtract duty.
- **Anti-windup tests the sum**: the existing conditional-integration freeze
  (`raw > MAX && error > 0`, `raw < MIN && error < 0`) now operates on
  `ff + P + I + D`, which is the real output. This is the core argument for
  computing ff inside `pid_step` rather than outside.
- With `k_hold = k_rate = 0` the controller is bit-identical to today's PID
  (regression-tested).
- The D low-pass filter (`PID_D_FILTER_TAU_S`) is unchanged.

**What k_rate is and isn't.** `k_rate` is a property of the *oven* (inverse plant
gain 1/R from the step test), not of the recipe. The profile's slopes enter as the
other factor of the product: `ff_rate = k_rate · rate(t)` where `rate(t)` comes
from the selected profile (§2). Editing a profile's ramp rates automatically scales
the feedforward — no retuning. Two boundary conditions:

1. **Capability limit:** the oven tops out around `(100 − hold(T)) / k_rate`
   ≈ 1.0–1.3 °C/s depending on temperature. A profile requesting more saturates
   ff at 100 % and lag reappears — correct behaviour (physics, not controller);
   the §9 ramp-lag metrics make it visible.
2. **Linearity:** R was measured at 80 % duty over 35–180 °C; mild nonlinearity
   elsewhere is absorbed by the PID trim. The trim staying small (integrator
   within ±10 %) is the health indicator for the model.

## 2. Profile rate (profile_curve)

- `profile_curve_t` gains `float stage_rate[STAGE_COUNT_]`, filled by
  `profile_curve_generate()`: HEAT_UP_1 → `first_heatup_rate`, SOAK → 0,
  HEAT_UP_2 → `second_heatup_rate`, REFLOW → 0, COOL_DOWN → `−cool_down_rate`
  (truthful sign; consumer clamps).
- New `float profile_curve_rate(const profile_curve_t *c, uint32_t elapsed_ms)`
  resolves the active stage via the existing `phase_end_ms[]` and returns its
  rate; past the end → 0.
- `control_task.c` passes `profile_curve_rate(...)` into `pid_step()` each tick.
- Analytic stage lookup (not finite differencing) means ff drops its entire
  rate contribution (~43 duty points at 0.6 °C/s) in the very tick a ramp ends —
  this is the soak-overshoot fix.

## 3. Persistence (NVS schema v2)

- `PID_SCHEMA_VERSION` 1 → 2; the blob (`pid_v2_t`) carries 5 floats.
- `pid_from_blob()` accepts v1 (migrates: kp/ki/kd kept, ff fields = defaults)
  and v2; `pid_to_blob()` always writes v2. Stored gains survive the OTA.
- `pid_validate()` extends to the new bounds (§4); invalid → full defaults, as today.

## 4. config.h additions & changes

New (all TUNE-commented, single-source per project convention):

```
KFF_HOLD_MIN 0.0f   KFF_HOLD_MAX 1.0f     KFF_HOLD_DEFAULT 0.19f
KFF_RATE_MIN 0.0f   KFF_RATE_MAX 300.0f   KFF_RATE_DEFAULT 71.0f
AMBIENT_REF_MIN_C 15.0f                   AMBIENT_REF_MAX_C 40.0f
```

Changed:

- `KP_DEFAULT` 10 → 0.44, `KI_DEFAULT` 10 → 0.0068, `KD_DEFAULT` 0 → 4.75
  (the identified no-overshoot set; with ff carrying the duty, gentle feedback is
  the right factory default — the old placeholder Ki = 10 implied Ti = 0.04 s).
- `KI_MAX` 50 → 1.0 (every plausible Ki for this plant is < 0.05; the old cap
  allowed a ~1000× -unstable keypad entry).

Calibration provenance (for the record): `k_hold` 0.19 fitted from run 1's two
quasi-steady segments (soak: ~26 % at 161 °C; peak plateau: ~60 % at 230 °C
climbing 0.32 °C/s), cross-checked against mid-ramp duty. `k_rate` 71 = duty/N
from the step-test fit.

## 5. HTTP API

- **`POST /suggest-gains`**: body requires all 5 fields; validated by
  `pid_validate()`. The SPA is embedded in firmware, so UI and API always match.
- **`TELEMETRY_API_VERSION`** 1 → 2: a stale cached page shows the existing
  version-mismatch banner (and disables Suggest) instead of sending 3-field bodies.
- **`GET /profile`** response adds:
  - `"gains": {kp, ki, kd, k_hold, k_rate}` — active values
  - `"ambient_ref_c": <float>` — the ambient reference captured for the current run
  - via a new mutex-protected `control_get_gains()` accessor (same snapshot
    pattern as `control_get_curve`). No new route; `OTA_MAX_URI_HANDLERS` untouched.
  - When idle, `ambient_ref_c` reflects the last `pid_reset()`; consumers that
    care (the run archive) fetch at run start, when it is by construction current.

## 6. Touchscreen UI

- `screen_pid`: two new keypad rows (K.hold, K.rate) using the existing row
  pattern and the §4 bounds for clamping.
- `screen_pid_tuning.c` confirm dialog lists all 5 suggested values.
- LVGL: two extra rows on an existing light screen; pool headroom verified on
  hardware as part of §10 (64 KB pool, base UI ~33 KB — no heavy-screen coexistence).

## 7. Web tuning page

- `fit.js` `computeGains()` additionally returns `k_rate = dCO/N` (clamped to
  §4 bounds) — every step test re-measures the plant gain for free.
- **k_hold auto-derivation from reflow runs.** New pure function in `fit.js`:

  ```
  k_hold_est(t) = (duty(t) − k_rate · slope(t)) / (temp(t) − ambient_ref)
  ```

  evaluated per sample over the active phases only, with qualifying filters:
  duty > 0, temp ≥ ambient_ref + 30 °C (avoids noise amplification near the
  divide-by-small region), slope smoothed over a ~5 s window, `k_rate` = the
  value active during the run. The estimate is the **median** of the qualifying
  samples (minimum 20 qualifying samples, else no estimate); the IQR is
  reported as confidence. The flag threshold is a FULL interquartile range
  of 0.10, i.e. a half-width of ±0.05; the readout displays median ± IQR/2,
  and the archive stores the full IQR. Rationale: the single-coefficient linear loss model is
  an approximation (wall-mass absorption early in a run, super-linear radiation
  at the top end), so per-segment estimates scatter (run 1: ~0.12 soak vs ~0.19
  peak plateau) — a robust median with visible spread is honest; a blind mean
  is not.
- After a reflow run the page displays the `k_hold` estimate ± IQR next to the
  gains card. "Suggest to oven" carries it **only if the user takes it**
  (default remains the oven's current value fetched from `/profile`); it is
  never auto-applied, and always goes through the existing touchscreen confirm.
- The gains card displays all 5 values.

## 8. Run file format (app.js)

Each saved reflow run gains four blocks; sample array unchanged:

```json
"fw": "v2.3.1",                          // GET /version, fetched once per session
"gains": {kp, ki, kd, k_hold, k_rate},   // GET /profile at run start
"ambient_ref_c": 30.5,                   // GET /profile at run start
"profile": {
  "phase_end_s": {"heat1":…, "soak":…, "heat2":…, "reflow":…, "cool":…},
  "breakpoints": [[t_s, temp_c], …]      // 6 corner points of the planned polyline
},
"metrics": { … }                          // §9; replaces overshoot/aboveLiquidus
```

Breakpoints are derived client-side from `phase_end_ms` + curve corner values;
the 1600-sample polyline is not stored. Step-test runs are unchanged apart from
`fw`.

## 9. Split metrics (verifyRun replacement)

Computed per phase using `phase_end_s`:

| metric                | definition                                              |
|-----------------------|---------------------------------------------------------|
| `ramp1_lag_mean/max`  | setpoint − temp during HEAT_UP_1 (K_HOLD calibration signal) |
| `ramp2_lag_mean/max`  | same during HEAT_UP_2                                   |
| `soak_excursion`      | max(temp − soak_temp) during SOAK                       |
| `peak_overshoot`      | max(temp − reflow_temp) from REFLOW start to run end    |
| `coast`               | max temp after last nonzero duty − temp at cutoff       |
| `above_liquidus_s`    | unchanged (LIQUIDUS_C = 217)                            |
| `k_hold_est`          | §7 estimator median (absent if < 20 qualifying samples) |
| `k_hold_iqr`          | §7 estimator full interquartile range (confidence)      |
| `k_hold_low_confidence` | §7 flag: full IQR > 0.10                              |

Archiving `k_hold_est`/`k_hold_iqr` per run also gives plant-drift tracking
(door seal aging, insulation degradation) across the run history for free.

The old single `overshoot` field is dropped (it conflated the soak excursion with
peak overshoot; run 1's "12.5" was the soak). Run-list readout shows the headline
three: soak excursion, peak overshoot, TAL. Old archived files keep their old
fields; the list renderer tolerates both shapes.

## 10. Testing

- `test_pid`: ff arithmetic; rate clamp (negative → 0); ambient clamp both ends;
  anti-windup freeze on the *sum*; ff = 0 ⇒ bit-identical legacy behaviour.
- `test_profile_curve`: `stage_rate[]` fill; `profile_curve_rate()` per stage,
  at boundaries, and past end.
- `test_nvs_schema`: v1 → v2 migration (ff defaults injected), v2 round-trip,
  new validate bounds incl. `KI_MAX` change.
- `test_telemetry_json` / server: `/profile` gains + ambient fields.
- `fit.test.mjs`: k_rate derivation and clamping; k_hold estimator on a
  synthetic run with known k_hold (recovers it within tolerance), the
  qualifying-sample filters, and the low-confidence IQR path.
- On hardware: LVGL pool headroom with the extended PID screen; then a
  **calibration reflow run** — ff should carry the duty and the integrator stay
  within ±10 %. Afterwards, accept the §7 `k_hold` estimate if it differs from
  the configured value and confidence is good (expected once or twice); the §9
  lag metrics corroborate.

## Out of scope

- SSR scheduler, reflow state machine, OTA flow — untouched.
- Multiple named profiles and other v2-backlog items.

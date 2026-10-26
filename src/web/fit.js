/* fit.js — Ziegler–Nichols open-loop "reaction-curve" PID identification.
 *
 * METHOD (Ziegler, J.G. & Nichols, N.B., 1942, "Optimum Settings for
 * Automatic Controllers"): drive the process open-loop with a fixed step in
 * controller output ΔCO (here, % SSR duty) and record the temperature rise.
 * The process-reaction curve has:
 *   - L = dead time: delay before the output starts to move (s)
 *   - N = maximum slope of the rise at the inflection point (°C/s)
 * The "reaction rate" is R = N / ΔCO  (°C per second, per % duty).
 *
 * UNITS — why the gains drop straight into the firmware PID:
 *   ΔCO is in % duty (firmware PID output is 0–100 % duty),
 *   error is in °C (firmware PID error = setpoint − temp, °C),
 *   so Kp = 1.2/(R·L) has units (% duty)/(°C·s · 1/s) = (% duty)/°C — exactly
 *   the firmware's output/error units. No conversion needed.
 */

/* Firmware gain bounds — MUST match include/config.h (KP_MAX/KI_MAX/KD_MAX).
 * Kept in sync manually; the JS unit test asserts the clamp values. */
export const BOUNDS = { KP_MAX: 100, KI_MAX: 1.0, KD_MAX: 50,
                        KFF_HOLD_MAX: 1.0, KFF_RATE_MAX: 300 };

/* Mirror of config.h KFF_HOLD_DEFAULT — fallback k_hold when the oven's
 * current value can't be fetched. Kept here with BOUNDS so all firmware
 * constant mirrors live in one place. */
export const KFF_HOLD_DEFAULT = 0.19;

const clamp = (x, hi) => (x < 0 ? 0 : x > hi ? hi : x);

/* Find dead time L and max slope N from a reaction curve.
 * samples: [{t_s, temp_c}] at ~1 Hz. Returns {ok, L, N, tInflect, tempInflect}.
 *
 * Approach: smooth with a short moving average to suppress thermocouple
 * noise, find the inflection as the window of maximum slope (estimated by
 * least-squares regression over a ±FIT_SLOPE_HALFWIN window, not a 2-point
 * central difference — the latter amplifies the per-sample noise and locks
 * onto spikes), then extend the tangent at that point back to the ambient
 * baseline; the time-axis intercept is L. Standard tangent construction of
 * the reaction-curve method. */
const FIT_SLOPE_HALFWIN = 5;   // ±5 samples (~11-wide) regression window for the slope

export function fitReactionCurve(samples) {
    if (!samples || samples.length < 2 * FIT_SLOPE_HALFWIN + 1) return { ok: false };
    const t = samples.map(s => s.t_s);
    const rawTemps = samples.map(s => s.temp_c);
    const y = movingAverage(rawTemps, 3);
    const W = FIT_SLOPE_HALFWIN;

    // Ambient baseline = median of the first few RAW samples (pre-smoothing):
    // a single cold-junction glitch in the first reading would otherwise bias
    // the tangent intercept L. Raw (not smoothed) so the glitch isn't first
    // spread across the early averaged values.
    const baseline = medianOf(rawTemps.slice(0, Math.min(5, rawTemps.length)));
    // Inflection = window of maximum regression slope (robust to noise).
    let N = 0, idx = -1;
    for (let i = W; i < y.length - W; i++) {
        const slope = regressionSlope(t, y, i, W);
        if (slope > N) { N = slope; idx = i; }
    }
    // Require a meaningful rise; a flat/noisy trace has no usable inflection.
    const totalRise = y[y.length - 1] - baseline;
    if (idx < 0 || N <= 0 || totalRise < 5) return { ok: false };

    // Tangent at the inflection: temp = y[idx] + N·(t − t[idx]).
    // Solve for the time where the tangent crosses the ambient baseline.
    const L = t[idx] - (y[idx] - baseline) / N;
    return { ok: true, L: Math.max(0, L), N, tInflect: t[idx], tempInflect: y[idx] };
}

/* Median of a small array — used for a glitch-robust ambient baseline. */
function medianOf(arr) {
    const s = [...arr].sort((a, b) => a - b);
    const m = s.length;
    return m === 0 ? 0 : (m % 2 ? s[(m - 1) / 2] : (s[m / 2 - 1] + s[m / 2]) / 2);
}

/* Least-squares slope (dy/dt) of the points in [i−W, i+W]. */
function regressionSlope(t, y, i, W) {
    let st = 0, sy = 0, stt = 0, sty = 0, n = 0;
    for (let j = i - W; j <= i + W; j++) {
        st += t[j]; sy += y[j]; stt += t[j] * t[j]; sty += t[j] * y[j]; n++;
    }
    const denom = n * stt - st * st;
    return denom !== 0 ? (n * sty - st * sy) / denom : 0;
}

function movingAverage(arr, half) {
    const out = new Array(arr.length);
    for (let i = 0; i < arr.length; i++) {
        let sum = 0, cnt = 0;
        for (let j = Math.max(0, i - half); j <= Math.min(arr.length - 1, i + half); j++) {
            sum += arr[j]; cnt++;
        }
        out[i] = sum / cnt;
    }
    return out;
}

/* Compute PID gains from N, ΔCO (% duty step), L (s) under a tuning rule.
 * Returns {kp, ki, kd} clamped to firmware BOUNDS. All gains in firmware
 * units (Kp in %/°C; Ki = Kp/Ti per second; Kd = Kp·Td seconds).
 *
 * Rules:
 *  - 'zn'  Ziegler–Nichols classic (1942) reaction-curve PID:
 *            Kp = 1.2/(R·L),  Ti = 2L,  Td = 0.5L.
 *  - 'conservative'  A gentler variant for processes that overshoot under
 *            classic ZN. We scale the ZN proportional gain down and lengthen
 *            the integral time (the same intent as Tyreus–Luyben detuning,
 *            but note TL is properly defined for the CLOSED-LOOP ultimate-gain
 *            method, Kp=Ku/3.2, Ti=2.2·Pu — NOT this open-loop reaction curve,
 *            so we deliberately do NOT call this "Tyreus–Luyben"):
 *            Kp = (1/1.7)·(1.2/(R·L)),  Ti = 2.2·(2L),  Td = (2L)/6.3.
 *  - 'no-overshoot'  Pessen / ZN "no overshoot" open-loop variant. The
 *            tabulated no-overshoot proportional constant is ~0.2/(R·L)
 *            (markedly gentler than classic ZN's 1.2), with Ti=2L, Td=L/3.
 *            See e.g. Pessen (1994) and the ZN-variant tables in standard PID
 *            references. Chosen as the default because this oven overshoots.
 */
export function computeGains({ N, dCO, L, rule }) {
    // Reject a degenerate identification: a zero/negative step (dCO), slope (N),
    // or dead time (L) makes R = N/dCO and 1/(R·L) blow up to Inf/NaN, which
    // would silently clamp to the gain caps instead of signalling "no fit".
    // Return zero gains (a do-nothing controller) so callers never apply garbage.
    if (!(dCO > 0) || !(N > 0) || !(L > 0)) return { kp: 0, ki: 0, kd: 0, k_rate: 0 };
    const R = N / dCO;                  // °C/s per % duty
    // Inverse plant gain 1/R = duty per °C/s of ramp — the feedforward rate
    // coefficient. A plant property (spec §1), re-measured by every step test.
    const kRate = clamp(dCO / N, BOUNDS.KFF_RATE_MAX);
    const znKp = 1.2 / (R * L);         // classic ZN proportional gain
    let kp, Ti, Td;
    switch (rule) {
        case 'conservative':
            kp = znKp / 1.7; Ti = 2.2 * (2 * L); Td = (2 * L) / 6.3; break;
        case 'no-overshoot':
            kp = 0.2 / (R * L); Ti = 2 * L; Td = L / 3; break;
        case 'zn':
        default:
            kp = znKp; Ti = 2 * L; Td = 0.5 * L; break;
    }
    const ki = Ti > 0 ? kp / Ti : 0;    // 1/s
    const kd = kp * Td;                 // s
    return {
        kp: clamp(kp, BOUNDS.KP_MAX),
        ki: clamp(ki, BOUNDS.KI_MAX),
        kd: clamp(kd, BOUNDS.KD_MAX),
        k_rate: kRate,
    };
}

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

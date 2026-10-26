import { test } from 'node:test';
import assert from 'node:assert/strict';
import { fitReactionCurve, computeGains, estimateKHold, BOUNDS } from '../../src/web/fit.js';

/* Synthetic first-order-plus-dead-time step response with KNOWN L and slope.
 * samples: [{t_s, temp_c}], duty constant. We construct a curve whose maximum
 * slope and dead time we control, then assert the fit recovers them. */
function synth({ amb = 25, gain = 200, tau = 70, L = 20, dt = 1, n = 240 }) {
    const s = [];
    for (let i = 0; i < n; i++) {
        const t = i * dt;
        const k = t - L;
        const temp = k <= 0 ? amb : amb + gain * (1 - Math.exp(-k / tau));
        s.push({ t_s: t, temp_c: temp });
    }
    return s;
}

test('fitReactionCurve recovers dead time within tolerance', () => {
    const samples = synth({ L: 20 });
    const fit = fitReactionCurve(samples);
    assert.ok(Math.abs(fit.L - 20) <= 3, `L=${fit.L} not within 3s of 20`);
});

test('fitReactionCurve reports a positive max slope N', () => {
    const fit = fitReactionCurve(synth({}));
    assert.ok(fit.N > 0, `N=${fit.N} should be > 0`);
});

test('fitReactionCurve flags failure on a flat (no-rise) trace', () => {
    const flat = Array.from({ length: 60 }, (_, i) => ({ t_s: i, temp_c: 25 }));
    const fit = fitReactionCurve(flat);
    assert.equal(fit.ok, false);
});

test('computeZN gains match hand-computed textbook numbers', () => {
    /* Hard-coded expected values (NOT a re-derivation of the implementation,
     * so a wrong constant in the code is actually caught — review finding #16).
     * N=0.8 °C/s, ΔCO=50 %, L=20 s  =>  R = N/ΔCO = 0.016 °C/(s·%).
     * Classic ZN reaction-curve PID:
     *   Kp = 1.2/(R·L) = 1.2/(0.016·20) = 3.75
     *   Ti = 2L = 40 s   => Ki = Kp/Ti = 3.75/40 = 0.09375
     *   Td = 0.5L = 10 s => Kd = Kp·Td = 3.75·10 = 37.5  (clamped to KD_MAX=50, unaffected) */
    const g = computeGains({ N: 0.8, dCO: 50, L: 20, rule: 'zn' });
    assert.ok(Math.abs(g.kp - 3.75) < 1e-3, `kp=${g.kp}`);
    assert.ok(Math.abs(g.ki - 0.09375) < 1e-4, `ki=${g.ki}`);
    assert.ok(Math.abs(g.kd - 37.5) < 1e-3, `kd=${g.kd}`);
});

test('no-overshoot rule yields a smaller Kp than classic ZN', () => {
    const args = { N: 0.8, dCO: 50, L: 20 };
    const zn = computeGains({ ...args, rule: 'zn' });
    const no = computeGains({ ...args, rule: 'no-overshoot' });
    assert.ok(no.kp < zn.kp, 'no-overshoot Kp should be gentler than ZN');
});

test('conservative rule yields a smaller Kp than classic ZN', () => {
    const args = { N: 0.8, dCO: 50, L: 20 };
    assert.ok(computeGains({ ...args, rule: 'conservative' }).kp
              < computeGains({ ...args, rule: 'zn' }).kp);
});

test('computeGains is monotonic: a manual L/N override recomputes cleanly', () => {
    /* Drag-to-refine feeds adjusted L and N straight back into computeGains.
     * A larger dead time L must reduce Kp (Kp ∝ 1/L). */
    const a = computeGains({ N: 0.8, dCO: 50, L: 20, rule: 'zn' });
    const b = computeGains({ N: 0.8, dCO: 50, L: 40, rule: 'zn' });
    assert.ok(b.kp < a.kp, 'larger L should give smaller Kp');
});

test('fitReactionCurve baseline is robust to a glitch in the first sample', () => {
    /* A wild cold-junction spike in the very first reading must not bias the
     * dead-time L (the baseline is a median of the first few RAW samples). */
    const samples = synth({ L: 20 });
    samples[0].temp_c = 200;            // single bogus first reading
    const fit = fitReactionCurve(samples);
    assert.ok(fit.ok, 'fit should still succeed');
    assert.ok(Math.abs(fit.L - 20) <= 5, `L=${fit.L} not within 5s of 20 despite first-sample glitch`);
});

test('fitReactionCurve rejects a too-short trace (no usable window)', () => {
    const short = Array.from({ length: 8 }, (_, i) => ({ t_s: i, temp_c: 25 + i }));
    assert.equal(fitReactionCurve(short).ok, false);
});

test('computeGains returns zero gains on a degenerate identification', () => {
    /* dCO/N/L = 0 would make R=N/dCO and 1/(R·L) blow up to Inf and clamp to
     * the caps — a silently-wrong "max gains". Must instead return do-nothing
     * zeros so a bad fit never pushes garbage to the oven. */
    for (const bad of [{ N: 0.8, dCO: 0, L: 20 }, { N: 0, dCO: 50, L: 20 }, { N: 0.8, dCO: 50, L: 0 }]) {
        const g = computeGains({ ...bad, rule: 'zn' });
        assert.deepEqual(g, { kp: 0, ki: 0, kd: 0, k_rate: 0 }, `expected zero gains for ${JSON.stringify(bad)}`);
    }
});

test('gains are clamped to firmware bounds (KP_MAX=100, KI_MAX=1, KD_MAX=50)', () => {
    /* Choose inputs whose RAW gains blow past every bound so the clamp must
     * actually engage (not a tautology). N tiny, ΔCO large, L tiny:
     *   R = N/ΔCO = 0.01/100 = 1e-4;  R·L = 1e-4·0.1 = 1e-5
     *   raw Kp = 1.2/1e-5 = 120000  -> clamp to 100
     *   raw Ki = Kp/(2L) = 120000/0.2 = 600000 -> clamp to 1 (KI_MAX shrank
     *   with the identified plant: every plausible Ki is < 0.05)
     *   raw Kd = Kp·0.5L = 120000·0.05 = 6000 -> clamp to 50
     * Assert EQUALITY to the caps, proving the clamp fired. */
    const g = computeGains({ N: 0.01, dCO: 100, L: 0.1, rule: 'zn' });
    assert.equal(g.kp, 100);
    assert.equal(g.ki, 1.0);
    assert.equal(g.kd, 50);
});

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

/* Synthetic reflow run generated FROM the model with known k_hold: ramp
 * 0.5 °C/s from 25->150 then hold. duty = k_hold*(T-amb) + k_rate*slope. */
function synthRun({ kHold = 0.2, kRate = 70, amb = 25, dt = 1.25, n = 400 }) {
    const s = [];
    for (let i = 0; i < n; i++) {
        const t = i * dt;
        const T = Math.min(150, amb + 0.5 * t);
        const slope = T < 150 ? 0.5 : 0;
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
    assert.equal(est.lowConfidence, false);
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

test('estimateKHold ignores samples past the active phases (heater-off tail)', () => {
    /* Append a cooling tail with duty 0 after activeEndS: must not affect the
     * estimate (duty>0 filter + activeEndS window both exclude it). */
    const run = synthRun({ n: 320 });                  // ends at t=398.75
    for (let t = 400; t < 500; t += 1.25)
        run.push({ t_s: t, temp_c: 150 - (t - 400) * 0.8, duty: 0, setpoint_c: 0 });
    const est = estimateKHold(run, { kRate: 70, ambientRef: 25, activeEndS: 399 });
    assert.equal(est.ok, true);
    assert.ok(Math.abs(est.median - 0.2) < 0.02, `median=${est.median}`);
});

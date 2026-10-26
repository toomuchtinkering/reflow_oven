/* app.js — wires the tuning dashboard to the device API. Imports the pure
 * fit math from fit.js (inlined at embed time). Run-lifecycle detection uses
 * run_id + steptest.active + end_reason per spec §3.1. */
import { fitReactionCurve, computeGains, estimateKHold, KFF_HOLD_DEFAULT } from './fit.js';
import { buildAligned, fmtElapsed } from './align.js';
import { escapeHtml } from './html.js';

/* Bumped to 3 with telemetry v3 (the dry_* fields). Archive import still
 * accepts every older doc (1 <= doc.api <= API_EXPECTED), so v2 archives keep
 * loading. */
const API_EXPECTED = 3;
const POLL_MS = 1000;
const $ = id => document.getElementById(id);
const LS_KEY = 'reflow_tuning_runs';

let curRunId = null, samples = [], wasActive = false, lastFit = null, curDuty = 0;
let fwVersion = null;     // from GET /version; refreshed at each run start (run archive)
let lastMetrics = null;   // metrics of the just-verified reflow run (k_hold suggest)
let joinedLate = false;   // true if the first sample of this run was already active (spec §3.1)
let curCeiling = 0;       // step-test ceiling for the current run (drawn as the ceiling line)
let wasRunning = false;   // previous sample was a normal reflow run (verification overlay)
let wasDrying = false;    // previous sample was a filament dry (run-lifecycle detection)
let curKind = null;       // 'step' | 'reflow' | 'dry' — set at run start, drives chart + archive
let curTarget = 0;        // drying target °C, latched from the first active sample
let curHoldMs = 0;        // drying hold duration, likewise
let plannedCurve = null;  // { step_ms, temp_c:[…] } from GET /profile, drawn ahead on a reflow run
let apiMismatchShown = false;  // version-mismatch banner is toasted only on the transition into
                               // mismatch, so a genuine mismatch can't flap on every poll
/* Which curves are plotted. 'current' = the live / just-finished run; an
 * archived run is keyed by its timestamp. The archive checkboxes toggle these,
 * so any combination (incl. only archived runs, current hidden) can be shown. */
const shown = new Set(['current']);
const PALETTE = ['#a371f7', '#d29922', '#db61a2', '#3fb950', '#58a6ff', '#e6679a'];

/* A genuine telemetry reading has the full expected shape. The web-sim proxy
 * answers 502 {"error":…} when the real device is unreachable, and the firmware
 * has a degraded fallback — neither is a real reading. Treating those as "not
 * live" (rather than parsing s.api off them) is what stops a missing/undefined
 * api from being misread as a version mismatch, which used to flap the banner
 * on connection loss. */
function isLiveTelemetry(s) {
    return !!s && typeof s.api === 'number' && typeof s.temp_c === 'number'
        && typeof s.run_id === 'number' && !!s.steptest;
}

async function poll() {
    try {
        let s = null;
        try {
            const r = await fetch('/telemetry', { cache: 'no-store' });
            if (r.ok) s = await r.json();   // 4xx/5xx (proxy 502, etc.) are NOT live data
        } catch { /* network/parse error — leave s null, handled below */ }

        if (!isLiveTelemetry(s)) {
            $('conn').textContent = '○ connection lost'; $('conn').style.color = 'var(--err)';
            apiMismatchShown = false;   // re-arm the one-shot toast for when a real device returns
            return;
        }

        $('conn').textContent = '● live'; $('conn').style.color = 'var(--ok)';
        $('apiv').textContent = 'api v' + s.api;
        const mismatch = (s.api !== API_EXPECTED);
        $('suggest').disabled = mismatch;   // don't push gains to a mismatched API
        if (mismatch && !apiMismatchShown) showBanner('firmware/UI version mismatch — update needed');
        apiMismatchShown = mismatch;        // toast once on entry; the apiv readout stays as the persistent cue
        onSample(s);
    } catch (e) {
        // A render error must never look like a live connection nor kill polling.
        $('conn').textContent = '○ connection lost'; $('conn').style.color = 'var(--err)';
        /* ...but it must not be silent either. Swallowing this made a chart bug
         * indistinguishable from an unplugged oven. Keep the last one for the
         * console: window.lastPollError. */
        window.lastPollError = String(e) + (e && e.stack ? '\n' + e.stack : '');
        console.error('poll/render failed:', e);
    } finally {
        setTimeout(poll, POLL_MS);
    }
}

function onSample(s) {
    // Status panel
    $('st_mode').textContent = s.mode;
    $('st_temp').textContent = s.temp_c.toFixed(1) + ' °C';
    // cj_c (electronics / cold-junction temp) is null until the device's
    // first sensor read, and absent entirely from pre-cj_c firmware.
    $('st_cj').textContent = (typeof s.cj_c === 'number') ? s.cj_c.toFixed(1) + ' °C' : '—';
    $('st_duty').textContent = s.duty + ' %';
    $('st_elapsed').textContent = (s.elapsed_ms / 1000 | 0) + ' s';
    // One "last end reason" readout, sourced from whichever mode last ran. A
    // dry's reason lives in its own field and would otherwise never surface.
    $('st_end').textContent = (s.mode === 'drying' || (s.dry_end && s.dry_end !== 'none'))
                            ? s.dry_end : s.steptest.end_reason;

    const active = s.steptest.active;
    const running = (s.mode === 'running');   // a normal reflow run (verification overlay)
    // dry_active is absent on pre-v3 firmware; `=== true` keeps undefined falsy
    // without turning a missing field into a live run.
    const drying = (s.dry_active === true);

    // Run lifecycle (spec §3.1). ORDER MATTERS (review finding F18):
    // 1) finalize the PREVIOUS run if it just ended — BEFORE any buffer reset,
    //    so a back-to-back run that starts within one poll can't wipe the
    //    finishing run's samples before we fit/save them;
    // 2) then handle a run_id change (new run → reset buffer);
    // 3) then capture this sample.
    const runIdChanged = (s.run_id !== curRunId);
    const stepEnded = (wasActive  && !active)  || (wasActive  && runIdChanged);
    const reflEnded = (wasRunning && !running) || (wasRunning && runIdChanged);
    const dryEnded  = (wasDrying  && !drying)  || (wasDrying  && runIdChanged);
    if (stepEnded && samples.length > 3)      finalizeRun(s);   // ZN fit + save (step test)
    else if (reflEnded && samples.length > 3) verifyRun();      // overshoot/liquidus (reflow)
    else if (dryEnded && samples.length > 3)  finalizeDry();    // hold tracking (drying)

    const firstPollEver = (curRunId === null);
    if (runIdChanged) {
        // "joined_late" = we connected to a run already in progress and missed
        // its early rise, so the tangent-based dead-time L is unreliable
        // (sample[0] is not ambient). The only case where we miss more than one
        // poll interval is when the FIRST poll of the whole page session is
        // already active — the device had been running before the page opened.
        // (A witnessed start is caught within ~1 poll of run_id appearing, so L
        //  stays trustworthy.)
        joinedLate = firstPollEver && active;
        curRunId = s.run_id; samples = []; lastFit = null; curDuty = 0; curCeiling = 0;
        curKind = active ? 'step' : running ? 'reflow' : drying ? 'dry' : null;
        curTarget = 0; curHoldMs = 0;
        // A zoom belongs to the run it was made on. Carrying it into the next
        // one leaves the chart parked on a window of a run that no longer
        // exists, with no live curve in it and no obvious way back.
        xZoomed = false;
        // Refresh the firmware version at every run start. The page-load fetch
        // goes stale after an OTA update: a tab left open across a flash keeps
        // reporting the pre-update version, so the archive would stamp the wrong
        // fw for runs done after the reboot (observed 2026-07-20). Re-fetching
        // here means the stamp reflects the firmware that actually ran.
        fetchVersion();
        // On a fresh REFLOW run, fetch the planned curve once for the
        // draw-ahead target line. Step tests have no planned curve.
        plannedCurve = null;
        if (running) fetchProfile();   // a dry has no planned curve — its reference is one flat line
        $('verify').style.display = 'none';   // clear the previous run's verification readout
    }
    if (active) {
        // Latch the step duty from an ACTIVE sample only. The firmware keeps
        // steptest.duty nonzero while MODE_STEP_TEST; latching only while active
        // guarantees curDuty (ΔCO) is the real step size, so gains can never be
        // computed with ΔCO = 0 (R = N/0). (review finding F18/#18)
        if (s.steptest.duty > 0) curDuty = s.steptest.duty;   // ΔCO from active sample only
        if (s.steptest.ceiling_c > 0) curCeiling = s.steptest.ceiling_c;  // for the ceiling line
        samples.push({ t_s: s.elapsed_ms / 1000, temp_c: s.temp_c, duty: s.steptest.duty,
                       cj_c: (typeof s.cj_c === 'number') ? s.cj_c : null });
    } else if (running) {
        // Reflow run: capture setpoint too so verifyRun can overlay achieved-vs-target.
        // cj_c rides along into the run archive so the electronics-bay
        // temperature over a full cycle can be analysed offline.
        samples.push({ t_s: s.elapsed_ms / 1000, temp_c: s.temp_c,
                       duty: s.duty, setpoint_c: s.setpoint_c,
                       cj_c: (typeof s.cj_c === 'number') ? s.cj_c : null });
    }
    if (drying) {
        // Latch the head from an ACTIVE sample, and keep latching: a dry that
        // the page joined late still gets its target on the first sample seen.
        curKind = 'dry';
        if (s.dry_target_c > 0) curTarget = s.dry_target_c;
        if (s.dry_hold_ms > 0)  curHoldMs = s.dry_hold_ms;
        // Same shape as the reflow branch — a dry has a setpoint too (the
        // analytic ramp), so achieved-vs-target overlays for free.
        samples.push({ t_s: s.elapsed_ms / 1000, temp_c: s.temp_c,
                       duty: s.duty, setpoint_c: s.setpoint_c,
                       cj_c: (typeof s.cj_c === 'number') ? s.cj_c : null });
    }
    wasActive = active; wasRunning = running; wasDrying = drying;
    redrawChart();
}

/* One-shot fetch of the active planned curve for the verification overlay.
 * Stored as { step_ms, temp_c:[…] }; temp_c[i] is the setpoint at i*step_ms. */
async function fetchProfile() {
    try {
        const p = await (await fetch('/profile', { cache: 'no-store' })).json();
        if (p && Array.isArray(p.temp_c)) plannedCurve = p;
        redrawChart();
    } catch { /* leave plannedCurve null; the incremental setpoint trail still shows */ }
}

async function fetchVersion() {
    try { fwVersion = (await (await fetch('/version', { cache: 'no-store' })).json()).version ?? null; }
    catch { /* leave null; archive stores null */ }
    // Surface it in the header so the running firmware is visible at a glance
    // (and re-fetches after an OTA update show up here on the next run start).
    const el = $('fwv');
    if (el) el.textContent = fwVersion ? 'firmware ' + fwVersion : '';
}

/* Planned setpoint at t seconds (curve is sampled at step_ms). */
function setpointAt(p, tS) {
    const i = Math.min(Math.round(tS * 1000 / p.step_ms), p.temp_c.length - 1);
    return p.temp_c[Math.max(0, i)];
}

/* Verification metrics for a reflow run: peak overshoot above the planned
 * setpoint, and time-above-liquidus (default 217 °C for SAC305; TUNE/UI later).
 * Overlays achieved vs setpoint on the chart. */
const LIQUIDUS_C = 217;
/* Explicit sign: a negative excursion must render "-2.0", not "+-2.0". */
const signed1 = v => (v < 0 ? '-' : '+') + Math.abs(v).toFixed(1);

/* Per-phase verification metrics (spec 2026-07-19 §9). phase_end_ms indices
 * follow the firmware stage enum: [none, heat1, soak, heat2, reflow, cool]. */
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
    const win = (a, b) => samps.filter(x => x.t_s > a && x.t_s <= b);
    const lag = seg => {
        const e = seg.map(x => (x.setpoint_c ?? 0) - x.temp_c);
        return e.length ? { mean: e.reduce((p, q) => p + q, 0) / e.length,
                            max: Math.max(...e) } : null;
    };
    const r1 = lag(win(0, pe[1])), r2 = lag(win(pe[2], pe[3]));
    if (r1) { out.ramp1_lag_mean = r1.mean; out.ramp1_lag_max = r1.max; }
    if (r2) { out.ramp2_lag_mean = r2.mean; out.ramp2_lag_max = r2.max; }
    const soak = win(pe[1], pe[2]);
    if (soak.length)
        out.soak_excursion = Math.max(...soak.map(x => x.temp_c))
                           - setpointAt(planned, (pe[1] + pe[2]) / 2);
    const lastT = samps.length ? samps[samps.length - 1].t_s : pe[4];
    const refl = win(pe[3], lastT);
    if (refl.length)
        out.peak_overshoot = Math.max(...refl.map(x => x.temp_c))
                           - setpointAt(planned, (pe[3] + pe[4]) / 2);
    const est = estimateKHold(samps, { kRate: planned.gains?.k_rate,
                                       ambientRef: planned.ambient_ref_c,
                                       activeEndS: pe[4] });
    if (est.ok) { out.k_hold_est = est.median; out.k_hold_iqr = est.iqr;
                  out.k_hold_low_confidence = !!est.lowConfidence; }
    return out;
}

/* Compact planned-profile record for the run archive: phase ends + the 6
 * corner points of the target polyline (spec §8) — not the 1600-sample curve. */
function profileSummary(p) {
    const pe = p.phase_end_ms.map(ms => ms / 1000);
    const bp = [[0, setpointAt(p, 0)]];
    for (let i = 1; i < pe.length; i++) bp.push([pe[i], setpointAt(p, pe[i])]);
    return { phase_end_s: { heat1: pe[1], soak: pe[2], heat2: pe[3],
                            reflow: pe[4], cool: pe[5] },
             breakpoints: bp };
}

/* Drying metrics: how tightly the hold tracked, and how long it took to get
 * there. Deliberately different from the reflow metrics, which are all
 * profile-segment based and meaningless for a flat 6-16 h hold. Band is the
 * firmware's DRY_HOLD_BAND_C. */
const DRY_BAND_C = 2;
function dryMetrics(samps, target) {
    const out = { kind: 'dry', target_c: target };
    if (!(target > 0) || !samps.length) return out;
    const first = samps.findIndex(s => s.temp_c >= target - DRY_BAND_C);
    if (first < 0) return out;             // never reached the band (heat-up timeout)
    out.time_to_band_s = samps[first].t_s;
    const devs = samps.slice(first).map(s => s.temp_c - target);
    out.hold_samples  = devs.length;
    out.hold_mean_dev = devs.reduce((a, b) => a + b, 0) / devs.length;
    out.hold_max_dev  = devs.reduce((a, b) => Math.abs(b) > Math.abs(a) ? b : a, 0);
    return out;
}

function finalizeDry() {
    const m = dryMetrics(samples, curTarget);
    lastMetrics = null;   // k_hold suggest is a reflow/step affordance only
    $('verifyText').textContent = (m.time_to_band_s == null)
        ? ` dry ${curTarget}° · never reached the band`
        : ` dry ${curTarget}° · to band ${(m.time_to_band_s / 60).toFixed(0)} min`
          + ` · hold ${signed1(m.hold_mean_dev)}° mean`
          + ` · ${signed1(m.hold_max_dev)}° worst`;
    $('verify').style.display = 'block';
    $('kholdSuggest').style.display = 'none';
    saveRun({ kind: 'dry', fw: fwVersion,
              dry_target_c: curTarget, dry_hold_ms: curHoldMs,
              gains: plannedCurve?.gains ?? null,
              profile: null, metrics: m });
    redrawChart();
}

function verifyRun() {
    const m = computeRunMetrics(samples, plannedCurve);
    lastMetrics = m;
    const est = (m.k_hold_est != null)
        ? ` · k_hold estimate ${m.k_hold_est.toFixed(3)} ±${(m.k_hold_iqr / 2).toFixed(3)}`
          + (m.k_hold_low_confidence ? ' ⚠' : '')
        : '';
    // Persistent readout that stays until the next run starts — a 4 s banner was
    // too quick to read for a result you want to compare across runs.
    $('verifyText').textContent =
        ` soak ${signed1(m.soak_excursion ?? 0)}°`
      + ` · peak ${signed1(m.peak_overshoot ?? 0)}°`
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
    redrawChart();   // setpoint overlay drawn below
}

/* Centered median-of-5 over temp_c, applied ONLY to the series handed to the
 * ZN fit — not the chart (raw stays visible) and not the stored/exported
 * samples. The thermocouple throws single-sample spikes (e.g. a 42 °C dip
 * mid-rise) that drag a mean and make fitReactionCurve's max-slope search latch
 * onto noise, inflating N and the dead-time L. A median rejects those outliers
 * outright; centered (±2) so it adds no lag that would push L later. */
function medianSmoothedForFit(samps) {
    const temps = samps.map(s => s.temp_c);
    const n = temps.length;
    return samps.map((p, i) => {
        const win = temps.slice(Math.max(0, i - 2), Math.min(n, i + 3)).sort((a, b) => a - b);
        const m = win.length;
        return { ...p, temp_c: m % 2 ? win[(m - 1) / 2] : (win[m / 2 - 1] + win[m / 2]) / 2 };
    });
}

function finalizeRun(s) {
    lastFit = fitReactionCurve(medianSmoothedForFit(samples));
    let gains = null;
    if (lastFit.ok) {
        // A late-joined run missed the early rise; the dead-time L is unreliable.
        // Annotate it rather than presenting a confidently-wrong number (spec §3.1).
        $('f_L').textContent = joinedLate ? (lastFit.L.toFixed(0) + ' s ⚠ joined late')
                                          : (lastFit.L.toFixed(0) + ' s');
        $('f_N').textContent = lastFit.N.toFixed(2) + ' °C/s';
        $('f_dco').textContent = curDuty + ' %';
        if (joinedLate) showBanner('joined mid-run — dead-time L may be wrong; re-run for a clean fit');
        gains = recompute();   // updates the gain readout AND returns the object — call once
    } else {
        $('f_L').textContent = $('f_N').textContent = '—';
        showBanner('couldn’t auto-fit — too little rise');
    }
    // Store a concrete object (or null), never `undefined`, in the archive.
    saveRun({ kind: 'step', duty: curDuty, ceiling: curCeiling, fw: fwVersion,
              rule: $('rule').value, fit: lastFit, gains: gains || null });
}

function recompute() {
    if (!lastFit || !lastFit.ok) return;
    const g = computeGains({ N: lastFit.N, dCO: curDuty, L: lastFit.L, rule: $('rule').value });
    $('g_kp').textContent = g.kp.toFixed(2);
    $('g_ki').textContent = g.ki.toFixed(2);
    $('g_kd').textContent = g.kd.toFixed(2);
    $('g_krate').textContent = g.k_rate.toFixed(0);
    return g;
}

async function currentOvenGains() {
    try { return (await (await fetch('/profile', { cache: 'no-store' })).json()).gains ?? null; }
    catch { return null; }
}

/* Show the oven's ACTIVE k_hold on the gains card (spec §7: all 5 values).
 * It has no step-test derivation, so it's fetched, not fitted. */
async function refreshOvenKHold() {
    const cur = await currentOvenGains();
    if (cur && typeof cur.k_hold === 'number') $('g_khold').textContent = cur.k_hold.toFixed(2);
}

async function suggest() {
    const g = recompute(); if (!g) return;
    // Keep the oven's current k_hold: the step-test fit has no basis for it
    // (it comes from reflow-run soak data — see suggestKHold).
    const cur = await currentOvenGains();
    g.k_hold = cur?.k_hold ?? KFF_HOLD_DEFAULT;
    $('g_khold').textContent = g.k_hold.toFixed(2);
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
    $('g_khold').textContent = body.k_hold.toFixed(2);
    const token = prompt('OTA token:'); if (!token) return;
    const r = await fetch('/suggest-gains', {
        method: 'POST', headers: { 'X-OTA-Token': token, 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
    });
    showBanner(r.ok ? 'sent — confirm on the touchscreen' : 'rejected (' + r.status + ')');
}

async function stop() {
    const token = prompt('OTA token:'); if (!token) return;
    await fetch('/stop', { method: 'POST', headers: { 'X-OTA-Token': token } });
}

// --- archive (localStorage) ---
function loadRuns() { try { return JSON.parse(localStorage.getItem(LS_KEY)) || []; } catch { return []; } }
/* Persist the just-finished run. `extra` carries the kind-specific fields
 * (step: duty/ceiling/rule/fit/gains; reflow: overshoot/aboveLiquidus). The
 * common timestamp + raw samples are added here. Both kinds land in one
 * archive so Compare can overlay either. */
function saveRun(extra) {
    const runs = loadRuns();
    runs.unshift({ ts: new Date().toISOString(), joinedLate, samples, ...extra });
    try { localStorage.setItem(LS_KEY, JSON.stringify(runs.slice(0, 50))); }
    catch { showBanner('archive full — export to free space'); }
    renderArchive();
}
function renderArchive() {
    const runs = loadRuns();
    $('arcHdr').textContent = `▾ Archive — ${runs.length} runs`;
    // Pinned first row: toggle the live / just-finished "current run".
    const curRow = `<div class=item>
         <input type=checkbox id=show_current ${shown.has('current') ? 'checked' : ''}>
         <span style="color:var(--temp)">■</span> <b>current run</b> <span class=mut>live</span></div>`;
    const rows = runs.map((r, i) => {
        const ts = r.ts.replace('T', ' ').slice(0, 16);
        const color = PALETTE[i % PALETTE.length];
        let meta, detail;
        if (r.kind === 'reflow') {
            meta = 'reflow' + (r.fw ? ' · ' + r.fw : '');
            const m = r.metrics;
            detail = m
                ? `soak ${signed1(m.soak_excursion ?? 0)}° · peak ${signed1(m.peak_overshoot ?? 0)}° · >${LIQUIDUS_C}°C ${(m.above_liquidus_s ?? 0).toFixed(0)}s`
                : `overshoot ${(r.overshoot ?? 0).toFixed(1)}° · >${LIQUIDUS_C}°C ${(r.aboveLiquidus ?? 0).toFixed(0)}s`;   // legacy pre-v2 archives
        } else if (r.kind === 'dry') {
            const h = (r.dry_hold_ms ?? 0) / 3600000;
            meta = `dry · ${r.dry_target_c ?? '?'}° · ${h.toFixed(1)}h` + (r.fw ? ' · ' + r.fw : '');
            const m = r.metrics;
            detail = (m && m.time_to_band_s != null)
                ? `to band ${(m.time_to_band_s / 60).toFixed(0)}min · hold ${signed1(m.hold_mean_dev)}° mean · ${signed1(m.hold_max_dev)}° worst`
                : 'never reached the band';
        } else {   // 'step' (or legacy runs saved before kind existed)
            meta = `step · ${r.duty}% → ${r.ceiling}° · ${r.rule}`;
            detail = r.gains ? `Kp ${r.gains.kp.toFixed(1)} / Ki ${r.gains.ki.toFixed(2)} / Kd ${r.gains.kd.toFixed(0)}` : '—';
        }
        // Every run field below may come from an imported archive file, so it is
        // escaped at the point of interpolation (color and i are ours).
        return `<div class=item>
         <input type=checkbox data-show="${escapeHtml(r.ts)}" ${shown.has(r.ts) ? 'checked' : ''}>
         <span style="color:${color}">■</span>
         <b>${escapeHtml(ts)}</b>
         <span class=mut>${escapeHtml(meta)}</span>
         <span class=mut style="margin-left:auto">${escapeHtml(detail)}</span>
         <button data-del=${i} style="background:#22272e">🗑</button></div>`;
    }).join('');
    $('archive').innerHTML = curRow + rows;

    // Checking a row plots that curve; unchecking hides it. No redraw of the
    // list needed — just update `shown` and repaint the chart.
    const cc = $('show_current');
    if (cc) cc.onchange = () => { cc.checked ? shown.add('current') : shown.delete('current'); redrawChart(); };
    $('archive').querySelectorAll('[data-show]').forEach(cb =>
        cb.onchange = () => { cb.checked ? shown.add(cb.dataset.show) : shown.delete(cb.dataset.show); redrawChart(); });
    $('archive').querySelectorAll('[data-del]').forEach(b =>
        b.onclick = () => { if (confirm('Delete this run?')) { const r = loadRuns();
            const gone = r[+b.dataset.del]; if (gone) shown.delete(gone.ts);
            r.splice(+b.dataset.del, 1); localStorage.setItem(LS_KEY, JSON.stringify(r));
            renderArchive(); redrawChart(); } });
}
function exportRuns() {
    /* Export the TICKED runs only, falling back to everything when nothing is
     * ticked — the checkboxes already express "the runs I care about", and
     * dumping all 50 to share two of them was busywork.
     *
     * Retention is deliberately untouched: every finished run still auto-saves.
     * Requiring runs to be marked in advance was considered and rejected — you
     * rarely know a run was interesting until it has finished. */
    const all = loadRuns();
    const picked = all.filter(r => shown.has(r.ts));
    // Stamp the API version so import can validate shape + api (spec §8).
    const doc = { api: API_EXPECTED, runs: picked.length ? picked : all };
    const blob = new Blob([JSON.stringify(doc, null, 2)], { type: 'application/json' });
    const a = document.createElement('a'); a.href = URL.createObjectURL(blob);
    a.download = 'reflow-tuning-archive.json'; a.click();
}
function importRuns() {
    const inp = document.createElement('input'); inp.type = 'file'; inp.accept = '.json';
    inp.onchange = async () => {
        let doc;
        try { doc = JSON.parse(await inp.files[0].text()); }
        catch { return showBanner('import failed — not valid JSON'); }
        // Accept the {api, runs} envelope; validate api (spec §8). A bare array
        // (no envelope) is rejected as an unrecognised structure.
        if (!doc || !Array.isArray(doc.runs)) return showBanner('import failed — expected {api,runs}');
        // Accept any archive generation up to ours: run DATA is forward-
        // compatible and renderArchive handles both metric shapes. Rejecting
        // api 1 would orphan every pre-v2 export (the calibration baselines).
        if (!(doc.api >= 1 && doc.api <= API_EXPECTED))
            return showBanner(`import failed — api v${doc.api} newer than v${API_EXPECTED}`);
        // Validate per-run shape: a run must have a timestamp and a samples array.
        const valid = doc.runs.filter(r =>
            r && typeof r.ts === 'string' && Array.isArray(r.samples));
        if (valid.length === 0) return showBanner('import failed — no valid runs found');
        const have = loadRuns(); const seen = new Set(have.map(r => r.ts));
        const merged = have.concat(valid.filter(r => !seen.has(r.ts)));   // merge, de-dup by ts
        localStorage.setItem(LS_KEY, JSON.stringify(merged.slice(0, 50)));
        renderArchive();
        showBanner(`imported ${merged.length - have.length} new run(s)`);
    };
    inp.click();
}
function showBanner(msg) { const b = $('banner'); b.textContent = msg; b.style.display = 'block';
    setTimeout(() => b.style.display = 'none', 4000); }

// --- chart (uPlot) ---
/* Archived runs currently checked, each with a stable colour (by archive index,
 * matching the colour dot in its row). */
function shownArchived() {
    const out = [];
    loadRuns().forEach((r, i) => { if (shown.has(r.ts)) out.push({ run: r, color: PALETTE[i % PALETTE.length] }); });
    return out;
}

/* Top of the temperature axis. A dry runs at 40-100 °C, which occupies the
 * bottom sixth of the reflow axis and hides every deviation that matters
 * against a ±2 °C hold band.
 *
 * Unlike the old canvas this no longer has to agree with anything: it is the
 * scale's range, and every position on screen is derived from it by uPlot. */
function chartTmax() {
    return (curKind === 'dry' && curTarget > 0) ? (curTarget + 20) : 250;
}

const C_TEMP = '#f0883e', C_DUTY = '#58a6ff', C_SET = '#3fb950',
      C_CEIL = '#c93c3c', C_PLAN = '#6e7681', C_GRID = '#161c24', C_AXIS = '#7d8590';

let u = null;              // the uPlot instance, or null before the first draw
let uKeys = [];            // series keys in column order; drives reconciliation
let handles = null;        // { L, inflect } DOM elements over the plot area
/* True while the operator is looking at a hand-picked x window. A live run must
 * keep extending as samples arrive, but a 1 Hz poll must not yank someone back
 * out of a zoom (spec §9), so the two cases need telling apart.
 *
 * Set from uPlot's own gesture events rather than inferred from the scale
 * values. Inference looked simpler and was wrong: uPlot widens a single-sample
 * autoscale to [t, t+0.1], which is not the data range, so the very first poll
 * of every run read as "the operator zoomed" and the chart then never
 * autoscaled again — it sat on the first 8 seconds for the whole run.
 * setSelect fires only for a real drag selection (uPlot clears the selection
 * with the hook suppressed), and dblclick is its built-in reset. */
let xZoomed = false;

/* The x window when nothing is zoomed: anchored at t=0, never shorter than a
 * minute — currentTMax()'s old 60 s floor, without which the first few seconds
 * of a run fill the whole width and the curve looks alarming. The last x value
 * already covers the planned curve and every ticked archive run, because those
 * are series in the same aligned data. */
function applyXFull() {
    const xs = u.data[0];
    u.setScale('x', { min: 0, max: Math.max(60, xs.length ? xs[xs.length - 1] : 0) });
}

/* Fade under the achieved-temperature curve. Rebuilt per draw because the
 * gradient is in canvas pixels and the plot height changes on resize.
 *
 * Falls back to the full canvas when bbox is not populated: uPlot resolves
 * series styles once BEFORE the first layout, and createLinearGradient throws
 * on a non-finite coordinate — which surfaced only as "connection lost",
 * because poll() catches render errors. */
function tempFill(u) {
    const bb = u.bbox || {};
    const top = Number.isFinite(bb.top) ? bb.top : 0;
    const h   = Number.isFinite(bb.height) ? bb.height : u.ctx.canvas.height;
    const g = u.ctx.createLinearGradient(0, top, 0, top + h);
    g.addColorStop(0, 'rgba(240,136,62,0.28)');
    g.addColorStop(1, 'rgba(240,136,62,0)');
    return g;
}

/* What should be plotted right now, in column order. Each descriptor carries
 * both its uPlot series options and the points buildAligned needs, so the two
 * cannot drift apart. */
function chartSeries() {
    const out = [];
    if (shown.has('current')) {
        out.push({ key: 'temp', label: 'temp', scale: 'temp', stroke: C_TEMP, width: 2,
                   fill: tempFill, points: samples.map(s => ({ t_s: s.t_s, value: s.temp_c })) });
        if (samples.some(s => s.setpoint_c))
            out.push({ key: 'setpoint', label: 'setpoint', scale: 'temp', stroke: C_SET,
                       width: 1.4, dash: [4, 3],
                       points: samples.map(s => ({ t_s: s.t_s, value: s.setpoint_c })) });
        out.push({ key: 'duty', label: 'duty', scale: 'duty', stroke: C_DUTY, width: 1.4,
                   points: samples.map(s => ({ t_s: s.t_s, value: s.duty })) });
        // Planned reflow target, drawn ahead from t=0 (verification view, §2.4).
        if (plannedCurve && plannedCurve.temp_c.length) {
            const stepS = (plannedCurve.step_ms || 1000) / 1000;
            out.push({ key: 'planned', label: 'planned', scale: 'temp', stroke: C_PLAN,
                       width: 1.5, dash: [4, 3],
                       points: plannedCurve.temp_c.map((T, i) => ({ t_s: i * stepS, value: T })) });
        }
    }
    // Archived curves selected via the checkboxes (colour matches the row dot).
    shownArchived().forEach(({ run, color }) => {
        out.push({ key: 'arc:' + run.ts, label: run.ts.replace('T', ' ').slice(0, 16),
                   scale: 'temp', stroke: color, width: 1.6, alpha: 0.85,
                   points: run.samples.map(s => ({ t_s: s.t_s, value: s.temp_c })) });
    });
    return out;
}

/* Reference lines and the ZN tangent, drawn straight onto uPlot's canvas.
 * Constant lines are drawn rather than declared as series so they cost no
 * column in the aligned data. */
function overlayPlugin() {
    return {
        hooks: {
            // A completed drag selection is the zoom gesture, and the only
            // one: uPlot applies the scale itself right after firing this.
            setSelect: [u => { if (u.select.width > 0) xZoomed = true; }],
            draw: u => {
                const ctx = u.ctx, { left, top, width, height } = u.bbox;
                const xv = t => u.valToPos(t, 'x', true);
                const yv = T => u.valToPos(T, 'temp', true);
                ctx.save();
                ctx.beginPath(); ctx.rect(left, top, width, height); ctx.clip();

                const hline = (T, color, dash, w) => {
                    const y = yv(T);
                    if (!Number.isFinite(y)) return;
                    ctx.strokeStyle = color; ctx.setLineDash(dash);
                    ctx.lineWidth = w * u.ctx.canvas.height / u.over.clientHeight;
                    ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(left + width, y); ctx.stroke();
                    ctx.setLineDash([]);
                };

                if (shown.has('current')) {
                    // Ceiling: where a step test auto-aborts (spec §5 legend).
                    if (curCeiling > 0) hline(curCeiling, C_CEIL, [5, 4], 1);
                    // Drying reference: one flat line at the target.
                    if (curKind === 'dry' && curTarget > 0) hline(curTarget, C_SET, [4, 3], 1.5);
                }

                // ZN tangent — only for the live step-test fit.
                if (shown.has('current') && lastFit && lastFit.ok && samples.length) {
                    const Tmax = chartTmax();
                    const baseT = samples[0].temp_c;
                    const t2 = lastFit.tInflect + (Tmax - lastFit.tempInflect) / lastFit.N;
                    ctx.strokeStyle = '#8b949e'; ctx.setLineDash([6, 4]);
                    ctx.lineWidth = 1 * u.ctx.canvas.height / u.over.clientHeight;
                    ctx.beginPath();
                    ctx.moveTo(xv(lastFit.L), yv(baseT));
                    ctx.lineTo(xv(t2), yv(Tmax));
                    ctx.stroke(); ctx.setLineDash([]);
                }
                ctx.restore();
                positionHandles();
            },
        },
    };
}

/* Drag-to-refine: the tangent is two control points — the inflection point
 * (drives N via its slope to the L-intercept) and the L-intercept on the
 * ambient baseline (drives dead time L).
 *
 * They are DOM elements rather than pixels on the canvas so that a press on a
 * handle can stop the event before uPlot begins a drag-zoom selection: the two
 * gestures are otherwise identical. That also means no hit-testing maths, and
 * touch-drag works. `chartGeom`/`canvasXY` — which hand-mirrored drawChart's
 * coordinate mapping and had to be kept in step with it — are gone;
 * valToPos/posToVal are that mapping. */
function makeHandles() {
    const mk = which => {
        const el = document.createElement('div');
        el.className = 'zn-handle';
        el.dataset.which = which;
        el.style.display = 'none';
        // Stop BOTH: uPlot binds mousedown for its selection, and pointerdown
        // does not suppress the compatibility mouse event.
        const swallow = ev => ev.stopPropagation();
        el.addEventListener('mousedown', swallow);
        el.addEventListener('touchstart', swallow, { passive: true });
        el.addEventListener('pointerdown', ev => {
            ev.stopPropagation();
            if (!lastFit || !lastFit.ok || !shown.has('current') || !samples.length) return;
            // Capture keeps the drag alive when the pointer leaves the 12 px
            // handle, which it immediately does. It is an optimisation, not a
            // precondition: it throws for a pointer id the element never
            // received, and an exception here would leave the handle inert.
            try { el.setPointerCapture(ev.pointerId); } catch { /* keep dragging */ }
            el.dataset.dragging = '1';
        });
        el.addEventListener('pointermove', ev => {
            if (el.dataset.dragging !== '1' || !u) return;
            ev.stopPropagation();
            // posToVal takes a position relative to the PLOT AREA in CSS
            // pixels, which is exactly what over's bounding rect gives — no
            // padding correction, and none in positionHandles either.
            const r = u.over.getBoundingClientRect();
            const t = u.posToVal(ev.clientX - r.left, 'x');
            const T = u.posToVal(ev.clientY - r.top, 'temp');
            if (which === 'inflect') {
                lastFit.tInflect = Math.max(0, t);
                lastFit.tempInflect = Math.max(0, T);
            } else {
                lastFit.L = Math.max(0, t);   // move the baseline intercept in time
            }
            // N is the slope from the L-intercept (at ambient baseline) to the
            // inflection point: N = (tempInflect − baseline) / (tInflect − L).
            const baseT = samples[0].temp_c;
            const dtime = lastFit.tInflect - lastFit.L;
            lastFit.N = dtime > 0 ? (lastFit.tempInflect - baseT) / dtime : lastFit.N;
            $('f_L').textContent = lastFit.L.toFixed(0) + ' s' + (joinedLate ? ' ⚠' : '');
            $('f_N').textContent = lastFit.N.toFixed(2) + ' °C/s';
            recompute();
            u.redraw();
        });
        const end = ev => { delete el.dataset.dragging;
                            if (el.hasPointerCapture?.(ev.pointerId)) el.releasePointerCapture(ev.pointerId); };
        el.addEventListener('pointerup', end);
        el.addEventListener('pointercancel', end);
        return el;
    };
    handles = { L: mk('L'), inflect: mk('inflect') };
    u.over.appendChild(handles.L);
    u.over.appendChild(handles.inflect);
    /* uPlot's built-in reset autoscales to the raw data extent; this puts the
     * anchored, floored window back over the top of it and hands the chart
     * back to the poll. Ignored on a handle, which uPlot also ignores. */
    u.over.addEventListener('dblclick', ev => {
        if (ev.target !== u.over) return;
        xZoomed = false;
        applyXFull();
    });
}

/* Keep the handles on their control points. Called from the draw hook, so it
 * follows zoom, pan, resize and new data without a second code path. */
function positionHandles() {
    if (!u || !handles) return;
    const live = shown.has('current') && lastFit && lastFit.ok && samples.length;
    const place = (el, t, T) => {
        if (!live) { el.style.display = 'none'; return; }
        // valToPos's CSS-pixel form is plot-area relative, and the handles are
        // children of u.over, so it is already the left/top they want.
        const x = u.valToPos(t, 'x');
        const y = u.valToPos(T, 'temp');
        if (!Number.isFinite(x) || !Number.isFinite(y)) { el.style.display = 'none'; return; }
        el.style.display = 'block';
        el.style.left = x + 'px';
        el.style.top  = y + 'px';
    };
    place(handles.L, live ? lastFit.L : 0, live ? samples[0].temp_c : 0);
    place(handles.inflect, live ? lastFit.tInflect : 0, live ? lastFit.tempInflect : 0);
}

/* Canvas height. Fixed rather than read back from #chart, because uPlot's live
 * legend renders below the canvas inside the same container: sizing the canvas
 * to the container's height would push the legend out over the page.
 *
 * Taller than the old 320 px canvas: the axis is 250 °C tall and a drying hold
 * is judged against a ±2 °C band, so vertical pixels are what make a deviation
 * visible at all. */
const CHART_H = 440;

function chartOpts(descs) {
    const el = $('chart');
    return {
        width: el.clientWidth || 640,
        height: CHART_H,
        legend: { live: true },
        cursor: { drag: { x: true, y: false } },   // drag-zoom in time only
        scales: {
            // x is elapsed seconds since run start, NOT a UNIX timestamp.
            // Identity: uPlot runs this for every x setScale — autoscale,
            // drag-zoom and redraw alike — and cannot tell them apart, so the
            // window is decided by the caller (applyXFull) instead. A plain
            // function, NOT a Range.Config object: uPlot only compiles a
            // config object into a range for the non-x scales, so an object
            // here is handed back verbatim and x never gets a min/max at all.
            x: { time: false,
                 range: (u, min, max) => (min == null ? [null, null] : [min, max]) },
            temp: { range: () => [0, chartTmax()] },
            duty: { range: [0, 100] },
        },
        // Every axis is named and every tick carries its unit. The old canvas
        // suffixed the ticks (250°, 100%) and named the units in the legend
        // strip below the chart; that strip is uPlot's own legend now, so the
        // names moved onto the axes where they belong. A dual-axis chart with
        // bare numbers on both sides is a guessing game.
        axes: [
            { scale: 'x', stroke: C_AXIS, label: 'elapsed', labelSize: 20,
              labelFont: '11px system-ui', labelGap: 2,
              grid: { stroke: C_GRID, width: 1 },
              ticks: { stroke: C_GRID, width: 1 },
              // Unit follows the VISIBLE range, so it keeps working zoomed in.
              values: (u, splits) => {
                  const sc = u.scales.x;
                  const span = (sc.max ?? 60) - (sc.min ?? 0);
                  return splits.map(v => fmtElapsed(v, span));
              } },
            { scale: 'temp', side: 3, stroke: C_TEMP,
              label: 'temperature (°C)', labelSize: 18,
              labelFont: '11px system-ui', labelGap: 2,
              grid: { stroke: C_GRID, width: 1 },
              ticks: { stroke: C_GRID, width: 1 },
              values: (u, splits) => splits.map(v => v + '°') },
            { scale: 'duty', side: 1, stroke: C_DUTY,
              label: 'duty (%)', labelSize: 18,
              labelFont: '11px system-ui', labelGap: 2,
              grid: { show: false }, ticks: { stroke: C_GRID, width: 1 },
              values: (u, splits) => splits.map(v => v + '%') },
        ],
        series: [{ label: 'time' }, ...descs.map(d => ({
            label: d.label, scale: d.scale, stroke: d.stroke, width: d.width,
            fill: d.fill, dash: d.dash, alpha: d.alpha ?? 1, spanGaps: true,
            points: { show: false },
        }))],
        plugins: [overlayPlugin()],
    };
}

/* Debugging handle. This page exists to diagnose the oven, and the chart is
 * now a library instance with its own state (scales, zoom window, series) that
 * is otherwise unreachable from the console — including devtools opened against
 * the real device. Exposed deliberately, and used by the browser checks that
 * cover the drag-vs-zoom interaction a unit test cannot reach. */
window.chart = () => u;

/* Rebuild the chart from current state. Replaces drawChart() at every call
 * site. Cheap enough to call on every 1 Hz poll: when only the data changed it
 * is a setData and a repaint, with no series or scale churn. */
function redrawChart() {
    const descs = chartSeries();
    const { data } = buildAligned(descs);
    const keys = descs.map(d => d.key);
    const el = $('chart');

    if (!u) {
        u = new uPlot(chartOpts(descs), data, el);
        uKeys = keys;
        makeHandles();
        applyXFull();   // the constructor autoscales to the raw extent; anchor it
        new ResizeObserver(() => {
            if (u && el.clientWidth) u.setSize({ width: el.clientWidth, height: CHART_H });
        }).observe(el);
        return;
    }

    // The series set changed (a run ticked on or off). uPlot has delSeries as
    // well as addSeries, so the instance is NOT torn down — which keeps the
    // zoom window and the DOM handles alive across a selection change.
    if (keys.length !== uKeys.length || keys.some((k, i) => k !== uKeys[i])) {
        for (let i = u.series.length - 1; i >= 1; i--) u.delSeries(i);
        descs.forEach(d => u.addSeries({
            label: d.label, scale: d.scale, stroke: d.stroke, width: d.width,
            fill: d.fill, dash: d.dash, alpha: d.alpha ?? 1, spanGaps: true,
            points: { show: false },
        }));
        uKeys = keys;
    }
    // resetScales=false throughout: uPlot's own autoscale would drop the 60 s
    // floor and start the axis at the first sample instead of 0, so the x
    // window is set here or deliberately left alone. setData with
    // resetScales=false commits nothing on its own, so exactly one of the two
    // branches below must run — and it must be only one. Both setScale and
    // redraw queue into the same pendScales slot, so calling redraw after
    // applyXFull overwrites the new window with the old one, and the chart
    // silently stops following the run.
    u.setData(data, false);
    if (xZoomed) u.redraw();
    else applyXFull();      // setScale commits, and a commit always repaints
}

$('stop').onclick = stop;
$('suggest').onclick = suggest;
$('kholdSuggest').onclick = suggestKHold;
fetchVersion();
refreshOvenKHold();
$('rule').onchange = recompute;
$('exp').onclick = exportRuns; $('imp').onclick = importRuns;
renderArchive();
poll();

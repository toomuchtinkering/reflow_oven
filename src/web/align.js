/* align.js — turn N independently-timed run traces into the single shared x
 * array uPlot requires, plus the elapsed-time axis formatter.
 *
 * Pure: no DOM, no uPlot, no module state. That is deliberate — it is the one
 * piece of the chart with interesting logic, so it lives here where
 * `npm run test:web` can reach it without a browser (design spec §1.3).
 *
 * Nothing here may interpolate, resample or otherwise invent a value. The
 * chart is display-only and must never be able to influence a gain suggestion
 * (spec §0.4), and lossless alignment is what keeps that guarantee cheap. */

/* Build uPlot's AlignedData from series with independent time bases.
 *
 * Input:  [{ key, points: [{ t_s, value }] }]
 * Output: { data: [xs, ...ys], keys }   — keys in column order, so the caller
 *         can map a column back to the run that produced it.
 *
 * Every series gets a full-length column: its own value at its own timestamps,
 * null everywhere else. uPlot is configured with spanGaps:true, which bridges
 * interior nulls only — a series' trailing nulls have no following value, so
 * nothing is drawn past the end of a run. That reproduces today's behaviour, in
 * which each run is an independent polyline ending where its samples end.
 *
 * Why the x axis is a union of raw timestamps rather than a uniform grid:
 * t_s is elapsed_ms from a ~1 Hz browser poll, so it is jittered and two runs
 * essentially never share a timestamp. Resampling onto a common grid would fix
 * the column count but move vertices the device actually reported. See spec
 * §1.2 — the point count is allowed to grow instead, because there is no
 * measured performance problem to trade against. */
export function buildAligned(series) {
    const keys = series.map(s => s.key);

    /* One pass to collect every distinct timestamp. A Set keyed on the number
     * collapses exact duplicates (series sampled at the same instant, and the
     * several series derived from one run's samples — temp, setpoint, duty —
     * which by construction share their timestamps). */
    const xset = new Set();
    for (const s of series) {
        for (const p of s.points) {
            if (Number.isFinite(p.t_s)) xset.add(p.t_s);
        }
    }
    const xs = [...xset].sort((a, b) => a - b);

    /* Index once, so filling each column is a lookup rather than a search.
     * Without this the whole thing is O(series x points x log x). */
    const xi = new Map();
    for (let i = 0; i < xs.length; i++) xi.set(xs[i], i);

    const data = [xs];
    for (const s of series) {
        const col = new Array(xs.length).fill(null);
        for (const p of s.points) {
            if (!Number.isFinite(p.t_s)) continue;
            /* A dropped sample or a JSON null leaves a gap. Coercing it to 0
             * would draw a spike to the bottom of the chart and read as a real
             * measurement. */
            if (!Number.isFinite(p.value)) continue;
            col[xi.get(p.t_s)] = p.value;
        }
        data.push(col);
    }
    return { data, keys };
}

/* Format an elapsed-time tick. `span` is the currently visible range in
 * seconds, so the unit follows a zoom rather than the run's full length.
 *
 * Replaces niceTimeStep + the inline `t >= 90 ? m:ss : Ns` formatter, which
 * capped its tick step at 1200 s and kept counting in minutes forever: a 12 h
 * dry drew 37 labels across ~552 px, each reading "720:00" (spec §2.2). */
export function fmtElapsed(t, span) {
    const s = Math.round(t);
    if (span < 90) return `${s}s`;
    if (span < 7200) {
        const m = Math.floor(s / 60);
        return `${m}:${String(s % 60).padStart(2, '0')}`;
    }
    const h = Math.floor(s / 3600);
    const m = Math.round((s % 3600) / 60);
    return `${h}:${String(m).padStart(2, '0')}`;
}

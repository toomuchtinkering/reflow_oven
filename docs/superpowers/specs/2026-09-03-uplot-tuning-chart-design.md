# uPlot tuning chart — design spec

**Date:** 2026-09-03
**Status:** implemented 2026-09-04 — see §10 for what the library actually did
**Backlog item:** [`v2_backlog` #19](../../backlog_history.md)
**Motivation:** Replace the hand-rolled canvas `drawChart` in `src/web/app.js`
with [uPlot](https://github.com/leeoniya/uPlot) 1.6.32 (MIT). The driver is
**functionality and appearance, not speed** — see §0.1. The chart gains a
crosshair value readout, drag-to-zoom, a time axis that survives a 12 h dry, and
a live legend; the code loses the duplicated coordinate maths between
`drawChart` and `chartGeom`.

## 0. Constraints that shaped this design

### 0.1 The performance trigger has *not* fired

`v2_backlog` #19 was parked behind "when long-run archives make the hand-rolled
canvas redraw sluggish". **That has not been observed** — it was inferred from
the arithmetic that a 12 h dry at 2 s sampling is ~21,600 points. The maintainer
confirmed on 2026-09-02 that the real motivation is more functionality and more
style.

**Consequence, and it is binding on this spec:** there is no performance target
here, and none may be invented. No decimation, no sampling budget, no frame-time
acceptance criterion. Performance improvements are a welcome side effect of
using a library built for large time series, not a requirement to be designed
against. If the chart is ever measured as slow, that is a new item with a real
number attached.

### 0.2 Measured budget

Measured on `feature/post-drying-followups` at commit `a958c0f`, not estimated:

| Resource | State |
|---|---|
| Firmware image | 1,462,901 / 2,031,616 B per OTA slot — 72.0%, **555 KiB free** |
| Embedded SPA today | **17,204 B** gzipped (49,072 B of HTML + inlined JS) |
| uPlot 1.6.32 | **22,009 B** gzipped JS + **772 B** gzipped CSS |
| Internal RAM | untouched — this is browser-side |

**The backlog's "~15 KiB gzipped" figure for uPlot was wrong.** The real cost is
22 KB, which roughly **doubles** the embedded page to ~40 KB gzipped. Against
555 KiB free that is still irrelevant, but the number is recorded here so the
decision rests on a measurement.

Note also that "flash is full" in the backlog's *Constraints of record* refers to
the **partition table** having no room for a coredump partition. The OTA slot
itself is 72% used. These are different claims and were being conflated.

### 0.3 uPlot must be vendored, not linked

The device serves this page itself, from flash, to a browser on a LAN that may
have no route to the internet. A CDN `<script src>` is therefore not an option:
uPlot is committed into `src/web/` and inlined at build time like `fit.js` and
`app.js` already are (§6).

### 0.4 The chart must not be able to change suggested gains

The ZN fit and every archive metric (overshoot, time above liquidus, hold-in-band)
are computed from the raw `samples` array and from `fit.js`. **No code in this
design feeds chart data back into either.** The aligned array built in §1 is
display-only. This is what makes §1's alignment choices safe to make on visual
grounds alone — this device switches mains voltage, and a chart must never be in
the path of a gain suggestion.

## 1. Data alignment

### 1.1 The problem

uPlot's `AlignedData` is `[xValues, series1Values, series2Values, …]` — **every
series shares one x array**. The current chart has no such constraint: each
archived run is drawn as an independent polyline over its own `t_s` values.

`t_s` is `elapsed_ms / 1000`, where `elapsed_ms` is read from the device by a
1000 ms browser poll (`POLL_MS`, `app.js:10`). The values are therefore jittered
by network and scheduling delay, and two runs essentially **never** share a
timestamp. A naive union of raw timestamps leaves every series null at every
other run's sample times.

### 1.2 Design

Union all shown series' `t_s` into one sorted, de-duplicated x array, and set
**`spanGaps: true`** on every series.

`spanGaps` bridges *interior* nulls only. A series' trailing nulls have no
following value, so nothing is drawn past the end of a run — each run's line
stays continuous over its own extent and stops where that run stopped, which is
exactly today's behaviour.

This is **lossless**: no interpolation and no resampling, so every sample the
device reported is a real vertex. A 12 h dry with five runs overlaid is ~108k
points, which is the workload uPlot exists to handle.

**Rejected alternative:** resampling every series onto a uniform grid at roughly
one point per horizontal pixel. It bounds the point count, but it invents
vertices the device never reported and can move a peak. Given §0.1 there is no
performance problem to justify that trade. If one ever appears, this is the
first thing to try.

### 1.3 Shape

```
buildAligned(series) -> { data: AlignedData, keys: string[] }
```

A **pure function in its own module** (`src/web/align.js`), taking a list of
`{ key, points: [{t_s, value}] }` and returning uPlot-shaped data plus the
series keys in column order. Pure so it can be unit-tested under `node --test`
without a DOM (§8), and separate so `app.js` does not grow another
responsibility.

## 2. Scales, axes and series lifecycle

### 2.1 Scales

Two y scales, mirroring today's dual axis:

| Scale key | Side | Unit | Series |
|---|---|---|---|
| `temp` | left (3) | °C | achieved temp, setpoint, planned curve, archived runs |
| `duty` | right (1) | % | duty |

The temperature range carries over from `chartTmax()` unchanged: `curTarget + 20`
for a drying run, otherwise 250. The reason is preserved from the drying work —
a dry at 40–100 °C occupies the bottom sixth of a 250 °C axis and hides every
deviation that matters against a ±2 °C band.

`duty` is fixed 0–100.

### 2.2 The x axis is elapsed time, not wall-clock

`scales.x.time = false`. uPlot's `time` flag means "data are UNIX timestamps";
these are seconds since run start. A custom `values` formatter on the x axis
picks its unit from the visible range:

| Range | Format | Example |
|---|---|---|
| < 90 s | seconds | `45s` |
| < 2 h | `m:ss` | `4:12` |
| ≥ 2 h | `h:mm` | `7:40` |

**This fixes a real defect.** `niceTimeStep` caps at 1200 s, so a 12 h dry
currently draws **37 x-axis labels** across 552 px of plot width — ~15 px each —
and because the formatter switches to `m:ss` past 90 s they read `720:00` rather
than `12:00`. uPlot chooses tick density from the range via `Axis.space` /
`Axis.incrs`, and because the formatter is driven by the *visible* range it keeps
working as you zoom.

### 2.3 Series lifecycle

The series set changes whenever a run is ticked or unticked, and the x array
changes with it (§1.2).

uPlot exposes both `addSeries(opts, idx)` and **`delSeries(idx)`**, so the
instance is **not** torn down on a selection change. Reconcile the series list
against the newly shown set, then `setData(data, false)`.

Passing `resetScales = false` is deliberate in two places:

- on a **selection change**, so a zoom window you are studying survives ticking
  another run on for comparison;
- on **live polling**, so a 1 Hz telemetry update does not yank you back out of a
  zoom mid-run.

The zoom resets only on an explicit double-click (uPlot's built-in) or at the
start of a new run.

*Implementation note:* if reconciliation proves fiddly in practice, rebuilding
the instance on a selection change is an acceptable fallback — the cost is
losing the zoom window on a tick, not correctness. A rebuild must also re-create
the DOM handles of §3.2 and re-bind their listeners, so keep that teardown in one
function rather than spread across the rebuild path. Live polling must **never**
rebuild.

## 3. The ZN tangent and its drag handles

### 3.1 The gesture conflict

uPlot begins a drag-zoom selection on `pointerdown` over the plot area. That is
the same gesture that currently grabs a tangent control point. The two cannot
both own it.

### 3.2 Design — plugin draws, DOM handles drag

**The tangent line** and the two horizontal reference lines (step-test ceiling,
dry target) are drawn by a **uPlot plugin** on the chart canvas in a `draw` hook,
using `u.valToPos(val, scaleKey)`. Constant reference lines are drawn, not
declared as series, so they cost no columns in the aligned array.

**The two control points** — the L-intercept on the ambient baseline, and the
inflection point — become **absolutely positioned DOM elements** over the plot,
repositioned from `valToPos` in the same `draw` hook.

Because they are real elements, `pointerdown` on a handle **stops propagation**
and uPlot's zoom selection never begins. The conflict is removed rather than
arbitrated — no reaching into uPlot's cursor internals. Three things come free:
a native `grab`/`grabbing` cursor, a hit target that needs no `Math.hypot`, and
working touch-drag on a tablet.

Dragging maps back through `u.posToVal(pos, scaleKey)` and recomputes `L` and
`N` with the existing geometry: `N = (tempInflect − baseline) / (tInflect − L)`.

### 3.3 What this deletes

`chartGeom()` and `canvasXY()` are removed outright. They exist only to
hand-mirror `drawChart`'s `sx`/`syT` mapping for hit-testing, and carry a comment
warning that the two "MUST agree, or the drag hit-testing lands somewhere other
than what was drawn". `valToPos`/`posToVal` are that mapping, by construction.

**`fit.js` is not touched.** It is pure math and stays exactly as it is.

## 4. Visual design

Direction **B — Refined** (chosen 2026-09-03 from three mocked alternatives).
The existing palette is kept in full, because it is already shared with the rest
of the page and is read fluently:

`--temp #f0883e` · `--duty #58a6ff` · `--ok #3fb950` · `--err #c93c3c` ·
planned `#6e7681` · plot ground `#0b0f14`

Changes:

- A soft gradient fill under the achieved-temperature curve, fading to
  transparent at the baseline.
- Grid dimmed from `#1b2129` to `#161c24`, and sparser.
- **Vertical time grid lines are kept** — at the new dimmed weight, not the
  current one. *(Explicitly requested; the first mockup dropped them and that
  was wrong.)*
- Units at the axis head (`°C`, `%`) instead of repeated on every tick label.
- Setpoint dashed, so it reads as a reference rather than a measurement.
- Archived overlays keep their stable per-run palette colour and 85% alpha.
- The **static HTML legend strip** below the canvas (`tuning.html`, `.legend`) is
  **replaced** by uPlot's live legend, which shows each series' value at the
  cursor instead of only naming colours.
- A `ResizeObserver` on the chart container calls `u.setSize({width, height})`,
  retiring the fixed `640×320` canvas that is currently CSS-scaled.

**Not included:** the shaded ±2 °C hold band around a dry target (mockup option
C). It is a new feature riding along on a port, and was set aside for that
reason. It remains a reasonable future item.

## 5. Archive export

Independent of the chart, raised in the same session:

`exportRuns()` currently ignores the archive checkboxes and writes **all** runs
(`loadRuns()`). Change it to export only the **ticked** runs, falling back to all
runs when nothing is ticked.

Retention is **unchanged**: every finished run still auto-saves, still capped at
50, still with the per-run delete button and the "archive full" banner.

Two stricter proposals were considered and rejected: keeping only runs marked in
advance (you rarely know a run was interesting until it has finished — and
`v2_backlog` #25 was just closed, permanently accepting "an unwatched run is
never recorded"; adding "an unmarked run is never kept" compounds it), and
quota-pruning unticked runs (solves a problem that is not being reported).

## 6. Build pipeline

Vendor into `src/web/`:

- `uplot.iife.min.js` — the IIFE build, which defines the `uPlot` global and
  needs no module loader in the inlined single-script scope
- `uplot.min.css`
- `uplot.LICENSE` — MIT, Copyright (c) 2022 Leon Sorokin, committed alongside

`tools/build_web_asset.py` gains two inline steps, following the existing
pattern: the CSS into a `<style>` block and the JS into a `<script>` block,
both **before** `fit.js` and `app.js` so the global exists when `app.js` runs.
The script's existing guarantees are preserved — deterministic gzip (`mtime=0`),
and the `.c` rewritten only when its bytes change so a pre-build hook does not
trigger spurious recompiles.

The vendored files are third-party and must be inlined **verbatim**; the
`export`/`import` stripping applied to `fit.js` and `app.js` must not be applied
to them.

## 7. Files touched

| File | Change |
|---|---|
| `src/web/align.js` | **new** — pure `buildAligned`, unit-tested |
| `src/web/uplot.iife.min.js`, `uplot.min.css`, `uplot.LICENSE` | **new** — vendored, verbatim |
| `src/web/app.js` | **removed:** `drawChart`, `chartGeom`, `canvasXY`, `niceTimeStep`, `tempGridStep`, `currentTMax` (uPlot derives the x range from the data). **kept:** `chartTmax` — it becomes the `temp` scale's range function (§2.1) — and `shownArchived`. **new:** uPlot construction, the tangent plugin, DOM handle drag, revised `exportRuns` |
| `src/web/tuning.html` | `<canvas>` → uPlot container; static `.legend` strip removed; uPlot CSS hook |
| `src/web/fit.js` | **none** |
| `tools/build_web_asset.py` | inline the vendored CSS + JS |
| `test/web/align.test.mjs` | **new** |

## 8. Testing

**Unit** (`npm run test:web`, Node's built-in runner — the command was repaired
in `e55de2c`, having been broken under Node 24):

- `buildAligned`: single series round-trips; two series with disjoint jittered
  timestamps produce one sorted union with nulls in the right columns;
  duplicate timestamps collapse to one x entry; empty input; a series that is
  wholly shorter than another leaves trailing nulls, never fabricated values.
- The x-axis tick formatter: boundaries at 90 s and 2 h, and either side of each.

Neither needs a DOM, which is why §1.3 puts `buildAligned` in its own module.

**Manual**, against `tools/web_sim.py` (mocks `/telemetry`, `/profile`, `/stop`,
`/suggest-gains` from a thermal model; `--device HOST` proxies a real oven):

1. Step test: tangent renders; both handles drag; gains recompute identically to
   before the port.
2. Drag a handle that sits inside the plot area — **uPlot must not start a zoom
   selection**. This is the §3.1 conflict and the single most important check.
3. Drag-zoom elsewhere in the plot; double-click resets.
4. Live run while zoomed: the 1 Hz poll must not reset the zoom.
5. Tick an archived run on and off mid-run: series appear/disappear, zoom
   survives, colours still match the archive row dots.
6. Reflow run: planned curve, setpoint, ceiling and duty all render on the
   correct axes.
7. Long-run axis: simulate a multi-hour dry and confirm `h:mm` ticks at sane
   density — the §2.2 defect.
8. Export with some runs ticked, and with none ticked.
9. Resize the browser window: the chart follows, handles stay on their points.

**Firmware**: `pio run` must still link, and the reported image size is expected
to grow by roughly 23 KB.

## 9. Required manual verification before this is called done

Item 2 above — handle-drag must not trigger a zoom — and item 4 — a live poll
must not reset a zoom — are the two failure modes that a unit test cannot reach
and that would make the chart worse than what it replaces. Neither can be
verified from the build VM alone; both are `web_sim.py` checks in a real browser.

## Out of scope

- **Any performance target or decimation.** See §0.1.
- **The ±2 °C hold band** (mockup C) — deferred, not rejected.
- **Moving the archive list into uPlot's legend.** The list carries export,
  import, per-run metrics and delete; it stays where it is.
- **Changing archive retention.** See §5.
- **`fit.js`.** Untouched.
- **Device-side changes of any kind.** This is entirely browser-side; the only
  firmware effect is the size of the embedded page.

## 10. Implementation notes (2026-09-04)

Written after the fact. Everything in §1–§9 held; these are the uPlot behaviours
that the design did not anticipate, each of which produced a bug that looked like
something else.

**`setData(data, false)` stores, it does not paint.** uPlot only commits when the
scales are reset. With `resetScales = false` on every poll (§2.3) the chart froze
at whatever it had last drawn — including the plugin's `draw` hook, so the ZN
handles never moved. Something must commit afterwards: either `redraw()` or a
`setScale`.

**Both of those queue into the same `pendScales` slot.** `applyXFull()` followed
by `redraw()` therefore overwrites the new window with the old one, and the chart
silently stops following the run. Exactly one of them may run per update.

**A `Range.Config` object is ignored on the x scale.** uPlot compiles a config
object into a range function only for the non-x scales; on x it is handed back
verbatim, so the scale never gets a min/max at all and every `valToPos` returns
`NaN`. The 60 s floor and the t=0 anchor are applied by an explicit `setScale`
instead, and `scales.x.range` is the identity.

**"Did the operator zoom?" cannot be inferred from the scale values.** uPlot
widens a single-sample autoscale to `[t, t+0.1]`, which is not the data range, so
the first poll of every run reads as a zoom. The flag is set from `setSelect`
(fired only for a real drag selection — the internal clear passes `_fire = false`)
and cleared by a `dblclick` listener on `u.over`, alongside uPlot's own reset.

**`valToPos`/`posToVal` are both plot-area relative in CSS pixels.** No
`over.offsetLeft` correction belongs in either direction. Applying one to both
cancels out for dragging while leaving the handles drawn ~50 px left and ~17 px
up of the tangent they belong to.

**uPlot's live legend renders below the canvas, inside the container.** Sizing
the canvas from the container's height pushes the legend out over the page, so
the canvas height is a constant and `#chart` has no fixed height.

Two notes for whoever writes the next browser check: uPlot listens for
`mousemove` on `over` (not the document) and drops a move whose `movementX` is 0,
which a synthetic `MouseEvent` reports unless the init dict says otherwise.

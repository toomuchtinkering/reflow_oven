# PID tuning web UI — design

**Status**: design, not yet implemented
**Branch**: `pid-tuning-web-ui` (off `ota-firmware-update`, since it builds on the HTTP server + WiFi that OTA introduces)
**Promotes**: a slice of v2 backlog item #7 (tiny web UI) + the tooling half of item #8 (PID tuning), retargeted from a USB bring-up tool to an OTA-deployable in-firmware feature
**Related**: [2026-05-25-ota-firmware-update-design.md](2026-05-25-ota-firmware-update-design.md) (HTTP server, WiFi, token auth, idle-reservation handshake, flash budget), [2026-05-15-reflow-oven-mvp-design.md](2026-05-15-reflow-oven-mvp-design.md) §4.1 (PID), [docs/v2_backlog.md](../../backlog_history.md) §7/§8

Tunable parameters (poll rate, default step duty/ceiling, page budget) are flagged **TUNE** and expected to be adjusted during implementation.

> **Reconciliation with the implementation plan (2026-05-31, post-review).** A few decisions were settled while writing/reviewing [the plan](../plans/2026-05-31-pid-tuning-web-ui.md); this spec is updated to match:
> - **Third tuning rule renamed "Tyreus–Luyben" → "conservative."** Tyreus–Luyben is properly defined for the *closed-loop ultimate-gain* method (Kp=Ku/3.2, Ti=2.2·Pu), which does not apply to this *open-loop reaction-curve* identification. The rule offered is a conservative ZN detune; calling it "Tyreus–Luyben" would be technically incorrect. The named set is therefore **ZN / conservative / no-overshoot** throughout. The no-overshoot proportional constant is ~0.2/(R·L) (Pessen/ZN "no overshoot" table), not 0.6.
> - **`joined_late`** (§3.1) is detected as "the *first poll of the page session* shows an already-active run." A run start witnessed while already polling is caught within ~1 poll (~1 s) — far less than a typical dead time — so L stays trustworthy without flagging.
> - **Max-duration** is entered on the touchscreen in **minutes** (1–15), stored as ms; `STEP_TEST_MAX_DURATION_MS` (§7) is the default.
> - **`settings_confirm`** gains an optional on-cancel callback so the §4 "Dismiss" works from both the modal Cancel and Back-navigation.
> - **§10 open question resolved:** suggested gains use a **distinct `CMD_SUGGEST_PID`** (+ `CMD_DISMISS_PID`); the touchscreen confirm enqueues the existing, unchanged `CMD_APPLY_PID`. Step-test parameters travel in the `cmd_msg_t` payload union (`steptest_params_t`).

> **Reconciliation with the implementation (2026-06-01, Phase 1–2 landed + reviewed).**
> - **Embedded-asset mechanism: generated C array, not CMake `EMBED_FILES`.** §6 mitigation 1 and the §2.2 table assume `EMBED_FILES` of a gzipped `tuning.html.gz`. Under PlatformIO's ESP-IDF integration both toolchain embed paths are unreliable (`EMBED_FILES` double-registers the generated `.S` in PlatformIO's SCons build and fails to link; `board_build.embed_files` emits the `.S` but never links it). The implementation instead generates `src/web/tuning_html_gz.c` (gzipped bytes as a `const unsigned char[]` + `_len`) via `tools/build_web_asset.py`, compiled through the normal `src/*.c` glob. **Serving is identical** — `GET /` returns the gzipped bytes with `Content-Encoding: gzip`. Functionally equivalent and more robust on this toolchain.
> - **Asset regeneration is configure-time only (Phase-4 follow-up).** `src/CMakeLists.txt` runs `build_web_asset.py` at CMake-configure time (covers fresh checkouts), but there is no build-time regeneration dependency, so editing `src/web/*` without a reconfigure can ship a **stale** embedded page. The Phase-4 README (Task 4.8) documents the manual `python3 tools/build_web_asset.py` step; harden regeneration (e.g. a PlatformIO `pre:` extra-script) when the SPA starts iterating in Phase 4. **Resolved (Phase 4):** `platformio.ini` now runs `tools/pio_gen_web_asset.py` as a `pre:` extra-script, regenerating the embedded asset on every build, so SPA edits can no longer ship stale.

## 1. Scope & success criteria

### 1.1 Problem

The integrated oven works end-to-end (SSR switching, thermocouple reading true under live mains switching after the GND→PE bond, control loop closing), but the PID gains overshoot the setpoint and leave a steady-state offset on short phases. With the electronics now inside the enclosure, **USB access is gone** — so the previously-planned `tools/bringup/14_step_test` approach is no longer viable. Tuning must be driven over the network and shipped via OTA.

### 1.2 In scope

- A new oven mode `MODE_STEP_TEST`: open-loop fixed-duty heat for PID system identification, run **inside** the existing control-task safety block so it inherits every safety gate.
- A new **Settings → PID Tuning** touchscreen sub-screen to enter step-test parameters (duty %, temperature ceiling, max duration) and **START** the test. Start of any heat is touchscreen-only (physical supervision required).
- New HTTP endpoints on the existing server: `GET /telemetry`, `POST /stop`, `POST /suggest-gains`, and a replaced `GET /` serving an embedded single-page app (SPA).
- A lean, vanilla-JS embedded SPA (dashboard layout) that: polls live telemetry and plots temperature + duty on a dual-axis hand-rolled canvas chart; runs the Ziegler–Nichols reaction-curve fit (dead-time L, max-slope N) in the browser; computes gains under a selectable tuning rule (ZN / conservative / no-overshoot); suggests gains to the oven (touchscreen-confirmed); archives runs in browser localStorage with delete / compare / export / import; and overlays achieved-vs-target during a verification reflow.
- A **stable, versioned device HTTP API** (`"api":1` in telemetry) so a future richer NAS/Docker-hosted UI can target the same contract without firmware redesign.
- **Preserve the existing OTA upload path intact** — see §1.5. This is a hard constraint, not a nice-to-have.

### 1.3 Out of scope (deferred or declined)

- **Remote start of any heat** (step test or reflow). Starting heat requires a person at the oven; no endpoint enqueues a START.
- **NAS/Docker-hosted UI** — the device API is kept compatible for it, but no external UI or CORS handling is built now. (Decision 2026-05-31: lean embedded UI as the self-contained default; rich external UI possible later against the same API.)
- **On-device run-log persistence** (backlog #3) — archive lives in the browser. Step-test traces are not stored in NVS.
- **Relay-feedback autotune** (backlog #8) — this feature provides the *data-driven manual* tuning path; automatic relay autotune remains conditional/deferred.
- **Generic remote start/stop of normal reflow runs** (the broader backlog #7) — only STOP and read-only status are exposed; remote reflow START is deferred.
- **Min-rise watchdog** — considered and explicitly dropped (decision 2026-05-31). Test ends only on ceiling / max-duration / manual-stop / fault.

### 1.4 Success criteria

1. From a browser on the LAN, `http://reflow.local/` serves the tuning dashboard with no other host running.
2. An operator at the oven enters step parameters and starts a step test from the touchscreen; the browser plots the live rise and, on completion, displays L, N, and computed gains.
3. The operator can choose a tuning rule and push suggested gains to the oven; the touchscreen shows a confirm prompt; on confirm, gains are applied and saved via the existing idle-only PID path.
4. Completed runs persist in the browser across reloads; the operator can delete, compare two, export, and import (merge) them.
5. After applying gains, a verification reflow overlays achieved-vs-target and reports peak overshoot and time-above-liquidus.
6. Every safety gate (TC fault, stale sensor, 260 °C cutoff) pre-empts the fixed-duty drive; the step test cannot run concurrently with an OTA upload or a reflow.
7. **OTA upload over WiFi still succeeds on the enclosed hardware after this feature lands** (the §1.5 hard invariant).
8. The operator watches the live rise and can STOP from the touchscreen PID Tuning sub-screen; the browser correctly distinguishes back-to-back runs (no spliced traces) via `run_id`.

### 1.5 Hard invariant: OTA must keep working

USB flashing is no longer practical now the electronics are enclosed — OTA is effectively the *only* update path. Therefore **nothing in this feature may regress `POST /ota`**:

- New handlers are *added* to the existing `httpd` instance; the `/ota`, `/version`, and `/` registrations and their behavior are not altered except `root_handler`'s body (which still returns valid HTML — now the SPA).
- `max_uri_handlers` is raised to fit the new routes (today it is exactly 4 and full — adding any route without raising it silently fails to register).
- The added routes must not exhaust `max_open_sockets` (default 7) in a way that starves an in-progress `/ota` upload — `/telemetry` uses short-lived connections and the browser polls at ~1 Hz, but this is verified (§6).
- The idle-reservation handshake (`control_begin_ota`/`control_end_ota`) is unchanged. `MODE_STEP_TEST` participates in the same `ota_reserved` mutual-exclusion so a step test and an OTA upload can never overlap (§3).
- **Regression gate**: an OTA upload over WiFi must be performed and confirmed working on the real (enclosed) hardware *before* this branch merges. A broken OTA on enclosed hardware is a near-brick scenario.

## 2. Architecture overview

### 2.1 Principle

The device is a thin, safe data source with a stable API. All analysis, visualization, and history live in the browser. Everything that ignites mains heat is physically supervised at the touchscreen.

### 2.2 New / modified components

| File | Role |
|---|---|
| `src/control/reflow_sm.{c,h}` | Add `MODE_STEP_TEST` to `oven_mode_t`, `SM_EVENT_CMD_START_STEP_TEST`; entry guards; abort/exit transitions and end-reason tracking |
| `src/control/control_task.c` | Step-test branch in the per-tick body (fixed duty via existing `ssr_tick`); temp-ceiling + max-duration aborts; hold + apply touchscreen-confirmed suggested gains; publish step-test sub-status |
| `src/oven_state.h` | Extend `oven_state_t` with a monotonic `run_id` (§3.1), step-test sub-status (active, configured duty/ceiling/max-ms, end-reason) and a pending-PID-suggestion flag + values; add `CMD_START_STEP_TEST` and `CMD_SUGGEST_PID` (or reuse pending pattern) command types |
| `src/ota/ota_server.c` | New handlers: `GET /telemetry`, `POST /stop`, `POST /suggest-gains`, `GET /profile`; replace `root_handler` to serve the embedded (gzipped) SPA; bump `max_uri_handlers` |
| `src/ui/screen_pid_tuning.{c,h}` | New Settings sub-screen: duty / ceiling / max-duration entry + START; **live temperature view + STOP while a step test runs** (the operator watches and aborts here — they are standing at the oven); surfaces the pending-gain confirm prompt |
| `src/web/tuning.html` | The SPA source (HTML + inline vanilla JS + CSS). Embedded as a **generated gzipped C array** (`src/web/tuning_html_gz.c` via `tools/build_web_asset.py`), **not** CMake `EMBED_FILES` — see the reconciliation note at the top of this doc and §6. |
| `src/ui/screen_settings.c` | Add a "PID Tuning" row to the Settings hub |
| `CMakeLists.txt` | Register `screen_pid_tuning.c`; embed the gzipped `tuning.html` |
| `include/config.h` | Add step-test bounds/defaults + telemetry poll/version constants (see §7) |
| `test/test_reflow_sm/` | Extend with `MODE_STEP_TEST` entry-guard + abort-reason + gate-preemption cases |
| `test/web/` (or `tools/`) | Node-runnable JS unit tests for the L/N detection + ZN/conservative/no-overshoot formulas against synthetic curves |

### 2.3 Data flow (one tuning cycle)

1. Operator at the oven opens **Settings → PID Tuning**, enters duty / ceiling / max-duration, taps **START** → `CMD_START_STEP_TEST` → control task enters `MODE_STEP_TEST` and bumps `run_id`.
2. Control task drives the SSR at the fixed duty each tick (after all safety gates), publishing `temp_c`, `duty_percent`, `elapsed_ms`, `run_id`, mode, and step-test sub-status into `oven_state_t`. The **PID Tuning sub-screen shows the live temperature rise and a STOP button** so the supervising operator can watch and abort without leaving the screen.
3. Browser polls `GET /telemetry` at ~1–2 Hz (**TUNE**), plots temperature (left axis) and duty (right axis), accumulates the sample array.
4. Test ends on **ceiling** (normal), **max-duration**, **manual STOP** (touchscreen or browser), or **fault**. End reason is published and stored with the run.
5. Browser runs inflection/slope detection → L, N; applies the selected tuning rule → Kp/Ki/Kd; overlays the tangent on the chart; operator may drag the tangent point to refine the fit.
6. Operator clicks **Suggest to oven →** → `POST /suggest-gains {kp,ki,kd}` → control task validates (`pid_validate`) and holds as a *pending suggestion* + sets a flag in `oven_state_t`.
7. Touchscreen shows "Apply Kp/Ki/Kd? [Confirm] [Dismiss]". Confirm routes into the existing idle-only `CMD_APPLY_PID` path (apply + NVS save + seq bump → "✓ saved" toast).
8. Browser saves the run (raw trace + params + gains + end-reason + rule) to localStorage (retaining the **50 most recent** runs; older ones drop off).
9. **Verification**: operator runs a supervised reflow; the browser overlays achieved-vs-target from the same `/telemetry` and reports peak overshoot + time-above-liquidus.

### 2.4 Endpoints

| Method/Path | Auth | Action |
|---|---|---|
| `GET /telemetry` | none (read-only) | One `oven_state_t` mutex read → JSON `{api, mode, temp_c, setpoint_c, duty, elapsed_ms, fault, run_id, steptest:{active,duty,ceiling_c,max_ms,end_reason}, pending_pid:{kp,ki,kd}|null}`. Always 200. `run_id` drives run-lifecycle detection — see §3.1. |
| `POST /stop` | token | Enqueue `CMD_STOP`. Valid in `MODE_STEP_TEST` and `MODE_RUNNING`. 401 on bad token; **503 if the control queue is full** (the STOP was dropped — never reported as success; safety-relevant, the SPA surfaces it and the caller retries). |
| `POST /suggest-gains` | token | Body `{kp,ki,kd}`; `pid_validate`; hold as pending suggestion. Does **not** apply. 400 on malformed/out-of-range, 401 on bad token, **503 if the control queue is full**. |
| `GET /profile` | none (read-only) | The active **planned** reflow curve, fetched once per run (it only changes on APPLY_PROFILE). JSON `{api, step_ms, len, phase_end_ms:[…], temp_c:[…]}` where `temp_c` is the setpoint at each `step_ms` (1 s) sample. Lets the web UI draw the full HEAT→SOAK→REFLOW→COOL target line *ahead* so the live achieved trace visibly chases it (verification of tuning). Not polled — one fetch on run start keeps the ~10 KB body off the 1 Hz telemetry path. |
| `GET /` | none | Serve embedded gzipped SPA (`Content-Encoding: gzip`). |

START of the step test is intentionally **absent** from the API — touchscreen only.

**Verification view (success criterion 5):** on a normal reflow run the web UI draws three things on one chart — the **planned** target curve (from `GET /profile`, drawn ahead from t=0), the **achieved** temperature (live from `/telemetry`), and the **instantaneous setpoint trail** — and reports peak overshoot (max achieved − setpoint) and time-above-liquidus. The planned-ahead line is what makes "is the tuning good?" visually obvious: a well-tuned loop hugs the target line; an over/under-tuned one visibly lags or overshoots it.

## 3. Step-test state machine

### 3.1 Run lifecycle & how the browser detects start/stop

The browser must know, from polling alone, when a run starts, when it ends (and why), and must never splice two runs into one trace — including the case where one run stops and a new one starts between two polls.

Mechanism: a **monotonic `run_id`** (`uint32_t`) in `oven_state_t`, **bumped by the control task each time a run starts** (step test *and* reflow — so the verification overlay benefits too). Combined with `steptest.active` and `end_reason`, the browser logic is:

- `active` true **and** `run_id` unchanged → same run, append samples.
- `run_id` changed → a *new* run started: finalize (or discard if too short) the previous trace, allocate a fresh sample buffer keyed to the new `run_id`.
- `active` was true, now false → run ended: read `end_reason` (`ceiling` / `max_duration` / `manual_stop` / `fault`) and trigger the fit.
- Browser connects **mid-run** (first poll already shows `active` with an unseen `run_id`) → start capturing from that point, mark the run `joined_late` so the fit can warn that the early rise (and thus dead-time L) may be missing.

This makes back-to-back runs unambiguous (the `run_id` increment is the boundary even if `active` looks continuously true across the gap) and lets a late-joining browser behave sensibly. `run_id` need not persist across reboot; it only has to be monotonic within a power cycle (start it at 0 on boot, increment on each run start).

### 3.2 States & events

`MODE_STEP_TEST` added to `oven_mode_t`. New event `SM_EVENT_CMD_START_STEP_TEST` (from `CMD_START_STEP_TEST`, parameters in the command payload or module statics).

**Entry guard** (mirrors START/OTA): `MODE_IDLE` only, `!ota_reserved`, `validation == SM_VALIDATION_NONE`. Step-test and OTA are mutually exclusive via the existing `ota_reserved` discipline (a step test in progress should also block an OTA reservation — enforce symmetrically).

**Per-tick behavior** (slots into `control_task.c` where the `pid_active` branch is today, [src/control/control_task.c](../../../src/control/control_task.c) §6):
- The existing safety block runs first and unchanged. TC fault / stale / >260 °C → `sm_trip_fault` + `ssr_force_off` + exit. The step test never reaches the SSR if a gate trips.
- Otherwise duty = the fixed configured value; `ssr_tick` drives the existing PWM window.
- Added aborts, each → `ssr_force_off` + return to `MODE_IDLE` with end-reason recorded:
  - `ceiling`: `temp_c >= configured ceiling` (normal end)
  - `max_duration`: `elapsed_ms >= configured max`
- `manual_stop`: `CMD_STOP` from either surface.
- `setpoint_c` published as 0 (no setpoint in open loop); the UI plots temperature + duty only.

## 4. Suggested-gain confirm path

Reuses the pending/confirm shape proven by OTA validation. `POST /suggest-gains` → control task stores `s_pending_pid` + sets `oven_state_t.pending_pid` → touchscreen surfaces a confirm prompt (only actionable in `MODE_IDLE`) → Confirm enqueues the existing `CMD_APPLY_PID` (idle-only apply + save + `pid_applied_seq` bump). Dismiss clears the pending suggestion. No new persistence logic; it routes into `apply_non_sm_command`'s existing PID branch. A suggestion may be POSTed while not idle (e.g. immediately post-test) and held until the operator is idle and confirms.

## 5. Web UI (embedded SPA)

**Layout** — two-column dashboard (layout C, approved 2026-05-31):
- **Left ~⅔**: hand-rolled `<canvas>` line chart, **dual-axis** — temperature on the left (°C), duty on the right (%). Overlays: ZN inflection tangent with draggable point + L marker, ceiling line. Below the chart: a **collapsible archive list**.
- **Right ~⅓ control/results rail**: Status (mode/temp/duty/elapsed/end-reason); **STOP** (always available); Reaction-curve fit (L, N, ΔCO; drag-to-refine hint); Tuning-rule dropdown (ZN / conservative / no-overshoot) with live-recomputed Kp/Ki/Kd; **Suggest to oven →** with "confirm on touchscreen" note.

**Archive** (browser localStorage):
- Collapsible list; each row: checkbox, timestamp, params (duty → ceiling · rule), resulting gains, and a **delete** (trash) affordance with confirm.
- Header actions, separate from the list: **Compare** (overlays the checked runs on the chart — checkbox selection is the single mechanism), **Export JSON** (whole archive), **Import JSON** (whole archive, **merge** with de-dup by timestamp).
- localStorage full/disabled → warn, keep current run in memory, still allow Export.

**Fit math** (browser JS):
- `R = N / ΔCO` (°C/s per % duty). Kp comes out in (% duty)/°C — matching the firmware PID's output (0–100% duty) and error (°C) units, so gains drop straight into the device with no unit conversion.
- Classic ZN: `Kp = 1.2/(R·L)`, `Ti = 2L → Ki = Kp/Ti`, `Td = 0.5L → Kd = Kp·Td`. Plus a "conservative" ZN detune and a no-overshoot rule (~`0.2/(R·L)`) from the same L/N.
- If inflection detection fails (too noisy / no clear rise) → "couldn't auto-fit — drag the tangent manually" instead of emitting garbage gains.

**Documentation requirement (decision 2026-05-31):** the fit math is the conceptual core of this feature and must be **fully explained inline in the code**, not left as bare formulas. Each routine (inflection/max-slope detection, `R`/`L`/`N` derivation, and every tuning-rule formula) carries a comment block that:
- states what it computes and the units of every term (e.g. `N` in °C/s, `ΔCO` in % duty, `Kp` in %/°C),
- cites the source method by name (Ziegler & Nichols open-loop *reaction-curve* / *process-reaction* method; the "conservative" ZN detune with its derivation; the chosen no-overshoot rule — e.g. the Pessen/"no overshoot" variant of the ZN open-loop table, ~`0.2/(R·L)`), with enough reference detail (author, rule name, and the table/relation used) that a reader can verify the constants against the literature; and notes explicitly that Tyreus–Luyben is *not* used because it is a closed-loop ultimate-gain rule inapplicable to open-loop reaction-curve identification,
- explains *why* the resulting `Kp` units line up with the firmware PID (the duty/°C derivation above), so a future maintainer doesn't "fix" a non-bug.

The same applies to any C-side mirror if one is ever added (none is planned — the math lives in JS per §2.1).

**Verification overlay**: during a supervised reflow, plot achieved vs the published target; compute peak overshoot (max achieved − setpoint) and time-above-liquidus.

**API version**: telemetry carries `"api":1`; on mismatch the page shows a "firmware/UI version mismatch" banner and disables Suggest.

## 6. Flash budget (the primary risk)

Per the OTA spec §2.4, the OTA firmware measured **~1.42 MB against a 1.47 MB (`0x170000`) app slot** as of 2026-05-29 — roughly **~85 KB of headroom in the slot**, tighter than the earlier ~200 KB estimate. The embedded page competes for that headroom (it must fit in the same app partition OTA writes).

Mitigations, in order:
1. **Embed the SPA gzipped**, serve with `Content-Encoding: gzip`. A ~50 KB HTML/JS page compresses to ~12–15 KB. Browsers decompress transparently. This is the main lever and makes the page cost negligible against the slot.
2. **No charting/UI libraries** — hand-rolled canvas plot, vanilla JS. Page source target **≤ ~50 KB uncompressed** (**TUNE**), hard ceiling ~100 KB.
3. **Measure first**: implementation step 1 is to build the current `firmware.bin` (not built in this checkout) and read the *actual* free slot headroom, then size the page against that real number. If headroom is critically low, the page can be trimmed or the slot layout revisited (out of scope here, but flagged).

**Socket budget**: `/telemetry` polling uses short-lived connections; verify it doesn't exhaust the `max_open_sockets` default (7) shared with `/ota`. Bump `max_uri_handlers` for the added handlers.

## 7. New config.h constants (proposed)

```c
/* Step test (open-loop PID identification) */
#define STEP_TEST_DUTY_MIN          10
#define STEP_TEST_DUTY_MAX          80
#define STEP_TEST_DUTY_DEFAULT      50     /* TUNE */
#define STEP_TEST_CEILING_MIN_C     80
#define STEP_TEST_CEILING_MAX_C     200    /* stays >=60 C below OVER_TEMP_LIMIT_C */
#define STEP_TEST_CEILING_DEFAULT_C 180    /* TUNE */
#define STEP_TEST_MAX_DURATION_MS   600000 /* 10 min backstop; TUNE */

/* Telemetry API */
#define TELEMETRY_API_VERSION       1
#define TELEMETRY_POLL_HINT_MS      1000   /* advisory; the browser owns the poll cadence. TUNE */
```

Exact placement follows the existing config.h grouping. (Single-source-of-truth rule: no inline pin/constant literals in source.)

## 8. Error handling & edge cases

- **Safety gates always win**: the step test runs inside the per-tick safety block; a tripped gate forces SSR off and exits before any fixed-duty drive.
- **Browser disconnect does not auto-stop** (supervised-in-person model): the ceiling + max-duration + the operator bound the heat. The page shows "connection lost" on poll failure and keeps retrying.
- **Endpoints**: 401 on bad/missing token (`/stop`, `/suggest-gains`); 400 on malformed JSON / out-of-range gains; **503 on `/stop` and `/suggest-gains` when the control command queue is full** — the command was dropped, so it is never reported as success (safety-relevant for STOP); the SPA shows the rejection and the caller retries; `/telemetry` is infallible-by-design (one mutex read, fixed buffer, always 200).
- **`/suggest-gains` when not idle**: accepted and held pending; the touchscreen only surfaces/applies the confirm in `MODE_IDLE`.
- **Import JSON**: validate shape + `api`; reject unknown structure with a clear message; **merge** (de-dup by timestamp) so import can't silently wipe existing runs.

## 9. Testing strategy

- **JS unit tests** (node, no browser): the highest-value target — L/N inflection/slope detection and the ZN / conservative / no-overshoot formulas against synthetic first-order-plus-dead-time curves with known answers. Lives in `test/web/` (or `tools/`).
- **State-machine tests** (extend `test_reflow_sm`, Unity, `env:native`): `MODE_STEP_TEST` entry guards (idle-only; rejected during OTA/fault/pending/running), each abort reason (ceiling / max-duration / manual-stop / fault), and that safety gates pre-empt the fixed-duty drive.
- **Endpoint tests** (follow `test_ota_*`): token rejection, idle-gating of apply, `/telemetry` JSON shape (incl. `run_id` monotonicity and `api` field).
- **OTA regression (mandatory gate, §1.5)**: perform and confirm a real OTA upload over WiFi on the enclosed hardware before merge. Also verify all pre-existing routes (`/`, `/version`, `/ota`) plus the new ones (`/telemetry`, `/stop`, `/suggest-gains`, `/profile` = 7 total) still register after `max_uri_handlers` is raised.
- **On-hardware**: the first supervised step test is the integration test. Confirm the booted-build SHA matches the build (stale-flash gotcha). Validate `/telemetry` holds steady under live SSR switching and with the LCD running. Exercise a stop→immediate-restart to confirm the browser starts a fresh trace on the new `run_id`.

## 10. Open implementation questions (resolve during planning)

- Whether step-test parameters travel in the `cmd_msg_t` payload union or module statics (small; payload union is cleaner if it fits).
- Whether `CMD_SUGGEST_PID` is a distinct command or folds into the existing pending pattern with a dedicated `oven_state_t` field.
- Exact decimation of the live trace for the verification overlay vs the raw step trace (mirror the existing `PROFILE_CURVE_STEP_MS` granularity where practical).

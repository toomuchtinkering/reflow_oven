# Roadmap

What is still open on the reflow oven controller. The first complete version
is described in the [MVP design spec](superpowers/specs/2026-05-15-reflow-oven-mvp-design.md),
and everything the firmware does today is in the [README](../README.md). This
page lists what could come next and why it comes in this order.

- **Item numbers are stable and never reused.** Specs and commit messages refer
  to items by number, and a closed number is never given to new work.
- **Closed items are in [backlog_history.md](backlog_history.md):** what shipped,
  what was solved another way, and what was decided against, each with the
  reason.
- **Older documents cite items as "`v2_backlog` #N".** This file was called
  `v2_backlog.md` until October 2026. The numbers are the same.
- **Priorities were last reviewed in September 2026.** If you want to work on an
  item, open an issue first so the order and the approach can be checked.

## At a glance

| # | Item | Value | Effort | Waits for |
|---|------|-------|--------|-----------|
| 21 | Screens that can be destroyed and rebuilt | high | high | nothing; #2 and #5/#6 wait for it |
| 2 | Touch calibration on the device | high | medium | #21 |
| 5 + 6 | Named profiles, each with its own PID gains | medium | high | #21 |
| 12 | Measured loop timing in the PID | low | low | only if timing jitter is ever observed |

---

## Why memory decides the order

The controller is an ESP32 without external RAM (PSRAM), and the order above
follows from its limits, all measured on the current firmware. Every item says
what it costs against them; an item that cannot say so is not ready to start.

- **Internal RAM is the tightest resource.** The user interface is built with
  [LVGL](https://lvgl.io), which allocates every on-screen object from its own
  fixed pool of 80 KB (`CONFIG_LV_MEM_SIZE_KILOBYTES` in `sdkconfig.defaults`).
  The pool rests at 57% and peaks at 89%, about 7.8 KB spare. The general ESP
  heap dips to 14.6 KB while the screen redraws, and at around 10 KB WiFi has
  stalled mid-transfer before. A firmware update that dies that way can leave
  a device that only a USB reflash recovers, which means opening the oven.
- **The LVGL pool and the heap are one budget.** Moving an allocation from one
  to the other only moves the risk to whichever is tighter at that moment. It
  was tried once, with the font glyph buffers, and reverted. Never grow the
  LVGL pool at the heap's expense.
- **The peak comes from drawing, not from the screens themselves.** While it
  renders, LVGL queues a short-lived draw task for every drawing operation in
  the strip of the screen being drawn. A busy screen can take the pool from 89%
  to exhausted within a single frame, much faster than the once-a-second memory
  sampling can see. The display is therefore drawn in strips of 24 lines (see
  `src/ui/ui_task.c`), and fewer objects per strip is what helps.
- **Flash is full.** The 4 MB chip holds two firmware slots of 1.94 MB each,
  which over-the-air updates with rollback need, and the second slot ends
  exactly at the end of the chip (`partitions.csv`). Any change to the
  partition table needs a USB reflash.
- **Settings storage (NVS) is a single 24 KB partition**, shared by the WiFi
  credentials, the OTA token, the reflow profile, the PID gains and the drying
  presets. It holds settings, not logs or temperature traces.

The practical rule: a new screen, or a widget that is always present, costs LVGL
pool memory. Until #21 is done, anything that adds a screen waits for it.

---

## Prerequisite

### 21. Screens that can be destroyed and rebuilt *(includes the former #14)*

**What.** Build each screen when it is opened and delete it when it is left,
instead of keeping every screen in memory.

**Why.** Today the four tabs (Home, Profile, Dry, Settings) are built at boot,
and each Settings page is built the first time it is opened. After that they
are only ever hidden. The only parts of the UI that are ever deleted are the
keypad pop-up and the WiFi password page. That keeps the LVGL pool high: in
September 2026 the UI froze several times because the pool ran out during a
redraw. A lighter keypad, a larger pool and the 24-line draw strip fixed the
freezes, but left no room for another screen.

**Cost and dependencies.** It gives memory back rather than spending it, and it
touches about ten UI modules. #2 and #5/#6 wait for it, and so will any future
screen. Do it when a new screen is added, or when the pool peak is seen above
about 90%.

**Watch out for:**
- Every screen module keeps its widgets in file-level `lv_obj_t *` variables,
  and `ui_refresh_timer` in `src/ui/ui_task.c` calls into all of them four
  times a second. Once a screen can be destroyed, each of those variables is a
  potential dangling pointer, in about ten modules, plus the page stack behind
  `settings_push_subscreen`.
- The "saved" confirmation timers (the former #14). `screen_profile.c` creates
  its timer without keeping the handle, so the timer can fire after its label
  is gone. `screen_pid.c` already keeps the handle. Teardown has to delete the
  timer, and the timer callback has to check that its label still exists.
- A missed guard turns an LVGL memory assert, which the firmware records and
  recovers from by rebooting, into a silent use-after-free. On a device that
  switches mains power, that is strictly worse.
- It should also fix a known leak: every time Settings → Network → Change is
  opened, the WiFi scan page is built again and the previous one is never
  deleted (§4.2 of the [OTA design](superpowers/specs/2026-05-25-ota-firmware-update-design.md)).
- Do not grow the LVGL pool from the heap to make room instead; see
  [above](#why-memory-decides-the-order).

---

## Features

### 2. Touch calibration on the device

**What.** A Settings → Touch page that runs the same 5-point calibration as the
bring-up firmware [`tools/bringup/08_touch_cal/`](../tools/bringup/08_touch_cal/)
and stores the resulting six coefficients in NVS. The math already exists in
`src/drivers/touch_cal_math.c`.

**Why.** Today the coefficients are the `TOUCH_CAL_*` constants in
[`include/config.h`](../include/config.h). To measure them you flash the
bring-up firmware, read its serial output, paste the numbers into `config.h`
and flash the main firmware again. Once the electronics are inside the oven,
that means opening it. This item deliberately breaks the rule that every
tunable lives in `config.h`, because these values are measured on each unit,
not chosen at design time.

**Cost and dependencies.** One new screen, so it waits for #21.

### 5 + 6. Named profiles, each with its own PID gains

**What.** Several named reflow profiles instead of one, each with its own PID
gains. Built as one project: it is a single change to the NVS storage format,
and once there are several profiles, tuning each one is the first thing you
want.

The UI was sketched in May 2026:
- On Home, the active profile's name as a chip with a ▼, disabled during a run.
- On the Profile tab, a `Profile: <name>` header row with a ⋯ menu to manage
  profiles.
- One shared pop-up list for picking a profile.
- The active profile applies everywhere: Home, Profile and Settings → PID all
  follow it, and Settings → PID always edits the active profile's gains.
- No extra tab: the tab bar already holds four tabs on a 320 px wide screen.

**Cost and dependencies.** Waits for #21, and needs new numbers first. It was
sized in May 2026, and two of its assumptions have moved since:
- The cap of 16 profiles was chosen "to bound NVS use" without a number behind
  it. NVS now also holds the WiFi credentials, the OTA token and the drying
  presets, so derive the cap from the space that is actually free.
- The picker is a new screen in all but name: a scrolling pop-up list with a
  menu on each row, opened while Home or Profile is still in memory. It is the
  largest single addition to the LVGL pool on this page, and it should be the
  first user of destroyable screens (#21), not something built around them.

**Watch out for:** feedforward, added in July 2026, weakened the case for #6 on
its own. Its holding-power term `k_hold` covers the steady state, so separate
gains per profile matter less between materials than they used to. Still worth
doing together with #5, no longer worth doing alone.

---

## Only if needed

### 12. Measured loop timing in the PID

**What.** Give the PID the measured time since the last control step instead of
the nominal 0.25 s (`CONTROL_TICK_MS / 1000` in `src/control/control_task.c`).
A few lines of code.

**When.** Only if scheduling jitter shows up. It was expected once WiFi arrived,
but WiFi, the web server and 12-hour drying runs are all in place and no jitter
has been seen. With those in place it can now be checked on real runs rather
than assumed.

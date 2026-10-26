#include "control/control_task.h"
#include "control/dry_setpoint.h"
#include "control/pid.h"
#include "control/profile_curve.h"
#include "control/reflow_sm.h"
#include "drivers/thermocouple.h"
#include "drivers/tc_filter.h"
#include "drivers/ssr.h"
#include "storage/nvs_store.h"
#include "audio/audio_events.h"
#include "audio/audio_task.h"
#include "diag/crash_log.h"
#include "ota/boot_status.h"
#include "ota/ota_validator.h"
#include "ui/ui_task.h"       /* ui_task_heartbeat — UI liveness supervision */
#include "ui_supervisor.h"
#include "config.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>

static const char *TAG = "control";

/* Heap-allocated: profile_curve_t (≈12 KB) in static BSS would overflow
 * the ESP32's ~180 KB static DRAM segment when the WiFi stack is linked.
 * CMD_APPLY_PROFILE only runs in MODE_IDLE (no concurrent PID access), so
 * a single buffer suffices — no double-buffering needed. */
static profile_curve_t *s_curve;
static profile_t    s_profile;
static pid_gains_t  s_pid_gains;
static pid_state_t  s_pid;
static sm_state_t   s_sm;

static QueueHandle_t s_cmd_queue;
static oven_state_t  *s_pub;
static int64_t       s_last_tc_ok_us;

/* Cold-junction (electronics) temperature. thermocouple_read writes it on
 * every successful SPI transfer — including TC-fault frames — so this static
 * naturally retains the last value across transport failures. NAN until the
 * first read (the UI shows "--"). */
static float s_cj_c = NAN;

/* Median filter over successive TC reads (one per tick). Kills the
 * single-sample noise spikes seen in test runs (±5 °C, SSR coupling)
 * before they reach the safety gates, the SM and the PID. Reset on any
 * failed read so pre-fault samples never blend with post-fault ones. */
static tc_filter_t s_tc_filter;

/* Step-test parameters latched from CMD_START_STEP_TEST, consumed by the
 * SM_EVENT_CMD_START_STEP_TEST handler in the tick body. */
static steptest_params_t s_step_params;
/* Drying parameters latched from CMD_START_DRY, consumed by the
 * SM_EVENT_CMD_START_DRY handler in the tick body. */
static dry_params_t s_dry_params;
/* Bumped on each run start (step test, dry AND reflow); published as run_id (§3.1). */
static uint32_t      s_run_id;

/* OTA reservation handshake (finding #13). control_begin_ota posts CMD_BEGIN_OTA
 * and waits on s_ota_ack for the control task's decision in s_ota_granted.
 * s_begin_ota_lock serializes concurrent callers (httpd OTA upload vs. the
 * About-screen slot switch): both share the one ack semaphore and grant flag,
 * so an unserialized pair could consume each other's ack and misread the
 * grant — leaking a reservation nobody owns (review M1). */
static SemaphoreHandle_t s_ota_ack = NULL;
static SemaphoreHandle_t s_begin_ota_lock = NULL;
static volatile bool     s_ota_granted = false;

static audio_state_snapshot_t s_prev_audio = {
    .mode = MODE_IDLE, .stage = STAGE_NONE, .fault_flags = 0,
    .dry_end_reason = DRY_END_NONE,
};

/* Publish a full snapshot under one mutex acquisition. Pass `temp_c` only
 * when the sensor read succeeded this tick; pass NULL to retain the
 * previously-published value (the UI keeps showing the last known temp
 * across a fault). `profile_applied_seq` / `pid_applied_seq` are NOT
 * touched here — they're bumped only by apply_non_sm_command after a
 * successful NVS save. */
static void publish(uint8_t duty, float setpoint, const float *temp_c) {
    xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
    if (temp_c) s_pub->temp_c = *temp_c;
    s_pub->cj_temp_c    = s_cj_c;
    s_pub->setpoint_c   = setpoint;
    s_pub->stage        = s_sm.stage;
    s_pub->elapsed_ms   = s_sm.elapsed_ms;
    s_pub->duty_percent = duty;
    s_pub->fault_flags  = s_sm.fault_flags;
    s_pub->mode         = s_sm.mode;
    s_pub->run_id          = s_run_id;
    s_pub->step_active     = (s_sm.mode == MODE_STEP_TEST);
    s_pub->step_duty       = s_sm.step_duty;
    s_pub->step_ceiling_c  = s_sm.step_ceiling_c;
    s_pub->step_max_ms     = s_sm.step_max_ms;
    s_pub->step_end_reason = (uint8_t)s_sm.step_end_reason;
    s_pub->dry_active          = (s_sm.mode == MODE_DRYING);
    s_pub->dry_stage           = (uint8_t)s_sm.dry_stage;
    s_pub->dry_target_c        = s_sm.dry_target_c;
    s_pub->dry_hold_ms         = s_sm.dry_hold_ms;
    s_pub->dry_hold_elapsed_ms = s_sm.dry_hold_elapsed_ms;
    s_pub->dry_end_reason      = (uint8_t)s_sm.dry_end_reason;
    s_pub->curve        = s_curve->samples;
    s_pub->curve_len    = s_curve->length;
    memcpy(s_pub->phase_end_ms, s_curve->phase_end_ms,
           sizeof(s_pub->phase_end_ms));
    xSemaphoreGive(s_pub->mutex);
}

static void publish_and_detect(uint8_t duty, float setpoint, const float *temp_c) {
    publish(duty, setpoint, temp_c);

    /* Detect & post audio events. Spec 2026-05-23 §3.4.
     * Drop-on-full (audio_post_event uses timeout 0): audio is non-critical. */
    audio_state_snapshot_t curr = {
        .mode = s_sm.mode, .stage = s_sm.stage, .fault_flags = s_sm.fault_flags,
        .dry_end_reason = s_sm.dry_end_reason,
    };
    audio_event_t evts[4];
    size_t n_evt = audio_detect_events(s_prev_audio, curr, evts);
    for (size_t i = 0; i < n_evt; i++) audio_post_event(evts[i]);
    s_prev_audio = curr;
}

/* Mirror the active gains + captured ambient ref into the published state so
 * GET /profile can report them (self-describing run archives, spec §5).
 * Called on the three mutation points: task start, CMD_APPLY_PID, run-start
 * pid_reset. */
static void publish_pid_info(void) {
    xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
    s_pub->pid_active_gains  = s_pid_gains;
    s_pub->pid_ambient_ref_c = s_pid.ambient_ref;
    xSemaphoreGive(s_pub->mutex);
}

/* Acceptance rules per spec §5.1 step 5:
 *   - MODE_FAULT  accepts only ACK_FAULT (and the implicit fault-clearing).
 *   - APPLY_PROFILE only lands in MODE_IDLE — writing the curve mid-run
 *     would tear the active trace, and during MODE_FAULT we want the UI
 *     to keep showing the curve that was running when the fault tripped.
 *   - APPLY_PID is allowed in IDLE and RUNNING (live-tune) but rejected
 *     in FAULT.
 */
static void apply_non_sm_command(const cmd_msg_t *msg) {
    switch (msg->type) {
        case CMD_APPLY_PROFILE: {
            if (s_sm.mode != MODE_IDLE) break;
            profile_t candidate = msg->payload.profile;
            if (!profile_validate(&candidate)) break;
            /* No concurrent PID/setpoint look-up touches s_curve here
             * (APPLY_PROFILE is IDLE-only, all in this task) — but the UI task
             * memcpys s_pub->curve under the mutex and would read a torn,
             * half-written array as profile_curve_generate fills it (finding
             * #26). Publish curve_len = 0 first so the UI skips the copy for the
             * brief generate window, then republish the finished curve. */
            xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
            s_pub->curve_len = 0;
            xSemaphoreGive(s_pub->mutex);
            /* Display curve uses the nominal start; the run-start edge below
             * regenerates it from the measured temperature before the run. */
            if (!profile_curve_generate(&candidate, PROFILE_START_DEFAULT_C, s_curve)) break;
            s_profile = candidate;
            /* §7.6 / §8: persist after validate+apply, then ACK the UI
             * by bumping the seq counter. The seq bump is the signal
             * the UI uses to fire its "✓ saved" toast — so it's skipped
             * when the NVS write failed (the profile is still applied
             * for this session, just not persisted). */
            bool saved = nvs_store_save_profile(&candidate);
            xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
            s_pub->curve     = s_curve->samples;
            s_pub->curve_len = s_curve->length;
            memcpy(s_pub->phase_end_ms, s_curve->phase_end_ms, sizeof(s_pub->phase_end_ms));
            if (saved) s_pub->profile_applied_seq++;
            xSemaphoreGive(s_pub->mutex);
            break;
        }
        case CMD_APPLY_PID:
            if (s_sm.mode == MODE_FAULT) break;
            if (!pid_validate(&msg->payload.pid)) break;
            s_pid_gains = msg->payload.pid;
            pid_set_gains(&s_pid, &s_pid_gains);
            publish_pid_info();
            /* §7.4 / §8: persist + ACK only when the control loop is
             * idle. nvs_set_blob + nvs_commit can stall this tick by
             * tens to hundreds of ms (flash erase / program). Doing
             * that mid-run would delay the PID step and break the
             * fixed-dt assumption in control_task (dt = CONTROL_TICK_MS).
             * Live-tuning in MODE_RUNNING still APPLIES the gains
             * immediately (pid_set_gains above) — only the NVS write
             * defers until the operator commits while idle. The UI's
             * "saved" toast won't fire on RUNNING commits, which is
             * the expected behaviour per spec §7.4 amendment. */
            {
                bool saved = (s_sm.mode == MODE_IDLE)
                          && nvs_store_save_pid(&s_pid_gains);
                xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
                /* Applying consumes any pending browser suggestion.
                 * screen_pid_tuning's confirm dialog relies on this flag
                 * dropping after OK — leaving it set re-pops the prompt
                 * on every UI tick until the user hits Cancel. */
                s_pub->pending_pid_valid = false;
                if (saved) s_pub->pid_applied_seq++;
                xSemaphoreGive(s_pub->mutex);
            }
            break;
        case CMD_BEGIN_OTA:
            /* Reserve only if genuinely idle and not already reserved. Decided
             * here on the control task, before this tick's sm_step applies any
             * queued START, so the reservation and START handling can't race
             * (finding #13). */
            s_ota_granted = (s_sm.mode == MODE_IDLE && !s_sm.ota_reserved);
            if (s_ota_granted) s_sm.ota_reserved = true;
            if (s_ota_ack) xSemaphoreGive(s_ota_ack);
            break;
        case CMD_END_OTA:
            s_sm.ota_reserved = false;
            break;
        case CMD_SUGGEST_PID:
            /* Hold a browser-suggested gain set as pending; the touchscreen
             * confirms it. Validate now so a bad suggestion never reaches the
             * confirm prompt. Does NOT apply or save here. The published
             * oven_state_t.pending_pid IS the single source of truth — the UI
             * reads it from the snapshot and carries its own copy through the
             * confirm dialog, so no separate control-task-side copy is needed
             * (review finding F19). */
            if (pid_validate(&msg->payload.pid)) {
                xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
                s_pub->pending_pid = msg->payload.pid;
                s_pub->pending_pid_valid = true;
                xSemaphoreGive(s_pub->mutex);
            }
            break;
        case CMD_DISMISS_PID:
            xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
            s_pub->pending_pid_valid = false;
            xSemaphoreGive(s_pub->mutex);
            break;
        default: break;
    }
}

/* Per spec §5.1 step 5 ("Drain cmd_queue"), pull every queued message in
 * this tick. APPLY_* messages are applied in arrival order. SM events
 * (START / STOP / ACK_FAULT) are batched: only the most recent one is
 * forwarded to sm_step (last-button-press wins). Returns the SM event to
 * pass to sm_step — SM_EVENT_NONE if no SM-affecting command was queued.
 */
static sm_event_t drain_commands(void) {
    sm_event_t latest = SM_EVENT_NONE;
    cmd_msg_t msg;
    while (xQueueReceive(s_cmd_queue, &msg, 0) == pdTRUE) {
        switch (msg.type) {
            case CMD_START:         latest = SM_EVENT_CMD_START;         break;
            case CMD_STOP:          latest = SM_EVENT_CMD_STOP;          break;
            case CMD_ACK_FAULT:     latest = SM_EVENT_CMD_ACK_FAULT;     break;
            case CMD_CLEAR_PENDING: latest = SM_EVENT_CMD_CLEAR_PENDING; break;
            case CMD_START_STEP_TEST:
                /* Stash params for the tick body; signal via a dedicated SM event. */
                s_step_params = msg.payload.steptest;
                latest = SM_EVENT_CMD_START_STEP_TEST;
                break;
            case CMD_START_DRY:
                /* Same pattern as the step test: stash params, signal by event. */
                s_dry_params = msg.payload.dry;
                latest = SM_EVENT_CMD_START_DRY;
                break;
            default:                apply_non_sm_command(&msg);          break;
        }
    }
    return latest;
}

/* UI-supervisor state (see UI_STALL_TIMEOUT_MS in config.h). Owned solely by
 * control_task; not shared. */
static uint32_t s_ui_beat;
static uint32_t s_ui_beat_ms;
static bool     s_ui_armed;

static void control_task(void *arg) {
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    nvs_store_load_profile(&s_profile);
    nvs_store_load_pid(&s_pid_gains);
    /* Nominal display curve until the first run captures the real temperature. */
    profile_curve_generate(&s_profile, PROFILE_START_DEFAULT_C, s_curve);

    /* Seq counters start at 0 (oven_state_t is static-zero'd in main).
     * They're bumped on every successful APPLY_*-then-NVS-save below;
     * the UI compares against its remembered baseline. */

    pid_init(&s_pid, &s_pid_gains);
    publish_pid_info();
    sm_init(&s_sm);
    tc_filter_reset(&s_tc_filter);
    /* Block START on a pending-verify boot whether self-tests passed
     * (confirmable) or failed (rollback-only) — finding #2. */
    switch (boot_status_validation()) {
        case BOOT_VALIDATION_PENDING:
            s_sm.mode = MODE_PENDING_VALIDATION;
            s_sm.validation = SM_VALIDATION_PENDING;
            break;
        case BOOT_VALIDATION_FAILED:
            s_sm.mode = MODE_PENDING_VALIDATION;
            s_sm.validation = SM_VALIDATION_FAILED;
            break;
        case BOOT_VALIDATION_NONE:
            break;
    }

    s_last_tc_ok_us = esp_timer_get_time();
    TickType_t next = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(CONTROL_TICK_MS);
    const float dt = CONTROL_TICK_MS / 1000.0f;

    bool pid_active_prev = false;

    while (1) {
        vTaskDelayUntil(&next, period);

        /* === 0. UI liveness. Placed at the top of the loop so it also runs on
         * the fault paths that `goto feed_wdt` past the rest of the body: a
         * dead panel matters most precisely when a fault is being displayed.
         *
         * Rebooting is the fail-safe response, not a drastic one. A wedged UI
         * leaves the operator with no STOP button while this task carries on
         * switching the SSR; esp_restart runs the registered shutdown handler,
         * which drives the SSR low, and the external pull-down holds it there
         * until firmware re-initialises the GPIO (§6.1 items 5-6). Losing a
         * long drying run to a spurious reboot is the accepted cost. */
        uint32_t ui_beat = ui_task_heartbeat();
        uint32_t now_ms  = (uint32_t)(esp_timer_get_time() / 1000);
        if (ui_beat != s_ui_beat || !s_ui_armed) {
            s_ui_beat    = ui_beat;
            s_ui_beat_ms = now_ms;
            s_ui_armed   = (ui_beat != 0);   /* arm only once the UI has ticked */
        }
        if (ui_supervisor_stalled(s_ui_armed, now_ms, s_ui_beat_ms, UI_STALL_TIMEOUT_MS)) {
            uint32_t stalled = now_ms - s_ui_beat_ms;
            ESP_LOGE(TAG, "UI heartbeat frozen for %u ms - rebooting", (unsigned)stalled);
            ssr_force_off();                 /* explicit; do not rely on shutdown order */
            crash_log_record_ui_stall(now_ms, stalled);
            esp_restart();
        }

        /* === 1. Read sensor === */
        float temp_c = 0.0f;
        uint8_t fault = 0;
        uint32_t age_dummy = 0;
        bool tc_ok = thermocouple_read(&temp_c, &s_cj_c, &fault, &age_dummy);
        if (tc_ok) temp_c = tc_filter_push(&s_tc_filter, temp_c);
        else       tc_filter_reset(&s_tc_filter);
        int64_t now_us = esp_timer_get_time();
        uint32_t age_ms = (uint32_t)((now_us - s_last_tc_ok_us) / 1000);
        if (tc_ok) {
            s_last_tc_ok_us = now_us;
            age_ms = 0;
        }

        /* === 2-4. Safety gates ===
         * Each gate calls publish() once with a single mutex grab, passing
         * &temp_c iff this tick's read succeeded (so the UI keeps the
         * last-known temperature across faults; see oven_state.h note).
         * WDT feed (step 8) still happens via `goto feed_wdt` before
         * looping back — we don't want a continued safety-gate trip
         * loop to be silently mistaken for a hung task.
         *
         * Each gate also restores the reflow PWM window after forcing the SSR
         * off: sm_trip_fault may be tripping out of MODE_DRYING, which runs
         * the much finer DRY_SSR_QUANTA window, and the SM has no way to reach
         * the SSR itself. Idempotent in every other mode.
         */
        const float *temp_ptr = tc_ok ? &temp_c : NULL;
        /* Gate ticks still drain the command queue (review M1): a BEGIN_OTA
         * must be answered (denied) promptly instead of sitting queued for the
         * whole fault — its caller times out after ~1 s and the late grant
         * would leak a reservation nobody owns. Non-SM commands keep their own
         * mode guards; the returned SM event is deliberately dropped — a
         * START/ACK during an actively-faulting tick is futile by design (the
         * fault re-trips next tick anyway). */
        if (fault) {
            sm_trip_fault(&s_sm, fault);
            ssr_force_off();
            ssr_set_window_quanta(SSR_QUANTA_PER_WINDOW);
            (void)drain_commands();
            publish_and_detect(0, 0.0f, temp_ptr);
            goto feed_wdt;
        }
        if (age_ms > SENSOR_MAX_AGE_MS) {
            /* Defense-in-depth only: thermocouple_read flags every failure
             * (SPI transport errors surface as FAULT_TC_OPEN above), so this
             * gate is unreachable today. It stays as a backstop in case the
             * read path ever learns to tolerate transient failures (review L2). */
            sm_trip_fault(&s_sm, FAULT_TC_STALE);
            ssr_force_off();
            ssr_set_window_quanta(SSR_QUANTA_PER_WINDOW);
            (void)drain_commands();
            publish_and_detect(0, 0.0f, temp_ptr);
            goto feed_wdt;
        }
        if (temp_c > OVER_TEMP_LIMIT_C) {
            sm_trip_fault(&s_sm, FAULT_OVER_TEMP);
            ssr_force_off();
            ssr_set_window_quanta(SSR_QUANTA_PER_WINDOW);
            (void)drain_commands();
            publish_and_detect(0, 0.0f, temp_ptr);
            goto feed_wdt;
        }
        /* Electronics over-temperature. The oven vents through the
         * electronics bay and no archived run ever reached thermal
         * equilibrium — the 40 min hold staircase hit 60 C still climbing.
         * A 16 h dry is an uncharacterised sustained load, so this guards
         * every mode, not just drying. Gated on tc_ok, not on the absence of
         * a fault above: s_cj_c is a retained static (NAN before the first
         * transfer, last-known value across failures — see its declaration
         * at the top of this file), so the comparison has to be tied to a
         * sample taken THIS tick. Equivalent today (a failed read always sets
         * a fault flag), but it stays correct if the stale gate above ever
         * becomes reachable, i.e. if the read path learns to tolerate
         * transient failures. */
        if (tc_ok && s_cj_c > CJ_LIMIT_C) {
            sm_trip_fault(&s_sm, FAULT_ELECTRONICS_HOT);
            ssr_force_off();
            ssr_set_window_quanta(SSR_QUANTA_PER_WINDOW);
            (void)drain_commands();
            publish_and_detect(0, 0.0f, temp_ptr);
            goto feed_wdt;
        }

        /* === 5. Drain cmd_queue (all messages, per spec §5.1 step 5).
         * APPLY_* are applied inside the drain in arrival order; the
         * most recent SM event wins this tick. */
        sm_event_t ev = drain_commands();

        /* Step-test start is a distinct entry (touchscreen-only). Handle it
         * before sm_step so the dedicated event doesn't fall through reflow
         * logic. run_id bumps on every run start (step test AND reflow). */
        if (ev == SM_EVENT_CMD_START_STEP_TEST) {
            if (sm_start_step_test(&s_sm, s_step_params.duty,
                                   s_step_params.ceiling_c, s_step_params.max_ms)) {
                s_run_id++;
            }
            ev = SM_EVENT_NONE;   /* consumed */
        }

        /* Drying start is a distinct entry (touchscreen-only), handled before
         * sm_step so the dedicated event doesn't fall through reflow logic. */
        if (ev == SM_EVENT_CMD_START_DRY) {
            if (sm_start_dry(&s_sm, s_dry_params.target_c,
                             s_dry_params.hold_ms, temp_c)) {
                pid_reset(&s_pid, temp_c);   /* captures ambient_ref for the ff hold term */
                publish_pid_info();
                ssr_set_window_quanta(DRY_SSR_QUANTA);
                s_run_id++;
            } else {
                /* Never swallow a refused start. The user taps START and
                 * nothing happens; without this line there is no other trace
                 * anywhere. sm_start_dry refuses on any of four conditions, so
                 * log the state that decided it. The target logged is the
                 * REQUESTED value — sm_start_dry clamps its own copy to
                 * [DRY_TEMP_MIN_C, DRY_TEMP_MAX_C] before applying the
                 * start-temperature gate. */
                ESP_LOGW(TAG,
                         "dry start refused: mode=%d ota_reserved=%d validation=%d "
                         "temp=%.1f requested_target=%.1f",
                         (int)s_sm.mode, (int)s_sm.ota_reserved,
                         (int)s_sm.validation, (double)temp_c,
                         (double)s_dry_params.target_c);
            }
            ev = SM_EVENT_NONE;   /* consumed */
        }

        /* A STOP while drying is routed here, before sm_step, because only the
         * control task owns the SSR. sm_step's own MODE_DRYING branch reaches
         * the identical state (sm_dry_stop -> MODE_IDLE, STAGE_NONE,
         * DRY_STAGE_NONE, DRY_END_MANUAL_STOP) but cannot force the output off
         * nor restore the PWM window. Consuming the event here makes that
         * branch unreachable for this path; it stays as defence in depth. */
        if (ev == SM_EVENT_CMD_STOP && s_sm.mode == MODE_DRYING) {
            sm_dry_stop(&s_sm);
            ssr_force_off();
            ssr_set_window_quanta(SSR_QUANTA_PER_WINDOW);
            ev = SM_EVENT_NONE;   /* consumed */
        }

        /* === 6. Step the SM. Compute PID if requested. === */
        sm_validation_t val_before = s_sm.validation;
        bool pid_active = sm_step(&s_sm, ev, temp_c, s_curve);
        /* PID integrator hygiene (finding #27): a STOP→START cycle drives
         * pid_active false→true, which triggers pid_reset below — so the
         * integrator never carries state across runs. No separate reset on
         * mode→IDLE is needed; this edge is the single reset point. */
        /* Commit the partition (cancel rollback) only on the genuine
         * PENDING→cleared transition, i.e. when the user actually confirmed
         * while in PENDING — never from a fault path (finding #4). */
        if (val_before == SM_VALIDATION_PENDING && s_sm.validation == SM_VALIDATION_NONE)
            ota_validator_mark_valid();
        if (pid_active && !pid_active_prev) {
            /* Regenerate the setpoint curve so heat-up 1 starts at the measured
             * temperature: setpoint(0) == temp, no standing error to wind the
             * integrator up over ramp 1. curve_len=0 first so the UI skips a
             * torn read while generate rewrites s_curve->samples (same guard as
             * APPLY_PROFILE); publish() at the end of this tick republishes the
             * finished curve + phase_end_ms. Done before the setpoint lookup so
             * this very tick already tracks the new curve. */
            xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
            s_pub->curve_len = 0;
            xSemaphoreGive(s_pub->mutex);
            profile_curve_generate(&s_profile, temp_c, s_curve);
            pid_reset(&s_pid, temp_c);
            publish_pid_info();   /* ambient_ref just became current */
            s_run_id++;   /* a fresh reflow run started (§3.1) */
        }
        pid_active_prev = pid_active;

        float setpoint = 0.0f;
        uint8_t duty = 0;
        if (pid_active) {
            setpoint = profile_curve_setpoint(s_curve, s_sm.elapsed_ms);
            float out = pid_step(&s_pid, setpoint,
                                 profile_curve_rate(s_curve, s_sm.elapsed_ms),
                                 temp_c, dt, PID_OUTPUT_MAX);
            duty = (uint8_t)out;
            ssr_tick((uint32_t)(esp_timer_get_time() / 1000), duty);
        } else if (s_sm.mode == MODE_STEP_TEST) {
            /* Open-loop fixed-duty drive. Safety gates above already ran and
             * would have forced off + exited on any fault. sm_step_test_tick
             * advances time and applies the ceiling/duration aborts. */
            if (sm_step_test_tick(&s_sm, temp_c)) {
                duty = s_sm.step_duty;
                ssr_tick((uint32_t)(esp_timer_get_time() / 1000), duty);
            } else {
                ssr_force_off();   /* test just ended this tick */
            }
        } else if (s_sm.mode == MODE_DRYING) {
            /* Mode-scoped over-temperature, checked before anything drives the
             * SSR. The global OVER_TEMP_LIMIT_C (260 C) is no guard for a 45 C
             * dry: a welded SSR would fuse the spool long before it trips.
             * Evaluated against the TARGET, not the ramping setpoint, so the
             * ramp cannot nuisance-trip it. temp_ptr is &temp_c at this point (the
             * gates above guarantee tc_ok); it is passed instead of a literal
             * &temp_c so this fault exit keeps behaving like the other four if
             * a gate is ever relaxed to let a failed read through. */
            if (temp_c > s_sm.dry_target_c + DRY_OVER_TEMP_MARGIN_C) {
                sm_trip_fault(&s_sm, FAULT_OVER_TEMP);
                ssr_force_off();
                ssr_set_window_quanta(SSR_QUANTA_PER_WINDOW);
                publish_and_detect(0, 0.0f, temp_ptr);
                goto feed_wdt;
            }
            /* Closed-loop drive, unlike the open-loop step test. The duty
             * ceiling is passed INTO pid_step so its anti-windup sees the real
             * saturation point; capping the return value here instead would
             * wind the integrator up invisibly. */
            float dry_sp = 0.0f;
            if (sm_dry_tick(&s_sm, temp_c, &dry_sp)) {
                setpoint = dry_sp;
                float rate = dry_setpoint_rate(s_sm.dry_start_temp_c,
                                               s_sm.dry_target_c,
                                               s_sm.elapsed_ms);
                float out = pid_step(&s_pid, setpoint, rate, temp_c, dt,
                                     DRY_DUTY_MAX_PERCENT);
                duty = (uint8_t)out;   /* pid_step clamps to [0, out_max]; never negative */
                ssr_tick((uint32_t)(esp_timer_get_time() / 1000), duty);
            } else {
                /* Run ended this tick — the SM is already back in MODE_IDLE.
                 * Reachable for DRY_END_COMPLETE and DRY_END_HEATUP_TIMEOUT
                 * only: a manual STOP is consumed before sm_step above, and a
                 * fault exits via a gate or the guard just above. */
                ssr_force_off();
                ssr_set_window_quanta(SSR_QUANTA_PER_WINDOW);
            }
        } else {
            ssr_force_off();
        }

        /* === 7. Publish. tc_ok was true at this point (no fault gate
         * tripped) so pass &temp_c. */
        publish_and_detect(duty, setpoint, &temp_c);

        /* === 8. Feed the task watchdog only after the iteration body
         * has run to completion (spec §5.1 step 8). A hang anywhere
         * above will leave the WDT unfed and panic after
         * CONTROL_TASK_WDT_S. */
    feed_wdt:
        esp_task_wdt_reset();
    }
}

bool control_begin_ota(void) {
    if (!s_ota_ack || !s_begin_ota_lock) return false;   /* control task not up yet */
    xSemaphoreTake(s_begin_ota_lock, portMAX_DELAY);     /* one handshake at a time (M1) */
    /* Discard any stale ack: a previous BEGIN_OTA could have been drained
     * after its caller already timed out, leaving the semaphore given with no
     * waiter. Clearing it first guarantees we wait for THIS request's own
     * response. */
    while (xSemaphoreTake(s_ota_ack, 0) == pdTRUE) { }
    cmd_msg_t m = { .type = CMD_BEGIN_OTA };
    bool granted = false;
    if (xQueueSend(s_cmd_queue, &m, pdMS_TO_TICKS(500)) == pdTRUE) {
        /* The control task answers within one tick — even while faulted (the
         * safety gates drain commands too). 4 ticks of margin. On timeout the
         * control task is wedged and the task WDT reboots us within
         * CONTROL_TASK_WDT_S anyway; queue an END_OTA so a late grant can't
         * outlive its caller in the unlikely interim. */
        if (xSemaphoreTake(s_ota_ack, pdMS_TO_TICKS(CONTROL_TICK_MS * 4)) == pdTRUE)
            granted = s_ota_granted;
        else
            control_end_ota();
    }
    xSemaphoreGive(s_begin_ota_lock);
    return granted;
}

void control_end_ota(void) {
    cmd_msg_t m = { .type = CMD_END_OTA };
    xQueueSend(s_cmd_queue, &m, pdMS_TO_TICKS(500));
}

bool control_post_cmd(const cmd_msg_t *msg) {
    if (!s_cmd_queue || !msg) return false;
    return xQueueSend(s_cmd_queue, msg, 0) == pdTRUE;
}

void control_get_snapshot(oven_state_t *out) {
    if (!out) return;
    /* Zero first so a call before control_task_create has published anything
     * (HTTP server can come up on WiFi-connect before the control task is
     * created) yields a valid zeroed/idle snapshot rather than stack garbage. */
    memset(out, 0, sizeof(*out));
    if (!s_pub) return;
    xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
    *out = *s_pub;
    xSemaphoreGive(s_pub->mutex);
    out->mutex = NULL;   /* don't leak the handle to callers */
    out->curve = NULL;   /* pointer not valid outside the lock */
}

void control_get_gains(pid_gains_t *out_gains, float *out_ambient_c) {
    if (!s_pub) {
        /* Boot window before control_task_create: defaults, never garbage. */
        if (out_gains) pid_defaults(out_gains);
        if (out_ambient_c) *out_ambient_c = 0.0f;
        return;
    }
    xSemaphoreTake(s_pub->mutex, portMAX_DELAY);
    if (out_gains)     *out_gains     = s_pub->pid_active_gains;
    if (out_ambient_c) *out_ambient_c = s_pub->pid_ambient_ref_c;
    xSemaphoreGive(s_pub->mutex);
}

void control_get_curve(float *out_temps, uint16_t *io_len, uint32_t *out_phase_ms) {
    /* s_pub guard: s_curve is pre-allocated early (control_task_prealloc) but
     * s_pub->mutex only exists after control_task_create, so check both before
     * locking (same boot-window concern as control_get_snapshot). */
    if (!s_curve || !s_pub || !out_temps || !io_len) { if (io_len) *io_len = 0; return; }
    xSemaphoreTake(s_pub->mutex, portMAX_DELAY);   /* same mutex guards curve swap publication */
    uint16_t n = s_curve->length;
    if (n > *io_len) n = *io_len;
    for (uint16_t i = 0; i < n; i++) out_temps[i] = s_curve->samples[i];
    if (out_phase_ms)
        for (int s = 0; s < STAGE_COUNT_; s++) out_phase_ms[s] = s_curve->phase_end_ms[s];
    xSemaphoreGive(s_pub->mutex);
    *io_len = n;
}

void control_task_prealloc(void) {
    /* Reserve the ~12 KB curve buffer EARLY in boot. control_task_create runs
     * last (after ui_init), by which point LVGL's ~60 KB DMA frame buffers and
     * the WiFi stack have fragmented the heap — so this single contiguous
     * allocation fails there even though enough total heap remains. Grabbing it
     * while the heap is still pristine avoids that. Idempotent. */
    if (s_curve) return;
    s_curve = pvPortMalloc(sizeof(profile_curve_t));
    if (!s_curve) ESP_LOGE(TAG, "FATAL: profile_curve_t prealloc failed");
    configASSERT(s_curve);
}

void control_task_create(QueueHandle_t cmd_queue, oven_state_t *state) {
    s_cmd_queue = cmd_queue;
    s_pub = state;
    s_pub->cj_temp_c = NAN;   /* "no reading yet"; main static-zeros the struct,
                               * and 0.0 °C is a legitimate temperature */
    s_ota_ack = xSemaphoreCreateBinary();
    configASSERT(s_ota_ack);
    s_begin_ota_lock = xSemaphoreCreateMutex();
    configASSERT(s_begin_ota_lock);
    control_task_prealloc();   /* no-op if app_main already pre-allocated it */
    xTaskCreatePinnedToCore(control_task, "control", 4096, NULL, 5, NULL, 1);
}

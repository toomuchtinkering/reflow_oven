#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "oven_state.h"
#include "control/reflow_sm.h"

/* Reserve the control task's ~12 KB curve buffer. Call once, EARLY in
 * app_main (before LVGL + WiFi fragment the heap), so the contiguous
 * allocation can't fail. control_task_create() calls it too (idempotent). */
void control_task_prealloc(void);

void control_task_create(QueueHandle_t cmd_queue, oven_state_t *state);

/* OTA reservation (finding #13). control_begin_ota() asks the control task to
 * reserve the oven for an update; it returns true only if the oven was idle
 * (and not already reserved), and once reserved the control task rejects
 * CMD_START. The decision is made on the control task, serialised with START
 * handling, so there is no idle-check→reserve race. Call control_end_ota() to
 * release the reservation if the upload fails (a successful upload reboots).
 * Both are safe to call from the OTA server (httpd) task. */
bool control_begin_ota(void);
void control_end_ota(void);

/* Post a command to the control task's queue from any task (e.g. the HTTP
 * server). Non-blocking (timeout 0); returns true if enqueued. Safe because
 * the control task drains the whole queue each tick. */
bool control_post_cmd(const cmd_msg_t *msg);

/* Copy the current published oven_state_t snapshot under its mutex. Safe from
 * any task; for read-only consumers like the telemetry endpoint. */
void control_get_snapshot(oven_state_t *out);

/* Copy the active planned curve (setpoint samples at PROFILE_CURVE_STEP_MS).
 * Writes up to *io_len samples into out_temps[] and the phase-end marks into
 * out_phase_ms[STAGE_COUNT_]; sets *io_len to the actual count. Safe to call
 * from the HTTP server task. */
void control_get_curve(float *out_temps, uint16_t *io_len, uint32_t *out_phase_ms);

/* Snapshot of the active PID/ff gains and the ambient reference captured at
 * the current/last run start. Safe from any task (mutex); pre-boot-window
 * calls get defaults. */
void control_get_gains(pid_gains_t *out_gains, float *out_ambient_c);

#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "oven_state.h"

/* Initialise the LVGL UI: display + touch + ports + screens. Does NOT
 * spawn a FreeRTOS task — esp_lvgl_port owns its own task; this
 * function just sets it up and returns. Naming retained as ui_init
 * (not ui_task_create) because the implementation does not call
 * xTaskCreate. */
void ui_init(QueueHandle_t cmd_queue, oven_state_t *state);

/* Monotonic counter bumped on every UI_REFRESH_MS tick, from inside the
 * esp_lvgl_port task. control_task polls it to detect a wedged UI: the LVGL
 * task is not watchdog-subscribed (the component provides no hook), so a
 * frozen panel is otherwise invisible to the firmware. Value itself is
 * meaningless; only whether it advances matters. */
uint32_t ui_task_heartbeat(void);

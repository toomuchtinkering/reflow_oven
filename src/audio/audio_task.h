/* Audio playback task. Consumes audio_event_t from g_audio_queue and
 * plays the corresponding melody table. Spec 2026-05-23 §3.3. */
#pragma once

#include <stdbool.h>
#include "audio/audio_events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

extern QueueHandle_t g_audio_queue;       /* depth 4, audio_event_t */

void audio_task_start(void);              /* call once after audio_init */

/* Mute control. set_muted persists to NVS as a side effect.
 * is_muted is safe to call from any task. */
void audio_set_muted(bool muted);
bool audio_is_muted(void);

/* Convenience posting helper. Drop-on-full (timeout=0). Returns true on enqueue. */
bool audio_post_event(audio_event_t evt);

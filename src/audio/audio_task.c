#include "audio/audio_task.h"
#include "audio/audio_melodies.h"
#include "drivers/audio.h"
#include "storage/nvs_store.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <stdatomic.h>

static const char *TAG = "audio_task";

QueueHandle_t g_audio_queue = NULL;
static atomic_bool s_muted = false;

static void play_melody(const audio_melody_t *m) {
    /* Amp is already on (enabled in audio_init); just emit the notes. */
    for (size_t i = 0; i < m->n; i++) {
        audio_play_tone(m->notes[i].freq_hz, m->notes[i].on_ms);
        if (m->notes[i].gap_after_ms) audio_silence(m->notes[i].gap_after_ms);
    }
}

/* Returns true if FAULT_ACK arrived mid-loop and we should bail out. */
static bool fault_gap_with_peek(uint32_t ms) {
    /* vTaskDelay tick granularity is 10 ms with the default config; peek
     * every 50 ms so an ACK is responded to within ~50 ms. */
    const uint32_t slice = 50;
    uint32_t waited = 0;
    while (waited < ms) {
        audio_event_t peek_evt;
        if (xQueuePeek(g_audio_queue, &peek_evt, 0) == pdTRUE &&
            peek_evt == AUDIO_EVT_FAULT_ACK) {
            /* Drain the ACK so the next xQueueReceive doesn't redo it. */
            xQueueReceive(g_audio_queue, &peek_evt, 0);
            return true;
        }
        uint32_t step = (ms - waited < slice) ? (ms - waited) : slice;
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
    }
    return false;
}

static void play_fault_alarm(void) {
    for (int i = 0; i < AUDIO_FAULT_LOOP_COUNT; i++) {
        play_melody(&MELODY_FAULT_BURST);
        if (i == AUDIO_FAULT_LOOP_COUNT - 1) break;
        if (fault_gap_with_peek(AUDIO_FAULT_GAP_MS)) {
            ESP_LOGI(TAG, "fault ack received, stopping alarm at burst %d", i + 1);
            return;
        }
    }
    ESP_LOGI(TAG, "fault alarm self-stopped after %d bursts", AUDIO_FAULT_LOOP_COUNT);
}

static void audio_task(void *arg) {
    (void)arg;
    audio_event_t evt;
    for (;;) {
        if (xQueueReceive(g_audio_queue, &evt, portMAX_DELAY) != pdTRUE) continue;
        if (atomic_load(&s_muted)) continue;
        switch (evt) {
            case AUDIO_EVT_STAGE_TRANSITION: play_melody(&MELODY_STAGE_TRANSITION); break;
            case AUDIO_EVT_RUN_COMPLETE:     play_melody(&MELODY_RUN_COMPLETE);     break;
            case AUDIO_EVT_FAULT:            play_fault_alarm();                    break;
            case AUDIO_EVT_FAULT_ACK:        /* no-op when idle; consumed inside the loop */ break;
        }
    }
}

void audio_task_start(void) {
    g_audio_queue = xQueueCreate(4, sizeof(audio_event_t));
    atomic_store(&s_muted, nvs_store_get_mute());
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 1, NULL, 1);
    ESP_LOGI(TAG, "audio_task started, muted=%d", (int)atomic_load(&s_muted));
}

void audio_set_muted(bool muted) {
    atomic_store(&s_muted, muted);
    nvs_store_set_mute(muted);
}

bool audio_is_muted(void) {
    return atomic_load(&s_muted);
}

bool audio_post_event(audio_event_t evt) {
    if (!g_audio_queue) return false;
    return xQueueSend(g_audio_queue, &evt, 0) == pdTRUE;
}

/* tools/bringup/09_audio/src/main.c
 * UART CLI for audio driver bring-up.
 *
 * Commands (single-char unless noted):
 *   s         = stage-transition pip (80 ms @ 1200 Hz)
 *   c         = run-complete chime  (rising C5-E5-G5)
 *   f         = single fault burst  (3x 880 Hz, ~540 ms)
 *   F         = full 15-burst alarm (~30 s)
 *   m         = toggle mute (local to this bring-up; suppresses all melody output)
 *   t <freq> <ms>
 *             = arbitrary tone; e.g. "t 1000 500" -> 1 kHz / 500 ms. Newline-terminated.
 *   ?         = print help
 *
 * Bring-up checklist (spec §7.2):
 *   - Tones audible without distortion.
 *   - No click on amp enable/disable transitions.
 *   - No idle hiss when AUDIO_EN is high (amp shut down).
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/audio.h"
#include "audio/audio_melodies.h"
#include "config.h"

static const char *TAG = "09_audio";

static bool s_muted = false;

static void play_melody(const audio_melody_t *m) {
    if (s_muted) { ESP_LOGI(TAG, "(muted, skipping)"); return; }
    for (size_t i = 0; i < m->n; i++) {
        audio_play_tone(m->notes[i].freq_hz, m->notes[i].on_ms);
        if (m->notes[i].gap_after_ms) audio_silence(m->notes[i].gap_after_ms);
    }
}

static void play_tone_args(void) {
    /* "t" was already consumed; read the rest of the line for freq + ms. */
    char buf[32];
    size_t i = 0;
    while (i + 1 < sizeof(buf)) {
        int ch = getchar();
        if (ch == EOF) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (ch == '\r' || ch == '\n') break;
        buf[i++] = (char)ch;
    }
    buf[i] = '\0';

    char *endp = NULL;
    long freq = strtol(buf, &endp, 10);
    long ms   = (endp && *endp) ? strtol(endp, NULL, 10) : 0;
    if (freq < 100 || freq > 10000 || ms <= 0 || ms > 10000) {
        printf("usage: t <freq Hz, 100-10000> <ms, 1-10000>\n");
        return;
    }
    if (s_muted) { ESP_LOGI(TAG, "(muted, skipping)"); return; }
    ESP_LOGI(TAG, "tone %ld Hz / %ld ms", freq, ms);
    audio_play_tone((uint32_t)freq, (uint32_t)ms);
}

static void play_help(void) {
    printf("\nCommands:\n"
           "  s            = stage pip\n"
           "  c            = completion chime\n"
           "  f            = one fault burst\n"
           "  F            = full fault alarm (~30 s)\n"
           "  m            = toggle mute (now: %s)\n"
           "  t <Hz> <ms>  = arbitrary tone; e.g. 't 1000 500'\n"
           "  ?            = this help\n\n",
           s_muted ? "MUTED" : "unmuted");
}

void app_main(void) {
    ESP_LOGI(TAG, "audio bring-up starting");
    audio_init();
    play_help();

    while (1) {
        int c = getchar();
        if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        switch (c) {
            case 's': ESP_LOGI(TAG, "stage pip");
                      play_melody(&MELODY_STAGE_TRANSITION); break;
            case 'c': ESP_LOGI(TAG, "completion chime");
                      play_melody(&MELODY_RUN_COMPLETE);     break;
            case 'f': ESP_LOGI(TAG, "one fault burst");
                      play_melody(&MELODY_FAULT_BURST);      break;
            case 'F': ESP_LOGI(TAG, "full fault alarm");
                      for (int i = 0; i < AUDIO_FAULT_LOOP_COUNT; i++) {
                          play_melody(&MELODY_FAULT_BURST);
                          if (i != AUDIO_FAULT_LOOP_COUNT - 1) {
                              audio_silence(AUDIO_FAULT_GAP_MS);
                          }
                      }
                      break;
            case 'm': s_muted = !s_muted;
                      ESP_LOGI(TAG, "mute = %s", s_muted ? "ON" : "off");
                      break;
            case 't': play_tone_args(); break;
            case '?': case '\r': case '\n': play_help(); break;
            default: break;
        }
    }
}

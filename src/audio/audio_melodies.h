/* Compile-time melody tables. One {freq, on_ms, gap_ms} note per row.
 * Spec 2026-05-23 §3.5. */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "audio/audio_events.h"

typedef struct {
    uint16_t freq_hz;
    uint16_t on_ms;
    uint16_t gap_after_ms;
} audio_note_t;

typedef struct {
    const audio_note_t *notes;
    size_t              n;
} audio_melody_t;

/* Storage. Definitions in audio_melodies.c. */
extern const audio_note_t   STAGE_TRANSITION_NOTES[1];
extern const audio_note_t   RUN_COMPLETE_NOTES[3];
extern const audio_note_t   FAULT_BURST_NOTES[3];

extern const audio_melody_t MELODY_STAGE_TRANSITION;
extern const audio_melody_t MELODY_RUN_COMPLETE;
extern const audio_melody_t MELODY_FAULT_BURST;

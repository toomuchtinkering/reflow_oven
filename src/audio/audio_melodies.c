#include "audio/audio_melodies.h"

const audio_note_t STAGE_TRANSITION_NOTES[1] = {
    {1200, 200, 0},
};

const audio_note_t RUN_COMPLETE_NOTES[3] = {
    { 523, 200, 60 },   /* C5 */
    { 659, 200, 60 },   /* E5 */
    { 784, 250,  0 },   /* G5 — held a bit longer as a clear 'done' anchor */
};

const audio_note_t FAULT_BURST_NOTES[3] = {
    { 880, 100, 80 },
    { 880, 100, 80 },
    { 880, 100,  0 },
};

const audio_melody_t MELODY_STAGE_TRANSITION = { STAGE_TRANSITION_NOTES, 1 };
const audio_melody_t MELODY_RUN_COMPLETE     = { RUN_COMPLETE_NOTES,     3 };
const audio_melody_t MELODY_FAULT_BURST      = { FAULT_BURST_NOTES,      3 };

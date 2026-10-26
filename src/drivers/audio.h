/* Onboard FM8002E driver. DAC2 (IO26) -> amp +IN, IO4 = active-low SHUTDOWN.
 * Spec 2026-05-23 §3.2. Only the audio_task is allowed to call play/silence.
 *
 * The amp is enabled inside audio_init() and stays on for the program's
 * lifetime — the FM8002E's per-cycle SHUTDOWN transitions are audibly
 * clicky, so we trade 4.4 mA of idle current for a clean silent rest.
 */
#pragma once

#include <stdint.h>

void audio_init(void);                                   /* once at boot */
void audio_play_tone(uint32_t freq_hz, uint32_t ms);     /* blocking */
void audio_silence(uint32_t ms);                         /* blocking */

/** Audio output (minigb_apu -> CSND). MIT. */
#pragma once

#include "gb3ds.h"

#include <stdbool.h>
#include <stdint.h>

/* Included from C++ as well (the Gambatte backend is C++). */
#ifdef __cplusplus
extern "C" {
#endif

/* Returns false when the sound service could not be started; the emulator
 * keeps running silently in that case. */
bool audio_init(void);
void audio_exit(void);

/* Output rate of everything below this line: 32768 Hz, stereo, s16. */
#define GBABE_AUDIO_RATE 32768u

/* Hand the core's audio to the hardware: `frames` stereo frames at
 * GBABE_AUDIO_RATE, interleaved. Safe once audio_init() has run. */
void audio_push_frames(const int16_t *stereo, unsigned frames);

#if defined(GBABE_CORE_WALNUT)
/* Walnut-CGB only: pull one emulated frame out of minigb_apu. */
void audio_frame(void);
#endif

void audio_set_enabled(bool on);
bool audio_enabled(void);

/* Number of frames the sound FIFO is behind, for the HUD. */
unsigned audio_queue_ms(void);

#ifdef __cplusplus
}
#endif

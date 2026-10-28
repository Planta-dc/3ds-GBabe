/** Audio output (minigb_apu -> CSND). MIT. */
#pragma once

#include "gb3ds.h"

#include <stdbool.h>

/* Returns false when the sound service could not be started; the emulator
 * keeps running silently in that case. */
bool audio_init(void);
void audio_exit(void);

/* Called once per emulated frame: pulls one frame of samples from the APU and
 * hands them to the hardware. */
void audio_frame(void);

void audio_set_enabled(bool on);
bool audio_enabled(void);

/* Number of frames the sound FIFO is behind, for the HUD. */
unsigned audio_queue_ms(void);

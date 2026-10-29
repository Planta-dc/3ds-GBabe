/** Variable-rate linear resampler for the audio FIFO. MIT. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Included from C++ as well (the Gambatte backend is C++). */
#ifdef __cplusplus
extern "C" {
#endif

/* Why this exists
 * ---------------
 * Gambatte produces exactly 35112 samples per emulated frame, which is
 * 2097152 Hz at 59.7275 fps, and the decimator turns that into 548.6 samples at
 * 32768 Hz. CSND plays 32768 samples a second, every second, whatever the
 * console is doing. Those two rates agree only if the emulator is running at
 * exactly 59.7275 fps:
 *
 *     56.7 fps -> 31,100 samples/s produced against 32,768 consumed
 *              -> ~1,670 samples/s missing, the 125 ms FIFO empty in 2.5 s
 *
 * and an empty FIFO is a hole in the sound. That is what the console log calls
 * `underruns`, and it is what crackling is. (docs/PORT-NOTES.md 18 has the
 * arithmetic from the 0.3.1 log: 176 underruns in 230 s.)
 *
 * The fix is to stop treating the sample rate as sacred. This resampler sits
 * between the decimator and the FIFO and is told, once per block, how fast to
 * consume its input: 1.0 when the FIFO is where it should be, below 1.0 when it
 * is draining (more output samples per emulated frame, i.e. the audio stretched
 * by up to 4 %), above 1.0 when it is filling up. A 4 % pitch shift is
 * inaudible in Game Boy sound; a hole every two seconds is not.
 *
 * The alternative - resampling against a fixed ratio - is what the port did
 * until now, and it is why the sound quality depended on the frame rate.
 */
typedef struct
{
	int32_t step;      /* input frames per output frame, 16.16 (65536 = 1:1) */
	int32_t pos;       /* position within the current block, 16.16           */
	int16_t carry[2];  /* last frame of the previous block, for interpolation */
	bool    started;
} resamp;

#define RESAMP_UNITY 65536

/* The control law, kept out of audio.c so it can be tested as arithmetic:
 * `fill` frames queued against `target`. Returns the step to feed
 * resamp_set_step(). 65536 on target, below it when the queue is low (stretch,
 * produce more), above it when the queue is high (compress), clamped to +-4 %. */
int32_t resamp_control_step(int32_t fill, int32_t target);

void resamp_reset(resamp *r);
void resamp_set_step(resamp *r, int32_t step_q16);

/* Resample `frames` interleaved stereo frames from `in` into `out` (at most
 * `out_cap` frames). Returns the number of frames written. Block boundaries are
 * seamless: the last input frame is carried, so a step that is not an integer
 * does not click once per block. */
unsigned resamp_run(resamp *r, const int16_t *in, unsigned frames,
		    int16_t *out, unsigned out_cap);

#ifdef __cplusplus
}
#endif

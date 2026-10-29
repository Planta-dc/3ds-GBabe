/** The 2 MHz -> 32768 Hz audio decimator, in two stages. MIT. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Gambatte's PSG produces one 32-bit word per sample at 2097152 Hz, holding two
 * 16-bit signed channels (left in the low half, right in the high half on a
 * little-endian machine - see PSG::fillBuffer() in the core). A frame is 35112
 * of them, and 35112 / 2097152 = 16.74 ms, so a frame of audio is a frame.
 *
 * The output has to be 32768 Hz, one sample per 64 inputs. That is a ratio of
 * 64, and it cannot be done in one filter: an alias-free conversion needs the
 * response to be down by ~60 dB by 18.7 kHz, which at 128 taps of a 2.1 MHz
 * stream is a transition band 22 kHz wide. So two stages, in the standard order
 * - a cheap filter first, where the samples are many, and the expensive one
 * after the rate has come down.
 *
 * **Stage 1** is a box filter of 8: 262144 Hz, one add per input sample. Its
 * response is -3.9 dB at its own Nyquist, so it lets plenty past - but
 * everything it lets past is above 131 kHz, where a Game Boy's output has
 * little left, and stage 2 deals with the rest.
 *
 * **Stage 2** is a 64-tap windowed-sinc lowpass (Blackman, 13.5 kHz corner) at
 * 262144 Hz, decimating by 8. Measured on the host (tools/host/dectest.c) by
 * putting one loud tone in and looking for it where it would land if the
 * conversion aliased it - the figure is the alias, re a full-scale tone:
 *
 *     tone in    lands at      64-tap sinc      box filter of 8
 *     20 kHz     12768 Hz        -27.6 dB           -6.2 dB
 *     50 kHz     15536 Hz        -74.4 dB          -13.6 dB
 *     200 kHz     3392 Hz        -81.5 dB          -35.4 dB
 *     300 kHz     5088 Hz        -85.6 dB          -35.5 dB
 *     700 kHz    11872 Hz        -84.7 dB          -35.7 dB
 *
 * The 20 kHz row is the worst of them and it is the transition band, not a
 * failure of the stopband: the corner is 13.5 kHz and 64 taps at this rate take
 * about 22 kHz to get from passband to stopband, so 20 kHz is only part-way
 * down. 96 and 128 taps were measured too (total aliasing -57.2 and -58.4 dB,
 * against nothing better than this filter's -56 dB) - two dB for 50-100 % more
 * work, which is why it is 64. What is left after that is stage 1: a box of 8
 * lets everything above 131 kHz through at its own -3.9 dB per octave and this
 * filter is what stops it.
 *
 * The history is a flat array of DECIMATOR_TAPS + 8, not a ring: eight new
 * stage-1 samples go in the tail, and one output is computed over the leading
 * 64 (contiguous, and in the order the taps want, because a symmetric filter
 * reads a time-reversed window the same way round). Then the array is shifted
 * down by eight: one memmove of 128 bytes per channel per output, 256 bytes in
 * all, once per 64 input samples. It keeps the inner loop a straight read,
 * which on an ARM11 with no L2 is the whole cost of this thing.
 */
#define DECIMATOR_TAPS 64

typedef struct
{
	int32_t sum_l, sum_r;  /* stage 1: 8 input samples -> one sample */
	uint8_t count1;        /* input samples into the current stage-1 group, 0-7 */
	uint8_t pending;       /* stage-1 samples since the last output, 0-7 */
	/* The last DECIMATOR_TAPS stage-1 samples, oldest first, then 8 slots for
	 * the ones that have arrived since the last output. */
	int16_t hist_l[DECIMATOR_TAPS + 8];
	int16_t hist_r[DECIMATOR_TAPS + 8];
} decimator;

void decimator_reset(decimator *d);

/* Feed `frames` interleaved samples from the core; write at most
 * frames/64 + 1 interleaved stereo int16 samples to `out` and return the count.
 * State carries across calls, so the caller may hand over any block size. */
unsigned decimator_run(decimator *d, const uint32_t *in, unsigned frames,
		       int16_t *out);

#ifdef __cplusplus
}
#endif

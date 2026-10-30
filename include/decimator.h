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
 * The history is a doubled ring of 2 * DECIMATOR_TAPS: every stage-1 sample is
 * written at `pos` and at `pos + TAPS`, so the last 64 samples are always the
 * contiguous run starting at `pos + 1` and no shifting is needed. (It used to
 * shift the array down by eight with two memmove()s per output - see
 * docs/PORT-NOTES.md 27, where the console measured what that cost.)
 *
 * The convolution uses the filter's symmetry: tap[k] * (x[k] + x[63-k]) does
 * two taps with one multiply, so an output is 32 multiplies, not 64. That is
 * not a micro-optimisation on this hardware - the ARM11 has no L2 and no NEON,
 * and the console measured the straight 64-tap loop at 5.9 ms a frame, more
 * than a third of a 16.7 ms frame for audio alone.
 */
#define DECIMATOR_TAPS 64

typedef struct
{
	int32_t sum_l, sum_r;  /* stage 1: 8 input samples -> one sample */
	uint8_t count1;        /* input samples into the current stage-1 group, 0-7 */
	uint8_t pending;       /* stage-1 samples since the last output, 0-7 */
	uint8_t pos;           /* where the next stage-1 sample goes, 0..63 */
	/* The last DECIMATOR_TAPS stage-1 samples, held twice over so that the
	 * window a convolution reads is always contiguous - see "the doubled
	 * ring" below. Every sample is written at pos and at pos + TAPS. */
	int16_t hist_l[2 * DECIMATOR_TAPS];
	int16_t hist_r[2 * DECIMATOR_TAPS];
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

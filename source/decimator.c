/**
 * The 2 MHz -> 32768 Hz decimator: a box filter of 8, then a 64-tap
 * windowed-sinc lowpass. See include/decimator.h.
 *
 * MIT licensed.
 */
#include "decimator.h"

#include <string.h>

/* A packed sample: left in the low half, right in the high half. Reading the
 * core's buffer as 32-bit words rather than as two 16-bit halves halves the
 * number of loads for the same bytes - on an ARM11 with no L2, the loads are
 * the cost. */
static inline int32_t left_of(uint32_t v)  { return (int16_t)(v & 0xFFFFu); }
static inline int32_t right_of(uint32_t v) { return (int16_t)(v >> 16); }

/* Stage 2's coefficients, Q15, summing to exactly 32768 so a constant input
 * comes out as the same constant: 64 taps, Blackman-windowed sinc, corner at
 * 13.5 kHz of the 262144 Hz stage-1 rate, designed and rounded at build time
 * (tools/host/dectest.c measures what it does on a signal). Response:
 *
 *      1 kHz   0.00 dB      12 kHz   -3.8 dB
 *      8 kHz  -0.7 dB      16 kHz  -11.6 dB
 *     10 kHz  -1.8 dB      18.4 kHz  -19.9 dB
 *                         22 kHz   -40.8 dB
 *
 * The top octave losing a few dB is deliberate - it is where the folded copies
 * would land, and a Game Boy's output has nothing musical up there. Longer
 * filters were measured too (96 taps: total aliasing -57.2 dB; 128 taps:
 * -58.4 dB) - a couple of dB, against 50-100 % more MACs, and both are behind
 * stage 1 in the end: what the box filter of 8 lets through above 131 kHz is
 * the floor, not this.
 *
 * The per-tone measurements - what lands where, and how loud - are printed by
 * `make -C tools/host dectest`; the table above is that output, not a design
 * target.
 */
static const int16_t taps[DECIMATOR_TAPS] = {
	    0,     0,     0,     1,     3,     7,    14,    22,
	   30,    35,    35,    25,     3,   -36,   -89,  -155,
	 -226,  -291,  -334,  -338,  -285,  -158,    51,   347,
	  723,  1162,  1639,  2120,  2567,  2943,  3215,  3358,
	 3350,  3215,  2943,  2567,  2120,  1639,  1162,   723,
	  347,    51,  -158,  -285,  -338,  -334,  -291,  -226,
	 -155,   -89,   -36,     3,    25,    35,    35,    30,
	   22,    14,     7,     3,     1,     0,     0,     0,
};

void decimator_reset(decimator *d)
{
	memset(d, 0, sizeof *d);
}

/* One output sample: the 64 taps over the 64 most recent stage-1 samples.
 *
 * `hist` holds them oldest first. A symmetric FIR reads a time-reversed window
 * exactly as it reads a forward one, so the taps are applied in order and no
 * index arithmetic goes in the inner loop. The sum cannot overflow int32:
 * |sample| <= 32768 and the taps sum to 32768, so |acc| <= 2^30. */
static inline int16_t convolve(const int16_t *hist)
{
	int32_t acc = 0;

	for (int k = 0; k < DECIMATOR_TAPS; k++)
		acc += (int32_t)taps[k] * hist[k];

	acc = (acc + (1 << 14)) >> 15;

	if (acc > 32767)
		return 32767;
	if (acc < -32768)
		return -32768;
	return (int16_t)acc;
}

/* A stage-1 sample is ready: put it in the tail, and every eighth one completes
 * a block of 64 - which is one output sample. */
static inline void push(decimator *d, int16_t l, int16_t r, int16_t *out,
			unsigned *written)
{
	const unsigned at = DECIMATOR_TAPS + d->pending;

	d->hist_l[at] = l;
	d->hist_r[at] = r;

	if (++d->pending < 8)
		return;

	d->pending = 0;

	out[*written * 2] = convolve(d->hist_l);
	out[*written * 2 + 1] = convolve(d->hist_r);
	(*written)++;

	/* Drop the eight oldest and make room for the next eight: the 64 live
	 * samples are hist[8..71], and they become hist[0..63]. */
	memmove(d->hist_l, d->hist_l + 8, sizeof d->hist_l[0] * DECIMATOR_TAPS);
	memmove(d->hist_r, d->hist_r + 8, sizeof d->hist_r[0] * DECIMATOR_TAPS);
}

unsigned decimator_run(decimator *d, const uint32_t *in, unsigned frames,
		       int16_t *out)
{
	unsigned written = 0;
	int32_t sum_l = d->sum_l, sum_r = d->sum_r;
	unsigned i = 0;

	/* Fast path: one stage-1 group per eight inputs, no branch per sample.
	 * This is the common case - every runFor() call in the backend hands
	 * over 2064 samples, which divides by 8 exactly, and the state starts
	 * aligned. */
	if (d->count1 == 0)
	{
		while (i + 8 <= frames)
		{
			const uint32_t v0 = in[i + 0], v1 = in[i + 1];
			const uint32_t v2 = in[i + 2], v3 = in[i + 3];
			const uint32_t v4 = in[i + 4], v5 = in[i + 5];
			const uint32_t v6 = in[i + 6], v7 = in[i + 7];

			const int32_t l = left_of(v0) + left_of(v1) +
					  left_of(v2) + left_of(v3) +
					  left_of(v4) + left_of(v5) +
					  left_of(v6) + left_of(v7);
			const int32_t r = right_of(v0) + right_of(v1) +
					  right_of(v2) + right_of(v3) +
					  right_of(v4) + right_of(v5) +
					  right_of(v6) + right_of(v7);

			push(d, (int16_t)(l >> 3), (int16_t)(r >> 3), out, &written);
			i += 8;
		}
	}

	/* Tail, and the general case for a caller that hands over a block that
	 * does not divide by 8. */
	for (; i < frames; i++)
	{
		const uint32_t v = in[i];

		sum_l += left_of(v);
		sum_r += right_of(v);

		if (++d->count1 == 8)
		{
			d->count1 = 0;
			push(d, (int16_t)(sum_l >> 3), (int16_t)(sum_r >> 3),
			     out, &written);
			sum_l = sum_r = 0;
		}
	}

	d->sum_l = sum_l;
	d->sum_r = sum_r;

	return written;
}

/**
 * The 2 MHz -> 32768 Hz decimator, one pass. See include/decimator.h.
 *
 * MIT licensed.
 */
#include "decimator.h"

/* A packed sample: left in the low half, right in the high half. Reading the
 * core's buffer as 32-bit words rather than as two 16-bit halves halves the
 * number of loads for the same bytes - on an ARM11 with no L2, the loads are
 * the cost. */
static inline int32_t left_of(uint32_t v)  { return (int16_t)(v & 0xFFFFu); }
static inline int32_t right_of(uint32_t v) { return (int16_t)(v >> 16); }

void decimator_reset(decimator *d)
{
	d->sum_l = d->sum_r = 0;
	d->acc_l = d->acc_r = 0;
	d->count1 = d->count2 = 0;
}

/* One stage-1 group is complete: average it and hand it to stage 2. Returns 1
 * when that completed a stage-2 group too, in which case `out` has been
 * written. */
static inline void emit(decimator *d, int16_t *out, unsigned *written)
{
	out[*written * 2]     = (int16_t)(d->acc_l >> 3);
	out[*written * 2 + 1] = (int16_t)(d->acc_r >> 3);
	(*written)++;
}

unsigned decimator_run(decimator *d, const uint32_t *in, unsigned frames,
		       int16_t *out)
{
	unsigned written = 0;
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

			d->acc_l += l >> 3;
			d->acc_r += r >> 3;
			i += 8;

			if (++d->count2 == 8)
			{
				d->count2 = 0;
				emit(d, out, &written);
				d->acc_l = d->acc_r = 0;
			}
		}
	}

	/* Tail, and the general case for a caller that hands over a block that
	 * does not divide by 8. */
	for (; i < frames; i++)
	{
		const uint32_t v = in[i];

		d->sum_l += left_of(v);
		d->sum_r += right_of(v);

		if (++d->count1 == 8)
		{
			d->count1 = 0;
			d->acc_l += d->sum_l >> 3;
			d->acc_r += d->sum_r >> 3;
			d->sum_l = d->sum_r = 0;

			if (++d->count2 == 8)
			{
				d->count2 = 0;
				emit(d, out, &written);
				d->acc_l = d->acc_r = 0;
			}
		}
	}

	return written;
}

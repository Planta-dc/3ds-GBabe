/**
 * The 2 MHz -> 32768 Hz audio decimator: two box filters, then a windowed-sinc
 * lowpass. See include/decimator.h.
 *
 * MIT licensed.
 */
#include "decimator.h"

#include <string.h>

_Static_assert(DECIMATOR_TAPS == 32, "the tables below are 32 taps");
_Static_assert(DECIMATE_BOX1 == 8, "stage 1 is a box of 8");
_Static_assert(DECIMATE_BOX2 == 2, "stage 2 is a box of 2");

/* Stage 3's coefficients: DECIMATOR_TAPS taps, Q15, summing to exactly 32768
 * so a constant input comes out as the same constant. Blackman-windowed sinc,
 * corner 13.5 kHz of the 131072 Hz stage-2 rate, decimating by 4.
 *
 * They are stored **two taps to a 32-bit word** (tap 0 in the low half, tap 1
 * in the high half) and the history is read packed the same way, because ARMv6
 * has no NEON and this machine is instruction-bound rather than maths-bound:
 * `smlabb`/`smlatt` on a packed word is two MACs per instruction.
 *
 * See docs/PORT-NOTES.md 29 for the measurements that put the filter here and
 * not at the stage-1 rate. `make -C tools/host dectest` prints what it does.
 */
static const int32_t taps[DECIMATOR_TAPS / 2] = {
	           2,      1966088,      3866683,    -15138843,
	   -43647480,     24444473,    245696323,    432936342,
	   362158542,    121835173,    -29818508,    -32965275,
	    -1704168,      3866683,       524318,            0,
};

/* Signed 16-bit halves of a packed word, sign-extended to 32 bits. */
static inline int32_t left_of(uint32_t v)  { return (int16_t)(v & 0xFFFFu); }
static inline int32_t right_of(uint32_t v) { return (int16_t)(v >> 16); }

/* Extract 16-bit halves from packed 32-bit words (for the FIR). */
#define LO(v) ((int32_t)(int16_t)(uint16_t)((v) & 0xFFFFu))
#define HI(v) ((int32_t)(int16_t)(uint16_t)((uint32_t)(v) >> 16))

/* One output sample. `h` points at DECIMATOR_TAPS stage-2 samples, oldest
 * first, 4-byte aligned (the doubled ring guarantees it - see the header).
 *
 * Two accumulators because a single one serialises: each pair of MACs would
 * wait for the previous result, and on an in-order core that stall is the loop.
 *
 * The sum cannot overflow int32. The taps sum to 32768, but their absolute
 * values sum to 40308 (a lowpass has negative lobes), and a sample is at most
 * 32768 in magnitude, so |acc| <= 40308 x 32768 = 1.32e9 - comfortably inside
 * the 2.147e9 a signed 32-bit accumulator holds, with the worst case being a
 * full-scale square wave for ever.
 */
static inline __attribute__((always_inline)) int16_t fir(const int16_t *h)
{
	const int32_t *w = (const int32_t *)h;
	int32_t a0 = 0, a1 = 0;

	for (int i = 0; i < DECIMATOR_TAPS / 4; i++)
	{
		const int32_t w0 = w[i * 2 + 0], t0 = taps[i * 2 + 0];
		const int32_t w1 = w[i * 2 + 1], t1 = taps[i * 2 + 1];

		a0 += LO(w0) * LO(t0) + HI(w0) * HI(t0);
		a1 += LO(w1) * LO(t1) + HI(w1) * HI(t1);
	}

	int32_t acc = (a0 + a1 + (1 << 14)) >> 15;

	if (acc > 32767)
		return 32767;
	if (acc < -32768)
		return -32768;
	return (int16_t)acc;
}

void decimator_reset(decimator *d)
{
	memset(d, 0, sizeof *d);
}

/* Each output is the average of DECIMATE_FIR consecutive stage-2 samples,
 * where a stage-2 sample is itself the average of two stage-1 samples and a
 * stage-1 sample the average of eight inputs: 8 x 2 x 4 = 64, one output per 64
 * inputs. See include/decimator.h for why the filter lives at the stage-2 rate
 * and not the stage-1 one. */
static inline void push(decimator *d, int16_t l, int16_t r, int16_t *out,
			unsigned *written)
{
	/* Stage 2: a box of two. Two adds here instead of a tap this long in
	 * the FIR, which would have cost a multiply and two loads. */
	d->sum2_l += l;
	d->sum2_r += r;

	if (++d->count2 < DECIMATE_BOX2)
		return;

	d->count2 = 0;

	const int16_t ml = (int16_t)(d->sum2_l >> 1);
	const int16_t mr = (int16_t)(d->sum2_r >> 1);

	d->sum2_l = d->sum2_r = 0;

	/* The doubled ring: written twice so that every window of
	 * DECIMATOR_TAPS is contiguous, no shifting. */
	const unsigned p = d->pos;

	d->hist_l[p] = ml;
	d->hist_l[p + DECIMATOR_TAPS] = ml;
	d->hist_r[p] = mr;
	d->hist_r[p + DECIMATOR_TAPS] = mr;
	d->pos = (p + 1) & (DECIMATOR_TAPS - 1);

	if (++d->pending < DECIMATE_FIR)
		return;

	d->pending = 0;

	/* The newest sample is at p, so the DECIMATOR_TAPS samples ending there
	 * start at p + 1. pos advances by DECIMATE_FIR (4) per output, so that
	 * start stays 4-byte aligned and the packed reads are legal. */
	out[*written * 2] = fir(&d->hist_l[p + 1]);
	out[*written * 2 + 1] = fir(&d->hist_r[p + 1]);
	(*written)++;
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

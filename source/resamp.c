/**
 * Variable-rate linear resampler for the audio FIFO. See include/resamp.h for
 * why the port needs one at all.
 *
 * MIT licensed.
 */
#include "resamp.h"

/* +-4 %: enough to cover a console running at 57 fps, small enough that no
 * Game Boy sound effect audibly changes pitch. */
#define STEP_MIN  (RESAMP_UNITY - RESAMP_UNITY / 25)
#define STEP_MAX  (RESAMP_UNITY + RESAMP_UNITY / 25)

/* Proportional gain: the queue has to be off target by an eighth of its own
 * size before the resampler runs at its limit. A 2 KiB queue and a 125 ms FIFO
 * means the correction is spread over seconds, which is what keeps it
 * inaudible. */
#define CONTROL_GAIN 8

/* Below this the queue is "where it should be" and the step is exactly 1:1, so
 * a console that is keeping up never resamples anything. ~64 frames = 2 ms. */
#define CONTROL_DEADBAND(target) ((target) / 32)

int32_t resamp_control_step(int32_t fill, int32_t target)
{
	if (target <= 0)
		return RESAMP_UNITY;

	int32_t err = fill - target;
	if (err < CONTROL_DEADBAND(target) && err > -CONTROL_DEADBAND(target))
		return RESAMP_UNITY;

	/* err > 0 means the queue is too full: eat more input per output frame
	 * (step > 1) so fewer output frames are produced.
	 *
	 * 64-bit on purpose: the product is 2^21 at most with a real queue, but
	 * this function is also the one thing in the audio path that is worth
	 * being able to test at silly values without it wrapping. */
	int64_t step = RESAMP_UNITY +
		       ((int64_t)err * RESAMP_UNITY) / ((int64_t)target * CONTROL_GAIN);

	if (step < STEP_MIN)
		step = STEP_MIN;
	if (step > STEP_MAX)
		step = STEP_MAX;
	return (int32_t)step;
}

void resamp_reset(resamp *r)
{
	r->step = RESAMP_UNITY;
	r->pos = 0;
	r->carry[0] = r->carry[1] = 0;
	r->started = false;
}

void resamp_set_step(resamp *r, int32_t step_q16)
{
	if (step_q16 < STEP_MIN)
		step_q16 = STEP_MIN;
	if (step_q16 > STEP_MAX)
		step_q16 = STEP_MAX;
	r->step = step_q16;
}

/* Interpolate one channel between a and b, `frac` in 1/256ths. */
static int16_t lerp(int16_t a, int16_t b, int32_t frac)
{
	return (int16_t)(a + (((int32_t)(b - a) * frac) >> 8));
}

unsigned resamp_run(resamp *r, const int16_t *in, unsigned frames,
		    int16_t *out, unsigned out_cap)
{
	if (frames == 0 || out_cap == 0 || r->step <= 0)
		return 0;

	/* The virtual array is [carry, in[0], in[1], ...]: index 0 is the frame
	 * the previous block ended on, so a fractional position between two
	 * blocks interpolates instead of jumping. */
	if (!r->started)
	{
		/* Start one virtual frame in, so the first output is in[0] rather
		 * than the carried copy of it: duplicate a sample at the start of
		 * a stream and it is a click, small but not free. */
		r->carry[0] = in[0];
		r->carry[1] = in[1];
		r->pos = RESAMP_UNITY;
		r->started = true;
	}

	unsigned written = 0;
	int32_t pos = r->pos;

	for (;;)
	{
		const uint32_t idx = (uint32_t)(pos >> 16);

		/* Needs the frame after this one, which is in[idx] (idx+1 in the
		 * virtual array), so stop one short. */
		if (idx >= frames || written >= out_cap)
			break;

		const int32_t frac = (pos >> 8) & 0xFF;

		int16_t a_l, a_r, b_l, b_r;

		if (idx == 0)
		{
			a_l = r->carry[0];
			a_r = r->carry[1];
		}
		else
		{
			a_l = in[(idx - 1) * 2];
			a_r = in[(idx - 1) * 2 + 1];
		}

		b_l = in[idx * 2];
		b_r = in[idx * 2 + 1];

		out[written * 2]     = lerp(a_l, b_l, frac);
		out[written * 2 + 1] = lerp(a_r, b_r, frac);
		written++;

		pos += r->step;
	}

	/* The next block's index 0 is this block's last frame. `pos` is measured
	 * from the old carry, so shift it by the whole frames consumed. */
	r->carry[0] = in[(frames - 1) * 2];
	r->carry[1] = in[(frames - 1) * 2 + 1];
	r->pos = pos - (int32_t)(frames << 16);

	/* Defensive: a wildly large step could leave the position past the block
	 * (only reachable if the clamp above is ever removed). */
	if (r->pos < 0)
		r->pos = 0;

	return written;
}

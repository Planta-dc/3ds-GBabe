/**
 * Variable-rate linear resampler for the audio FIFO. See include/resamp.h for
 * why the port needs one at all.
 *
 * MIT licensed.
 */
#include "resamp.h"

/* +-20 %: the range is set by which mistake is worse. A console running a Game
 * Boy Color game at 55 fps is a game running at 92 %, and its sound should slow
 * down with it - that is what the hardware being imitated does when it is short
 * of time. Refusing to correct past 4 % (which is what this did) does not keep
 * the pitch right, it drains the queue and turns a slightly slow game into a
 * stream of holes, which is exactly what the console log was showing. */
#define STEP_MIN  (RESAMP_UNITY - RESAMP_UNITY / 5)
#define STEP_MAX  (RESAMP_UNITY + RESAMP_UNITY / 5)

/* Level trim: a queue a whole target too full asks for 1/64 (1.6 %) on top of
 * the measured rate. Deliberately slow - the rate is measured, not inferred
 * from the level, and the level sawtooths by a whole block as blocks are
 * submitted, so this exists only to remove bias between the two clocks. */
#define TRIM_GAIN 64

/* Inside this band the rate is exactly 1:1 and nothing is resampled at all, so
 * a console that is keeping up gets bit-exact audio through here: a sample in is
 * a sample out, no interpolation, no rounding. Half a percent is invisible in
 * pitch terms and wide enough to swallow the small permanent offset the level
 * trim settles at (the queue's natural mean is not exactly its target, because
 * when a block is submitted is a deadline and not a tick). */
#define SNAP (RESAMP_UNITY / 200)

int32_t resamp_control_step(int32_t fill_ema, int32_t target, int32_t rate_q16)
{
	if (target <= 0)
		return RESAMP_UNITY;

	/* Feedforward: the rate the emulator is actually producing samples at,
	 * already in the units resamp_run() wants. This used to be inferred from
	 * the queue level instead, and the queue level is a sawtooth - it drops a
	 * whole 1024-frame block every 31 ms - so the loop spent its life pinned
	 * against the clamps, modulating the pitch by several percent at the
	 * block rate. Measured on the host, a full-speed console was being
	 * stretched 0.5 % on average with a 5 % peak-to-peak wobble. */
	int64_t step = (rate_q16 > 0) ? rate_q16 : RESAMP_UNITY;

	/* Feedback: the slow bias, from a level that has been smoothed far
	 * longer than the block period (see audio_rate_step()). */
	step += ((int64_t)(fill_ema - target) * RESAMP_UNITY) /
		((int64_t)target * TRIM_GAIN);

	if (step > RESAMP_UNITY - SNAP && step < RESAMP_UNITY + SNAP)
		step = RESAMP_UNITY;
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

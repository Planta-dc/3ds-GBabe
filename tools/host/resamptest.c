/**
 * The audio rate matcher, as arithmetic.
 *
 *   make -C tools/host resamptest
 *
 * Two things are checked, and neither needs a console:
 *
 *   resamp_control_step()  turns "the FIFO is this full against this target"
 *                          into "run the resampler this fast". It has to be
 *                          1:1 on target, stretch when the queue is draining
 *                          (the slow-frame case that used to be a hole), and
 *                          never ask for more than the 5 % the ear tolerates.
 *   resamp_run()           does the resampling. At 1:1 it must be the identity
 *                          - a sample dropped or repeated per block would be a
 *                          31 ms periodic click - and at other rates it must
 *                          stay continuous across block boundaries, because a
 *                          block is 31 ms and a discontinuity there is exactly
 *                          the artefact this whole thing exists to prevent.
 *
 * NOT part of the 3DS build.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "resamp.h"

static int failures;

static void check(const char *what, int ok)
{
	printf("%s: %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		failures++;
}

/* ------------------------------------------------------------------------- */
/* The control law                                                           */
/* ------------------------------------------------------------------------- */

static void test_control(void)
{
	const int32_t target = 2048;

	check("on target is 1:1", resamp_control_step(target, target) == RESAMP_UNITY);
	check("a full queue compresses (eats input faster)",
	      resamp_control_step(target * 2, target) > RESAMP_UNITY);
	check("an empty queue stretches (produces more samples)",
	      resamp_control_step(0, target) < RESAMP_UNITY);

	/* A queue that is a hair off target must not move the rate at all, or the
	 * resampler would modulate the pitch of every sound for nothing. */
	check("a small error is left alone",
	      resamp_control_step(target + 20, target) == RESAMP_UNITY &&
	      resamp_control_step(target - 20, target) == RESAMP_UNITY);

	/* Clamped: 5 % is the limit, whatever the error. */
	const int32_t worst_low = resamp_control_step(-100000, target);
	const int32_t worst_high = resamp_control_step(100000, target);

	printf("clamps: %d.%03d..%d.%03d\n", worst_low / 65536,
	       (int)(((worst_low % 65536) * 1000) / 65536),
	       worst_high / 65536,
	       (int)(((worst_high % 65536) * 1000) / 65536));

	check("the stretch is limited to 5 %",
	      worst_low >= RESAMP_UNITY - RESAMP_UNITY / 25);
	check("the squeeze is limited to 5 %",
	      worst_high <= RESAMP_UNITY + RESAMP_UNITY / 25);

	/* Monotone: a fuller queue never asks for more stretching than an empty
	 * one. This is the property that stops the loop oscillating. */
	int monotone = 1;
	int32_t prev = resamp_control_step(0, target);

	for (int32_t fill = 0; fill <= target * 2; fill += target / 32)
	{
		const int32_t step = resamp_control_step(fill, target);

		if (step < prev)
			monotone = 0;
		prev = step;
	}
	check("a fuller queue never stretches more", monotone);

	/* And it must survive being asked nonsense. */
	check("a zero target is 1:1", resamp_control_step(10, 0) == RESAMP_UNITY);
	check("a negative target is 1:1", resamp_control_step(10, -5) == RESAMP_UNITY);
}

/* ------------------------------------------------------------------------- */
/* The resampler                                                             */
/* ------------------------------------------------------------------------- */

#define BLOCK 72 /* what the decimator hands over per runFor() call */

/* A ramp that never wraps, so a dropped or duplicated sample is visible as a
 * step of 2 or 0 instead of 1. */
static void fill_ramp(int16_t *buf, unsigned frames, int *next)
{
	for (unsigned i = 0; i < frames; i++)
	{
		const int16_t v = (int16_t)((*next) & 0x7FFF);

		buf[i * 2] = v;
		buf[i * 2 + 1] = v;
		(*next)++;
	}
}

/* The stream matters, not the block: at 1:1 every input sample must come out
 * exactly once, in order. (The first block is one sample short, because the
 * resampler starts one virtual frame in to avoid repeating a sample at the very
 * start of the stream; after that it is 72 in, 72 out.) */
static void test_identity(void)
{
	static int16_t in[BLOCK * 2], out[BLOCK * 2];
	static int in_stream[BLOCK * 20];
	static int out_stream[BLOCK * 20];
	resamp r;
	int next = 0;
	unsigned total = 0;
	int bad = 0;

	resamp_reset(&r);

	for (int block = 0; block < 20; block++)
	{
		fill_ramp(in, BLOCK, &next);
		for (unsigned i = 0; i < BLOCK; i++)
			in_stream[block * BLOCK + i] = in[i * 2];

		const unsigned n = resamp_run(&r, in, BLOCK, out, BLOCK + 8);

		for (unsigned i = 0; i < n; i++)
			out_stream[total + i] = out[i * 2];

		total += n;
	}

	for (unsigned i = 0; i < total; i++)
		if (out_stream[i] != in_stream[i])
			bad++;

	printf("1:1: %u samples out of %d in, %d out of place\n",
	       total, BLOCK * 20, bad);

	check("at 1:1 every input sample comes out once, in order",
	      total + 1 == BLOCK * 20 && bad == 0);
}

/* A 200 Hz sine, which is what the discontinuity check has to look at: a ramp
 * *has* to jump when the resampler skips an input sample, and a sine does not.
 * The largest step of a sine at 32768 Hz is 2*pi*f/sr*amplitude, so anything
 * much beyond that is a click at a block boundary rather than the signal. */
static void test_rate(int32_t step, const char *name, int expect_more,
		      int expect_less, unsigned *worst_step)
{
	static int16_t in[BLOCK * 2], out[BLOCK * 2];
	resamp r;
	unsigned total = 0;
	int prev = -1;
	unsigned worst = 0;
	double phase = 0.0;

	resamp_reset(&r);
	resamp_set_step(&r, step);

	for (int block = 0; block < 300; block++)
	{
		for (unsigned i = 0; i < BLOCK; i++)
		{
			const int16_t v = (int16_t)(12000.0 *
						    sin(phase));
			in[i * 2] = in[i * 2 + 1] = v;
			phase += 2.0 * 3.14159265358979 * 200.0 / 32768.0;
		}

		const unsigned n = resamp_run(&r, in, BLOCK, out, BLOCK + 8);

		total += n;
		for (unsigned i = 0; i < n; i++)
		{
			const int v = out[i * 2];

			if (prev >= 0 && (unsigned)abs(v - prev) > worst)
				worst = (unsigned)abs(v - prev);
			prev = v;
		}
	}

	/* 300 blocks of 72 frames is 21600 input frames; a step of `s` produces
	 * about 21600/s. */
	const unsigned expect = (unsigned)((21600.0 * 65536.0) / (double)step);
	const int within = (int)total > (int)expect - 100 &&
			   (int)total < (int)expect + 100;

	printf("%s: %u frames out of 21600 in (expect ~%u), worst step %u\n",
	       name, total, expect, worst);

	check(name, within);
	/* 2*pi*200/32768*12000 = 460 at 1:1; +-5 % and interpolation add a
	 * little. A dropped or repeated block would be thousands. */
	check("... with no discontinuity at a block boundary", worst < 700);

	if (expect_more)
		check("... and it produced more samples than it was given",
		      total > 21600);
	if (expect_less)
		check("... and it produced fewer samples than it was given",
		      total < 21600);

	*worst_step = worst;
}

int main(void)
{
	unsigned worst = 0;

	test_control();
	test_identity();
	test_rate(RESAMP_UNITY, "1:1 on a sine", 0, 0, &worst);
	test_rate(RESAMP_UNITY - RESAMP_UNITY / 25, "stretched 4 %", 1, 0, &worst);
	test_rate(RESAMP_UNITY + RESAMP_UNITY / 25, "squeezed 4 %", 0, 1, &worst);

	/* The buffer the backend gives it is 80 frames for 72 in; a step at the
	 * clamp must not overflow it. */
	{
		static int16_t in[BLOCK * 2], out[80 * 2];
		resamp r;
		int next = 0;

		resamp_reset(&r);
		resamp_set_step(&r, RESAMP_UNITY - RESAMP_UNITY / 25);

		unsigned worst = 0;
		for (int block = 0; block < 200; block++)
		{
			fill_ramp(in, BLOCK, &next);

			const unsigned n = resamp_run(&r, in, BLOCK, out, 80);

			if (n > worst)
				worst = n;
		}
		printf("worst block at full stretch: %u frames\n", worst);
		check("the backend's 80-frame buffer is big enough", worst <= 80);
	}

	printf("resamp: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}

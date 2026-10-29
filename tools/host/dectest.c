/**
 * The decimator: bit-exact against the two-stage version it replaces.
 *
 *   make -C tools/host dectest
 *
 * This is a pure speed rewrite - one pass instead of two, no per-sample branch,
 * one 32-bit load per stereo sample instead of two 16-bit ones - so the thing
 * that makes it acceptable is that its output is *identical*, not similar. A
 * reformulation that is "about the same" would be another unverifiable claim on
 * the pile this project already has; 200 frames of pseudo-random audio compared
 * sample for sample is not.
 *
 * The reference below is the code that was in source/emu_gambatte.cpp: two
 * cascaded box filters of 8, each with its own truncation, carrying state
 * across calls. Both must agree byte for byte, in any block size.
 *
 * Also prints a rough timing ratio. It is x86 and it is indicative only - the
 * number that matters is `dec=` in the console log - but a rewrite of this kind
 * should at least not be slower on the machine that can be measured.
 *
 * NOT part of the 3DS build.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "decimator.h"

static int failures;

static void check(const char *what, int ok)
{
	printf("%s: %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		failures++;
}

/* ------------------------------------------------------------------------- */
/* The reference: exactly what emu_gambatte.cpp used to do                   */
/* ------------------------------------------------------------------------- */

typedef struct
{
	int32_t sum_l, sum_r;
	uint8_t count;
} ref_stage;

static void ref_reset(ref_stage *s) { s->sum_l = s->sum_r = 0; s->count = 0; }

static unsigned ref_run(ref_stage *s, const int16_t *in, unsigned n, int16_t *out)
{
	unsigned written = 0;

	for (unsigned i = 0; i < n; i++)
	{
		s->sum_l += in[i * 2];
		s->sum_r += in[i * 2 + 1];

		if (++s->count == 8)
		{
			s->count = 0;
			out[written * 2]     = (int16_t)(s->sum_l >> 3);
			out[written * 2 + 1] = (int16_t)(s->sum_r >> 3);
			written++;
			s->sum_l = s->sum_r = 0;
		}
	}
	return written;
}

/* ------------------------------------------------------------------------- */

/* A deterministic pseudo-random signal with a Game Boy-ish shape: a square wave
 * plus noise, so the accumulators see real sums rather than a constant. */
static uint32_t rng_state = 0x12345678u;

static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static void fill(uint32_t *buf, unsigned n, unsigned *phase)
{
	for (unsigned i = 0; i < n; i++)
	{
		const int16_t sq = (*phase < 256) ? 8000 : -8000;
		const int16_t noise = (int16_t)((rng() & 0x7FF) - 0x400);
		const int16_t l = (int16_t)(sq + noise);
		const int16_t r = (int16_t)(sq / 2 - noise);

		/* The core's layout: left in the low half, right in the high. */
		buf[i] = (uint16_t)l | ((uint32_t)(uint16_t)r << 16);
		*phase = (*phase + 1) & 0x1FF;
	}
}

#define FRAME_SAMPLES 35112u /* one Game Boy frame at 2 MHz                */
#define MAX_OUT       (FRAME_SAMPLES / 64 + 2)

static void compare(int frames, unsigned block, const char *what)
{
	static uint32_t in[FRAME_SAMPLES];
	static int16_t got[MAX_OUT], want[MAX_OUT];
	static int16_t mid[FRAME_SAMPLES / 8 * 2 + 2];
	decimator d;
	ref_stage s1, s2;
	unsigned phase = 0;
	unsigned bad = 0, total = 0;

	decimator_reset(&d);
	ref_reset(&s1);
	ref_reset(&s2);

	for (int f = 0; f < frames; f++)
	{
		unsigned used = 0;

		while (used < FRAME_SAMPLES)
		{
			unsigned n = block;
			if (n > FRAME_SAMPLES - used)
				n = FRAME_SAMPLES - used;

			fill(in, n, &phase);

			const unsigned ng = decimator_run(&d, in, n, got);

			const unsigned nm = ref_run(&s1, (const int16_t *)in, n, mid);
			const unsigned nw = ref_run(&s2, mid, nm, want);

			if (ng != nw)
				bad++;

			const unsigned cmp = (ng < nw) ? ng : nw;
			for (unsigned i = 0; i < cmp * 2; i++)
				if (got[i] != want[i])
					bad++;

			total += nw;
			used += n;
		}
	}

	printf("%-28s %d frames in %4u-sample blocks: %u samples, %u differ\n",
	       what, frames, block, total, bad);
	check(what, bad == 0 && total > 0);
}

/* ------------------------------------------------------------------------- */

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void timing(void)
{
	static uint32_t in[FRAME_SAMPLES];
	static int16_t got[MAX_OUT], mid[FRAME_SAMPLES / 8 * 2];
	decimator d;
	ref_stage s1, s2;
	unsigned phase = 0;
	const int frames = 2000;

	decimator_reset(&d);
	ref_reset(&s1);
	ref_reset(&s2);
	fill(in, FRAME_SAMPLES, &phase);

	double t0 = now_ms();
	for (int f = 0; f < frames; f++)
		decimator_run(&d, in, FRAME_SAMPLES, got);
	double t1 = now_ms();
	for (int f = 0; f < frames; f++)
	{
		const unsigned nm = ref_run(&s1, (const int16_t *)in, FRAME_SAMPLES,
					    mid);
		ref_run(&s2, mid, nm, got);
	}
	double t2 = now_ms();

	const double one = (t1 - t0) / frames;
	const double two = (t2 - t1) / frames;

	printf("one pass   %6.3f ms/frame\n", one);
	printf("two stages %6.3f ms/frame  (%.2fx)\n", two, two / one);
	printf("(x86, indicative only - the console's `dec=` is the measurement)\n");
}

int main(void)
{
	compare(200, FRAME_SAMPLES / 17, "35112-frame, uneven blocks");
	compare(50, 2064, "2064-sample blocks (the backend)");
	compare(20, 1000, "1000-sample blocks");
	compare(20, 64, "64-sample blocks");
	compare(20, 7, "7-sample blocks (worst case)");
	compare(20, 1, "1-sample blocks");

	timing();

	printf("decimator: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}

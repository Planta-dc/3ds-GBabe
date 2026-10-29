/**
 * The decimator: what it keeps, what it must not keep, and what it costs.
 *
 *   make -C tools/host dectest
 *
 * This used to be two cascaded box filters of 8 over the whole 64:1 ratio,
 * and the test checked that a rewritten version of that produced bit-identical
 * output. It did - and it was still the wrong filter. There is a real lowpass in
 * it now, so the test is about the signal:
 *
 *   - the rate and the joins: one output per 64 inputs, the same output for any
 *     block size the caller hands over (the backend uses 2064, the box-filter
 *     tests used 1), and no glitch at a block boundary;
 *   - the passband: a 1 kHz tone comes out at the amplitude it went in at;
 *   - the stopband: tones above the 16.384 kHz Nyquist must not come back as
 *     audible tones. This is the check the old filter fails, and it prints what
 *     the old filter does to the same input, so the difference is a measurement
 *     and not a claim in a comment;
 *   - DC and full scale.
 *
 * The aliasing figure is exact rather than statistical: a tone at f0 sampled at
 * 2.097 MHz comes out of a decimator at the single frequency it folds to, so a
 * one-bin DFT at that frequency is the whole of the energy it left in the band.
 * Anything a listener would hear from fold-back is in those numbers.
 *
 * NOT part of the 3DS build.
 */
#include <math.h>
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

#define IN_RATE   2097152.0
#define OUT_RATE  32768u
#define FRAME     35112u           /* one Game Boy frame at 2 MHz        */
#define MAX_OUT   (FRAME / 64 + 2)
#define FRAMES    60               /* a second of audio                  */
#define TONE_AMP  12000.0

/* The signal source, producing `n` stereo frames in the core's packed layout. */
typedef void (*generator)(uint32_t *buf, unsigned n, unsigned *phase);

/* The filter this replaces: two cascaded box filters of 8, each with its own
 * truncation, carrying state across calls. */
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
/* Signals                                                                   */
/* ------------------------------------------------------------------------- */

static uint32_t rng_state = 0x12345678u;

static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/* A Game Boy-ish signal: a square wave plus noise, so the accumulators see real
 * sums rather than a constant. */
static void gen_game(uint32_t *buf, unsigned n, unsigned *phase)
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

static double tone_hz;
static double tone_phase;

static void gen_tone(uint32_t *buf, unsigned n, unsigned *phase)
{
	(void)phase;

	for (unsigned i = 0; i < n; i++)
	{
		const int16_t v = (int16_t)(TONE_AMP * sin(tone_phase) + 0.5);

		tone_phase += 2.0 * M_PI * tone_hz / IN_RATE;
		if (tone_phase > 2.0 * M_PI)
			tone_phase -= 2.0 * M_PI;

		buf[i] = (uint16_t)v | ((uint32_t)(uint16_t)v << 16);
	}
}

static void gen_dc(uint32_t *buf, unsigned n, unsigned *phase)
{
	(void)phase;
	for (unsigned i = 0; i < n; i++)
		buf[i] = 1234u | (1234u << 16);
}

static void gen_square(uint32_t *buf, unsigned n, unsigned *phase)
{
	for (unsigned i = 0; i < n; i++)
	{
		const int16_t v = (*phase < 256) ? 30000 : -30000;

		buf[i] = (uint16_t)v | ((uint32_t)(uint16_t)v << 16);
		*phase = (*phase + 1) & 0x1FF;
	}
}

/* ------------------------------------------------------------------------- */
/* Running a signal through both filters                                     */
/* ------------------------------------------------------------------------- */

/* Interleaved stereo comes out of decimator_run, so a buffer for MAX_OUT
 * frames holds twice that many int16 - the previous version of this test got
 * that wrong and wrote past its arrays, which is why its numbers never agreed
 * with anything. */
static int16_t frame_new[MAX_OUT * 2], frame_old[MAX_OUT * 2];
static int32_t out_new[MAX_OUT * FRAMES];
static int32_t out_old[MAX_OUT * FRAMES];

/* Decimate `FRAMES` frames of `gen`. Fills out_new (always) and out_old (when
 * with_ref), returns the number of samples written to each. */
static unsigned run_signal(generator gen, int with_ref)
{
	static uint32_t in[FRAME];
	static int16_t mid[FRAME / 8 + 2], tmp[MAX_OUT * 2];
	decimator d;
	ref_stage s1, s2;
	unsigned phase = 0, n_new = 0, n_old = 0;

	decimator_reset(&d);
	ref_reset(&s1);
	ref_reset(&s2);

	for (int f = 0; f < FRAMES; f++)
	{
		gen(in, FRAME, &phase);

		const unsigned n = decimator_run(&d, in, FRAME, frame_new);
		for (unsigned i = 0; i < n; i++)
			out_new[n_new + i] = frame_new[i * 2];
		n_new += n;

		if (with_ref)
		{
			const unsigned nm = ref_run(&s1, (const int16_t *)in, FRAME, mid);
			const unsigned nr = ref_run(&s2, mid, nm, frame_old);

			for (unsigned i = 0; i < nr; i++)
				out_old[n_old + i] = frame_old[i * 2];
			n_old += nr;
		}
	}

	if (!with_ref)
		n_old = 0;

	if (n_new > (unsigned)(MAX_OUT * FRAMES) || n_old > (unsigned)(MAX_OUT * FRAMES))
	{
		printf("buffer overflow: %u %u\n", n_new, n_old);
		exit(1);
	}

	return n_new;
}

/* RMS of the left channel from `skip` on, as an amplitude (a sine of amplitude
 * A has RMS A/sqrt(2)). Total RMS is the right measurement here: the signals in
 * these tests are single tones, so whatever the filter leaves in the output is
 * that tone, attenuated - there is nothing else for a spectral estimate to
 * separate it from, and no window or normalisation to get wrong. */
static double rms_amplitude(const int32_t *x, unsigned n, unsigned skip)
{
	double acc = 0.0;

	for (unsigned i = skip; i < n; i++)
		acc += (double)x[i] * (double)x[i];

	return sqrt(2.0 * acc / (double)(n - skip));
}

/* Where a tone at f0 ends up after a 64:1 decimation to 32768 Hz. */
static double folded(double f0)
{
	double f = fmod(f0, (double)OUT_RATE);
	if (f > OUT_RATE / 2.0)
		f = OUT_RATE - f;
	return f;
}

static void measure_alias(const char *what, double f0, int with_ref)
{
	tone_hz = f0;
	tone_phase = 0.0;

	const unsigned n = run_signal(gen_tone, with_ref);
	const double ours = rms_amplitude(out_new, n, 4096);

	/* Into the band it folds to (printed so the table reads as a map of
	 * what lands where), and how loud it is against the tone that went in. */
	printf("  %-10s %7.0f Hz -> %6.0f Hz: %6.1f dB", what, f0, folded(f0),
	       20.0 * log10((ours + 1e-9) / TONE_AMP));

	if (with_ref)
	{
		const double theirs = rms_amplitude(out_old, n, 4096);
		printf("   (box filters %6.1f dB)",
		       20.0 * log10((theirs + 1e-9) / TONE_AMP));
	}
	printf("\n");
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                     */
/* ------------------------------------------------------------------------- */

/* Any block size gives the same samples: the state carries across calls and the
 * caller's own buffer boundaries must be invisible in the output. What is
 * compared is the left channel of each output frame. */
static void check_block_sizes(void)
{
	static uint32_t in[FRAME];
	static int16_t tmp[MAX_OUT * 2];
	static int32_t whole[MAX_OUT * 4];
	const unsigned sizes[] = { 1, 7, 63, 64, 65, 1000, 2064, 35112 };
	unsigned phase = 0, n_whole = 0;
	decimator d;

	/* Reference: one call per frame. The signal is regenerated for every
	 * pass below, so the generator's state has to be reset with it - it was
	 * not, to begin with, and every pass got different noise. */
	rng_state = 0x12345678u;
	decimator_reset(&d);
	for (int f = 0; f < 4; f++)
	{
		gen_game(in, FRAME, &phase);
		const unsigned n = decimator_run(&d, in, FRAME, tmp);

		for (unsigned i = 0; i < n; i++)
			whole[n_whole + i] = tmp[i * 2];
		n_whole += n;
	}

	for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++)
	{
		unsigned p = 0, total = 0, bad = 0;

		rng_state = 0x12345678u;
		decimator_reset(&d);

		for (int f = 0; f < 4; f++)
		{
			gen_game(in, FRAME, &p);

			unsigned used = 0;
			while (used < FRAME)
			{
				unsigned n = sizes[s];

				if (n > FRAME - used)
					n = FRAME - used;

				const unsigned w = decimator_run(&d, in + used, n, tmp);

				for (unsigned i = 0; i < w; i++, total++)
					if (total >= n_whole ||
					    tmp[i * 2] != whole[total])
						bad++;

				used += n;
			}
		}

		char label[64];
		snprintf(label, sizeof label, "%u-sample blocks, same output",
			 sizes[s]);
		printf("%-34s %u samples\n", label, total);
		check(label, bad == 0 && total == n_whole);
	}
}

static void check_rate(void)
{
	const unsigned n = run_signal(gen_game, 0);

	/* 35112 inputs per frame is 548.6 outputs; 60 frames is 32917.5. */
	const double want = (double)FRAME * FRAMES / 64.0;

	printf("60 frames: %u out, expected %.1f\n", n, want);
	check("one output sample per 64 inputs", fabs((double)n - want) <= 1.0);
}

static void check_passband(void)
{
	static const double freqs[] = { 440.0, 1000.0, 5000.0, 8000.0, 12000.0 };
	int ok = 1;

	for (size_t i = 0; i < sizeof freqs / sizeof freqs[0]; i++)
	{
		tone_hz = freqs[i];
		tone_phase = 0.0;

		const unsigned n = run_signal(gen_tone, 0);
		const double db = 20.0 * log10((rms_amplitude(out_new, n, 4096) + 1e-9) /
					       TONE_AMP);

		printf("  %6.0f Hz: %6.2f dB\n", freqs[i], db);

		/* Flat to 5 kHz; the 13.5 kHz corner means the top of the band is
		 * deliberately soft (see include/decimator.h), and 12 kHz -3.8 dB
		 * is that corner doing its job. */
		const double limit = (freqs[i] <= 5000.0) ? 0.3 : 4.5;

		if (fabs(db) > limit)
			ok = 0;
	}

	check("the passband keeps the programme's amplitude", ok);
}

static void check_dc(void)
{
	const unsigned n = run_signal(gen_dc, 0);
	int32_t lo = out_new[1024], hi = out_new[1024];

	for (unsigned i = 1024; i < n; i++)
	{
		if (out_new[i] < lo) lo = out_new[i];
		if (out_new[i] > hi) hi = out_new[i];
	}

	printf("DC 1234 in, %d..%d out\n", (int)lo, (int)hi);
	check("a constant comes out unchanged", lo >= 1233 && hi <= 1235);
}

static void check_full_scale(void)
{
	const unsigned n = run_signal(gen_square, 0);
	int32_t peak = 0, trough = 0;

	for (unsigned i = 4096; i < n; i++)
	{
		if (out_new[i] > peak) peak = out_new[i];
		if (out_new[i] < trough) trough = out_new[i];
	}

	printf("+-30000 square in, %d..%d out\n", (int)trough, (int)peak);
	check("a full-scale square keeps its level and does not wrap",
	      peak > 25000 && trough < -25000 && peak < 40000 && trough > -40000);
}

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void timing(void)
{
	static uint32_t in[FRAME];
	static int16_t out[MAX_OUT * 2], mid[FRAME / 8 + 2];
	decimator d;
	ref_stage s1, s2;
	unsigned phase = 0;
	const int frames = 2000;

	decimator_reset(&d);
	memset(in, 0, sizeof in);
	gen_game(in, FRAME, &phase);

	const double t0 = now_ms();
	for (int f = 0; f < frames; f++)
		decimator_run(&d, in, FRAME, out);
	const double t1 = now_ms();

	ref_reset(&s1);
	ref_reset(&s2);
	for (int f = 0; f < frames; f++)
	{
		const unsigned nm = ref_run(&s1, (const int16_t *)in, FRAME, mid);

		ref_run(&s2, mid, nm, out);
	}
	const double t2 = now_ms();

	const double now = (t1 - t0) / frames;
	const double was = (t2 - t1) / frames;

	printf("this filter  %6.3f ms/frame\n", now);
	printf("box filters  %6.3f ms/frame  (%.2fx)\n", was, was / now);
	printf("(x86, indicative only - the console's `dec=` is the measurement)\n");
}

int main(void)
{
	check_block_sizes();
	check_rate();
	check_passband();
	check_dc();
	check_full_scale();

	printf("\naliasing: a tone above the output's Nyquist coming back in band\n");
	printf("(0 dB is a full-scale 12 000 tone; lower is better)\n");

	measure_alias("20 kHz", 20000.0, 1);
	measure_alias("50 kHz", 50000.0, 1);
	measure_alias("200 kHz", 200000.0, 1);
	measure_alias("300 kHz", 300000.0, 1);
	measure_alias("700 kHz", 700000.0, 1);

	/* The assertion is on the measurement, not on the design. A tone at
	 * 50 kHz is well inside the stopband: the box filters passed it at about
	 * -6 dB, i.e. as loudly as the music, folded into the band; this filter
	 * should bury it. */
	tone_hz = 50000.0;
	tone_phase = 0.0;
	{
		const unsigned n = run_signal(gen_tone, 1);
		const double ours = 20.0 * log10((rms_amplitude(out_new, n, 4096) + 1e-9) /
						 TONE_AMP);
		const double theirs = 20.0 * log10((rms_amplitude(out_old, n, 4096) + 1e-9) /
						   TONE_AMP);

		printf("\n50 kHz tone: this filter %.1f dB, box filters %.1f dB\n",
		       ours, theirs);
		check("a 50 kHz tone is buried (below -50 dB)", ours < -50.0);
		check("... and by a wide margin over the box filters", theirs > -20.0);
	}

	printf("\n");
	timing();

	printf("\ndecimator: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}

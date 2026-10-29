/**
 * Does the sound survive a console that is not quite full speed?
 *
 * audio.c keeps a FIFO between the emulator and CSND, and include/resamp.h
 * keeps the two ends balanced by stretching or squeezing the stream. The
 * emulator produces 548.625 samples per *emulated* frame whatever the console
 * is doing; the channel consumes 32768 samples per *wall-clock* second. A
 * console running at 59.7275 frames/s makes those the same number. A console
 * running at 57 makes them differ by 4.8 %, and something has to give.
 *
 * audiotest.c measures the block scheduler at full speed. This measures the
 * other half - the rate matcher - by running the real audio.c and resamp.c at
 * a chosen speed in real time, exactly as emu_gambatte.cpp drives them (17
 * runFor() chunks per frame, a resamp_set_step() before each one).
 *
 * What matters is not "did it work" but *how* it gave: a stream that has to
 * stretch by 4.8 % and is clamped at 4 % drains and underruns; a control law
 * fed by a sawtooth queue level wobbles the pitch at the block rate. Both show
 * up here as numbers - the spread of the step (pitch wobble) and the queue.
 *
 *   make audiospeed                     # 100 %, 97 %, 95 %, 105 %
 *   ./gbabe-audiospeed 0.95 6           # 95 %, 6 seconds
 *
 * MIT licensed.
 */
#include <3ds.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio.h"
#include "resamp.h"

#define FULL_FPS     59.7275
#define NOMINAL_RATE 32768.0
/* 35112 stereo samples a frame from the core, 2064 per runFor() call. */
#define IN_PER_FRAME (NOMINAL_RATE / FULL_FPS)
#define CHUNK_FRAMES 33        /* 2064 / 64, rounded up: the decimator's output */
#define TICKS_TO_NS(t) ((s64)((u64)(t) * 1000000000ull / SYSCLOCK_ARM11))

static s16 in[CHUNK_FRAMES * 2];
static s16 played[96 * 2];
static resamp shaper;

/* Fractional input frames owed to the emulator, carried across frames so the
 * long-run rate is 548.625 and not 548 or 549. */
static double owed_in;

static int feed_frame(double speed, unsigned *pushed_in, unsigned *pushed_out,
		      int *step_min, int *step_max, long long *step_sum,
		      long double *step_sq, unsigned *step_n,
		      unsigned *queue_min, unsigned *queue_max)
{
	owed_in += IN_PER_FRAME;
	unsigned want = (unsigned)owed_in;
	owed_in -= want;

	unsigned done = 0;
	while (done < want)
	{
		unsigned n = want - done;
		if (n > CHUNK_FRAMES)
			n = CHUNK_FRAMES;

		/* Same order as emu_gambatte.cpp: ask the control law, then
		 * resample. It is asked once per chunk, which is what sets the
		 * loop's time constant. */
		const int32_t step = audio_rate_step();

		if (step < *step_min)
			*step_min = step;
		if (step > *step_max)
			*step_max = step;
		*step_sum += step;
		*step_sq += (long double)step * step;
		(*step_n)++;

		resamp_set_step(&shaper, step);

		const unsigned out = resamp_run(&shaper, in, n, played,
						sizeof played /
						(2 * sizeof played[0]));

		if (out > 0)
		{
			audio_push_frames(played, out);
			*pushed_out += out;
		}

		*pushed_in += n;
		done += n;

		const unsigned q = audio_queue_ms();
		if (q < *queue_min)
			*queue_min = q;
		if (q > *queue_max)
			*queue_max = q;
	}

	(void)speed;
	return 0;
}

static int run(double speed, int seconds)
{
	g_host_csnd_reset();
	resamp_reset(&shaper);
	owed_in = 0.0;

	static s16 wave;
	for (unsigned i = 0; i < sizeof in / sizeof in[0]; i += 2)
	{
		wave = (s16)(((i / 2) / 16 & 1) ? 4000 : -4000);
		in[i] = wave;
		in[i + 1] = wave;
	}

	if (!audio_init())
	{
		printf("audiospeed: audio_init() failed - is GBABE_HOST_AUDIO=1 "
		       "set?\n");
		return 2;
	}

	const u64 frame_ticks = (u64)(SYSCLOCK_ARM11 / (FULL_FPS * speed));
	const unsigned frames = (unsigned)(FULL_FPS * speed * (double)seconds);

	unsigned pushed_in = 0, pushed_out = 0;
	int step_min = RESAMP_UNITY, step_max = RESAMP_UNITY;
	long long step_sum = 0;
	long double step_sq = 0.0L;
	unsigned step_n = 0;
	unsigned queue_min = 1000, queue_max = 0;
	/* Second half only: after the FIFO and the rate estimate have settled. */
	int ss_step_min = RESAMP_UNITY, ss_step_max = RESAMP_UNITY;
	long long ss_step_sum = 0;
	long double ss_step_sq = 0.0L;
	unsigned ss_step_n = 0;
	unsigned ss_queue_min = 1000, ss_queue_max = 0;

	const u64 start = svcGetSystemTick();
	u64 next_frame = start + frame_ticks;

	for (unsigned f = 0; f < frames; f++)
	{
		for (;;)
		{
			const u64 now = svcGetSystemTick();
			if (now >= next_frame)
				break;

			u64 wake = next_frame;
			const u64 audio_at = audio_next_deadline();
			if (audio_at != 0 && audio_at < wake)
				wake = audio_at;

			if (wake > now)
				svcSleepThread((s64)TICKS_TO_NS(wake - now));
			else
				svcSleepThread(200000);

			audio_pump();
		}

		/* The first seconds are the stream settling: the FIFO is filled
		 * from empty and the rate estimate starts from 1:1. What the
		 * player hears is the steady state, so report that separately. */
		const int steady = (f >= frames / 2);

		feed_frame(speed, &pushed_in, &pushed_out,
			   steady ? &ss_step_min : &step_min,
			   steady ? &ss_step_max : &step_max,
			   steady ? &ss_step_sum : &step_sum,
			   steady ? &ss_step_sq : &step_sq,
			   steady ? &ss_step_n : &step_n,
			   steady ? &ss_queue_min : &queue_min,
			   steady ? &ss_queue_max : &queue_max);
		next_frame += frame_ticks;
	}

	const u64 elapsed = svcGetSystemTick() - start;

	audio_stats st;
	audio_get_stats(&st);
	audio_exit();

	const double wall_s = (double)elapsed / (double)SYSCLOCK_ARM11;
	const int gaps = g_host_csnd_report();

	const int32_t mean = (int32_t)(step_sum / (step_n ? step_n : 1));
	const double pitch = ((double)mean / RESAMP_UNITY - 1.0) * 100.0;
	const double spread = ((double)(step_max - step_min) / RESAMP_UNITY) * 100.0;

	printf("audiospeed %.2fx: %u frames in %.2f s (%.2f fps), in %u out %u\n",
	       speed, frames, wall_s, frames / wall_s, pushed_in, pushed_out);
	printf("audiospeed %.2fx: step %.4f (pitch %+.2f %%), wobble %.2f %% "
	       "(min %.4f max %.4f), queue %u-%u ms\n",
	       speed, (double)mean / RESAMP_UNITY, pitch, spread,
	       (double)step_min / RESAMP_UNITY, (double)step_max / RESAMP_UNITY,
	       queue_min, queue_max);
	printf("audiospeed %.2fx: measured %.4f, level %d of %d frames\n",
	       speed, (double)st.rate_meas_q16 / RESAMP_UNITY, st.fill_frames,
	       st.fill_target);
	/* Wobble is the standard deviation of the step, not its range: one
	 * sample from the settling period is not a wobble, and a 32 Hz
	 * modulation of the pitch is exactly what a deviation shows up in. */
	const double ss_mean = (double)ss_step_sum / (ss_step_n ? ss_step_n : 1);
	long double var = 0.0L;
	if (ss_step_n > 1)
		var = ss_step_sq / ss_step_n - (long double)ss_mean * ss_mean;
	const double ss_dev = (var > 0.0L) ? (double)sqrtl(var) : 0.0;

	printf("audiospeed %.2fx: steady step %.4f (pitch %+.2f %%), wobble "
	       "%.3f %% rms (range %.4f..%.4f), queue %u-%u ms\n",
	       speed, ss_mean / RESAMP_UNITY, (ss_mean / RESAMP_UNITY - 1.0) * 100.0,
	       ss_dev / RESAMP_UNITY * 100.0,
	       (double)ss_step_min / RESAMP_UNITY,
	       (double)ss_step_max / RESAMP_UNITY, ss_queue_min, ss_queue_max);
	printf("audiospeed %.2fx: %u underruns, %u resyncs, %u dropped, "
	       "%u late blocks\n\n",
	       speed, st.underruns, st.resyncs, st.dropped, st.late_blocks);

	return gaps;

	(void)queue_min;
}

int main(int argc, char **argv)
{
	if (argc >= 2)
	{
		const double speed = atof(argv[1]);
		const int seconds = (argc >= 3) ? atoi(argv[2]) : 6;
		return run(speed, seconds) ? 1 : 0;
	}

	static const double speeds[] = { 1.00, 0.97, 0.95, 1.05 };
	int bad = 0;

	for (unsigned i = 0; i < sizeof speeds / sizeof speeds[0]; i++)
		bad |= run(speeds[i], 4);

	return bad ? 1 : 0;
}

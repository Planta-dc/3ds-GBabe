/**
 * Does the sound path keep the hardware fed?
 *
 * audio.c hands the CSND service one 1024-frame block per ear and the channel
 * stops when the block ends, so the next block has to be submitted *before* the
 * current one runs out. Doing that once per emulated frame is not good enough:
 * a frame is 16.7 ms and a block is 31.25 ms, so the submit lands up to a whole
 * frame after the deadline - a silence hole every 31 ms, which is the 32 Hz
 * stutter the sound had.
 *
 * There is no way to hear that here, so this measures it instead: the host
 * stand-in for CSND (tools/host/stub.c, enabled with GBABE_HOST_AUDIO=1)
 * pretends to play every block it is given, in real time, and records when they
 * started. What is left is to drive audio.c the way the frame loop does - a
 * frame of audio every 16.7412 ms, and an idle sleep that wakes for whichever
 * comes first, the next frame or the next block deadline - and look for holes.
 *
 * Run by `make -C tools/host audiotest` (two seconds of wall clock).
 *
 * MIT licensed.
 */
#include <3ds.h>

#include <stdio.h>

#include "audio.h"

#define FRAME_TICKS ((u64)(SYSCLOCK_ARM11 / 59.7275))
#define FRAMES 120              /* ~2 s of emulated time */
/* 32768 Hz at 59.7275 frames/s is 548.62 samples per frame, so the emulator
 * hands over 548 and 549 alternately. Pushing a flat 548 would drain the FIFO at
 * 36 samples/s, which is not what the real backend does. */
#define OUT_RATE_SAMPLES 32768.0

static s16 frame_audio[550 * 2];

/* Fill n stereo frames with a quiet square wave, so the buffers are not all
 * zero: whether the hardware plays them is not under test here, only when they
 * arrive. */
static void fill(int n)
{
	static int phase;
	for (int i = 0; i < n; i++)
	{
		const s16 v = ((phase++ / 8) & 1) ? 2000 : -2000;
		frame_audio[i * 2] = v;
		frame_audio[i * 2 + 1] = v;
	}
}

static u64 ticks_to_ns(u64 ticks)
{
	return (u64)((double)ticks * 1000000000.0 / SYSCLOCK_ARM11);
}

/* One run of the frame loop's audio rule: sleep until the next emulated frame
 * is due or the next sound block is, whichever comes first, pump, and hand over
 * a frame of samples when the frame is. `stall_at` is the frame at which to
 * spend `stall_ms` doing something else - an SD write, the menu opening - which
 * is what the console does once a second.
 *
 * `step_min` is the smallest rate the control law asked for after the stall:
 * the thing that used to be wrong was that it asked for 0.80 - a fifth
 * too fast - for a second after every resync. */
static void run(int frames, int stall_at, int stall_ms, double *step_min)
{
	const u64 start = svcGetSystemTick();
	u64 next_frame = start + FRAME_TICKS;
	double produced = 0.0;
	int pushed = 0, done = 0;
	double lowest = 1.0;

	while (done < frames)
	{
		const u64 now = svcGetSystemTick();

		if (now < next_frame)
		{
			u64 wake = next_frame;

			const u64 audio_at = audio_next_deadline();
			if (audio_at != 0 && audio_at < wake)
				wake = audio_at;

			if (wake > now)
				svcSleepThread((s64)ticks_to_ns(wake - now));
			else
				svcSleepThread(200000); /* 0.2 ms, as main.c does */

			audio_pump();
			continue;
		}

		if (done == stall_at && stall_ms > 0)
		{
			svcSleepThread((s64)stall_ms * 1000000);
			audio_pump();
		}

		produced += OUT_RATE_SAMPLES / 59.7275;
		const int n = (int)produced - pushed;
		pushed += n;

		/* The backend's order: decimate, ask the law for a step, hand the
		 * samples over. */
		const int32_t step = audio_rate_step();

		if (done > stall_at && stall_ms > 0)
		{
			const double s = (double)step / 65536.0;

			if (s < lowest)
				lowest = s;
		}

		fill(n);
		audio_push_frames(frame_audio, (unsigned)n);
		done++;
		next_frame += FRAME_TICKS;
	}

	*step_min = lowest;
}

int main(void)
{
	if (!audio_init())
	{
		printf("audiotest: audio_init() failed - is GBABE_HOST_AUDIO=1 "
		       "set?\n");
		return 2;
	}

	/* Phase 1: an undisturbed console. Nothing may go wrong at all. */
	double step_min;

	run(FRAMES, -1, 0, &step_min);

	audio_stats st;
	audio_get_stats(&st);

	int bad = g_host_csnd_report();

	printf("audiotest: %d frames, %u blocks, %u ear pairs, %u underruns, "
	       "%u resyncs, %u dropped, %u late\n", FRAMES, st.blocks,
	       st.ear_pairs, st.underruns, st.resyncs, st.dropped,
	       st.late_blocks);
	printf("audiotest: margin %d us, worst lateness %d us, queue at the end "
	       "%u ms\n", st.margin_us, st.worst_late_us, st.queue_ms);
	if (bad)
	{
		printf("audiotest: FAIL\n");
		return 1;
	}

	printf("audiotest: ok - the stream was continuous\n");

	/* Phase 2: a 30 ms hitch, once. The hole it leaves is the hitch's own -
	 * what is being checked is that the margin and the pitch come back. */
	g_host_csnd_reset();

	run(FRAMES, 30, 30, &step_min);
	audio_get_stats(&st);

	printf("\naudiotest: after a 30 ms hitch: margin %d us, lowest rate "
	       "%.3f, %u underruns, %u resyncs\n", st.margin_us, step_min,
	       st.underruns, st.resyncs);

	int ok = 1;

	if (st.margin_us >= 1900)
	{
		printf("audiotest: the margin is pinned at its ceiling - the "
		       "hitch was tracked as if it were scheduling jitter\n");
		ok = 0;
	}

	/* Some dip is right and expected: the window in which the hitch
	 * happened is thrown away, but the queue really did drain, and the law
	 * says a few percent for a fraction of a second. What is being ruled
	 * out is the 20 % clamp - `rate 0.800` for a second after every stall,
	 * fourteen times a session, which is a fifth of an octave of pitch
	 * error and the most audible thing the console did. */
	if (step_min < 0.94)
	{
		printf("audiotest: the rate dived to %.3f after the hitch - "
		       "`rate 0.800` in the console log was this\n", step_min);
		ok = 0;
	}

	printf("audiotest: %s - a hitch does not poison the stream\n",
	       ok ? "ok" : "FAIL");

	audio_exit();

	return ok ? 0 : 1;
}

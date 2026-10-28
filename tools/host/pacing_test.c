/**
 * Frame pacing arithmetic, on the host.
 *
 *   make -C tools/host pacetest
 *
 * The loop paces itself off the ARM11 tick counter at 59.7275 Hz, and the
 * cases that matter are the ones you cannot see on a console: a frame that is
 * late, a frame that is early, a stall long enough to throw the schedule away,
 * and the unsigned-underflow trap that made the resync path fire on every
 * frame (see include/pace.h).
 *
 * NOT part of the 3DS build.
 */
#include <3ds.h>

#include "gb3ds.h"
#include "pace.h"

#include <stdio.h>

/* One Game Boy frame, and a millisecond, in ARM11 ticks - the same numbers the
 * device build uses (GBABE_FRAME_TICKS / SYSCLOCK_ARM11). */
#define FRAME  ((uint64_t)GBABE_FRAME_TICKS)
#define MS     ((uint64_t)(SYSCLOCK_ARM11 / 1000))

static int failures;

static void check(const char *what, int ok)
{
	printf("%s: %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		failures++;
}

/* Run `frames` frames whose work takes `work` ticks, waking on each slot. */
static uint64_t run(pace_state *p, uint64_t t0, unsigned frames, uint64_t work)
{
	uint64_t now = t0;

	for (unsigned i = 0; i < frames; i++)
	{
		now += pace_wait(p, now);
		if (!pace_due(p, now, FRAME))
		{
			printf("  !! pace_due() said no at %llu\n",
			       (unsigned long long)now);
			break;
		}
		now += work;
	}

	return now;
}

static void test_steady_state(void)
{
	pace_state p = { 0 };
	const uint64_t t0 = 1000 * FRAME;

	pace_restart(&p);
	run(&p, t0, 10, 0);
	run(&p, t0 + 10 * FRAME, 10, 0);

	check("twenty on-time frames are twenty frame periods",
	      p.next == t0 + 20 * FRAME);
	check("nothing was late", p.late == 0);
	check("nothing was resynced", p.resyncs == 0);
}

/* A full minute of frames: the rate has to hold and the schedule must not
 * drift by more than the rounding of the last frame. */
static void test_one_minute(void)
{
	pace_state p = { 0 };
	const uint64_t t0 = 7;

	run(&p, t0, 3583, 0);

	const double seconds = (double)(p.next - t0) / (double)SYSCLOCK_ARM11;
	const double hz = 3583.0 / seconds;

	printf("3583 frames in %.4f s = %.4f Hz\n", seconds, hz);
	check("a minute of frames keeps the rate within 0.01 Hz",
	      hz > 59.71 && hz < 59.74);
	check("a minute of frames has no resync", p.resyncs == 0);
	check("a minute of frames has no late frames", p.late == 0);
}

static void test_early_is_not_behind(void)
{
	pace_state p = { 0 };

	pace_restart(&p);
	check("the first frame is due at once", pace_due(&p, 5000, FRAME));

	/* Poll while early, ten thousand times, one tick at a time: none is
	 * due, and - the regression this file exists for - none may be
	 * mistaken for a frame that is enormously late. */
	int due = 0;
	for (int i = 0; i < 10000; i++)
		if (pace_due(&p, 5000 + (uint64_t)i, FRAME))
			due++;

	check("polling early never reports a frame due", due == 0);
	check("polling early never counts a resync", p.resyncs == 0);
	check("polling early never counts a late frame", p.late == 0);
	check("polling early does not move the schedule",
	      p.next == 5000 + FRAME);
}

/* One frame overruns by 12 ms. The next frame is still due at the slot it
 * always was: the loop pulls back on time instead of carrying the delay. */
static void test_one_late_frame(void)
{
	pace_state p = { 0 };

	pace_restart(&p);
	pace_due(&p, 0, FRAME);

	const uint64_t slot = p.next;

	if (!pace_due(&p, slot + 12 * MS, FRAME))
		printf("  !! second frame not due\n");

	check("a 12 ms late frame is counted", p.late == 1);
	check("a 12 ms late frame is not a resync", p.resyncs == 0);
	check("the next slot is unmoved by one late frame",
	      p.next == slot + FRAME);
}

/* Jitter: every frame is woken 3 ms after its slot. That is not the machine
 * being slow, and the schedule must not drift. */
static void test_jitter(void)
{
	pace_state p = { 0 };
	const uint64_t t0 = 12345;
	uint64_t framed = 0;
	uint64_t slot = t0;

	pace_restart(&p);
	p.next = t0;

	for (int i = 0; i < 3000; i++)
	{
		if (!pace_due(&p, slot + 3 * MS, FRAME))
			break;
		framed++;
		slot += FRAME;
	}

	check("3000 jittered frames all ran", framed == 3000);
	check("3 ms of jitter is not counted as late", p.late == 0);
	check("jitter never resyncs", p.resyncs == 0);
	check("jitter does not accumulate into the schedule",
	      p.next == t0 + 3000 * FRAME);
}

/* The lid was shut for a fifth of a second: one resync, one new schedule, and
 * no attempt to replay the lost frames back to back. */
static void test_stall(void)
{
	pace_state p = { 0 };

	pace_restart(&p);
	pace_due(&p, 0, FRAME);
	pace_due(&p, FRAME, FRAME);

	const uint64_t stalled = 200 * MS; /* the console was asleep */

	check("a frame is due after the stall", pace_due(&p, stalled, FRAME));
	check("the stall is one resync", p.resyncs == 1);
	check("the stall is counted as a late frame", p.late == 1);
	check("the new schedule starts one frame after the stall",
	      p.next == stalled + FRAME);

	run(&p, stalled + FRAME, 60, 0);
	check("the cadence returns after a resync", p.resyncs == 1);
}

/* Three frames behind is still catchable - the loop can run them back to back
 * and be on time again. Four is not: catching up would mean four frames of
 * emulation in the time of one. Check both sides of that boundary exactly. */
static void test_resync_boundary(void)
{
	pace_state p = { 0 };

	pace_restart(&p);
	pace_due(&p, 0, FRAME); /* the schedule is now at 1F */

	pace_state three = p;
	pace_due(&three, 4 * FRAME, FRAME); /* 3F after the slot */
	check("three frames behind does not resync", three.resyncs == 0);
	check("three frames behind is late", three.late == 1);

	pace_state four = p;
	pace_due(&four, 5 * FRAME, FRAME); /* 4F after the slot */
	check("four frames behind resyncs", four.resyncs == 1);
	check("the resync keeps the frame that was due", four.late == 1);
}

/* A long run: a million frames must not drift, and must not accumulate a
 * rounding error into the schedule. */
static void test_long_run(void)
{
	pace_state p = { 0 };
	const uint64_t t0 = 99;

	run(&p, t0, 1000000, 0);

	check("a million frames drift by less than one frame",
	      p.next - (t0 + 1000000 * FRAME) < FRAME);
	check("a million frames never resync", p.resyncs == 0);
	check("a million on-time frames are never late", p.late == 0);
}

/* The counters are read and cleared once a second, so the HUD sees a rate
 * rather than a running total. */
static void test_counters(void)
{
	pace_state p = { 0 };
	unsigned late = 0, resyncs = 0;

	pace_restart(&p);
	pace_due(&p, 0, FRAME);
	pace_due(&p, FRAME + 10 * MS, FRAME); /* one late */

	pace_take_counters(&p, &late, &resyncs);
	check("a late frame reaches the counter", late == 1);
	check("no resync reaches the counter", resyncs == 0);

	pace_take_counters(&p, &late, &resyncs);
	check("the counters are cleared when read", late == 0 && resyncs == 0);
}

/* Not pacing, but it lives in the same window of ticks and it was wrong for two
 * releases: SYSCLOCK_ARM11 * 30 does not fit in the 32-bit unsigned int that
 * libctru's constant has, so the autosave interval wrapped to 14 seconds. */
static void test_autosave_interval(void)
{
	const uint64_t ticks = GBABE_AUTOSAVE_TICKS;
	const double seconds = (double)ticks / (double)SYSCLOCK_ARM11;

	printf("autosave every %.2f s\n", seconds);
	check("the autosave interval is 30 s, not a wrapped 14",
	      seconds > 29.9 && seconds < 30.1);
	check("the autosave interval does not fit in 32 bits",
	      ticks > (uint64_t)0xFFFFFFFFu);
}

int main(void)
{
	test_steady_state();
	test_one_minute();
	test_early_is_not_behind();
	test_one_late_frame();
	test_jitter();
	test_stall();
	test_resync_boundary();
	test_long_run();
	test_counters();
	test_autosave_interval();

	printf("pace: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}

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

/* One frame overruns by 12 ms - more than the slip tolerance - so the slots it
 * missed are given up rather than replayed: the next frame is a whole period
 * from now, not from the slot that has already gone by. */
static void test_one_late_frame(void)
{
	pace_state p = { 0 };

	pace_restart(&p);
	pace_due(&p, 0, FRAME);

	const uint64_t slot = p.next;

	if (!pace_due(&p, slot + 12 * MS, FRAME))
		printf("  !! second frame not due\n");

	check("a 12 ms late frame is counted", p.late == 1);
	check("a 12 ms late frame gives up its slot", p.resyncs == 1);
	check("what it gave up is measured", p.slip == 12 * MS);
	check("the next frame is a whole period from when this one started",
	      p.next == slot + 12 * MS + FRAME);
	check("no frame starts less than a period after the previous one",
	      p.next - (slot + 12 * MS) == FRAME);
}

/* Sub-tolerance lateness is absorbed: the schedule stays absolute, so a loop
 * that always wakes 0.4 ms late runs at exactly the intended rate and never
 * drifts. */
static void test_small_jitter_is_absorbed(void)
{
	pace_state p = { 0 };
	const uint64_t t0 = 5000;
	uint64_t now = t0;

	pace_restart(&p);
	p.next = t0;

	for (int i = 0; i < 2000; i++)
	{
		now += pace_wait(&p, now);
		now += MS / 2; /* the sleep overshoot */
		if (!pace_due(&p, now, FRAME))
			break;
	}

	check("sub-tolerance jitter gives up nothing", p.slip == 0);
	check("sub-tolerance jitter never re-anchors", p.resyncs == 0);
	check("sub-tolerance jitter does not accumulate",
	      p.next == t0 + 2000 * FRAME);
}

/* Wakes that are consistently later than the slip tolerance. Those are not
 * hitches - there is nothing to catch up on, the loop is simply late every
 * time - and the rule re-anchors, which costs exactly the lateness per frame.
 *
 * That is a real cost and it is the reason the tolerance is 2 ms and not zero:
 * the loop sleeps to `slot - frame/8` and then polls, so its lateness on the
 * console is a few hundred microseconds (see `wait` in the log). Two
 * milliseconds is ten times that, while still being small enough that catching
 * up after a hitch is never visible. What this test pins down is the direction
 * of the error: a late-waking loop runs *slower* than the Game Boy, never
 * faster. */
static void test_late_wakes_never_run_fast(void)
{
	pace_state p = { 0 };
	const uint64_t t0 = 12345;
	uint64_t now = t0, first = 0;
	unsigned framed = 0;

	pace_restart(&p);
	p.next = t0;

	for (unsigned i = 0; i < 3000; i++)
	{
		now += pace_wait(&p, now);
		now += 3 * MS; /* woken late, every time */
		if (!pace_due(&p, now, FRAME))
			break;
		if (first == 0)
			first = now;
		framed++;
	}

	/* 3000 starts are 2999 periods, not 3000 - measuring from the first
	 * start is the only way this number means anything. */
	const double seconds = (double)(now - first) / (double)SYSCLOCK_ARM11;
	const double hz = (double)(framed - 1) / seconds;

	printf("3000 frames woken 3 ms late: %.4f Hz\n", hz);
	check("a late-waking loop still runs every frame", framed == 3000);
	check("a late-waking loop runs at the intended rate, not below it",
	      hz > 59.71 && hz <= 59.73);
	check("only the first wake is a hitch once it is the norm",
	      p.resyncs == 1);
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

/* The point of PACE_SLIP_TICKS: after a hitch the emulator must never run
 * faster than the Game Boy to catch up.
 *
 * The old rule let it run up to three frames back to back, and the console log
 * shows what that looks like from outside - whole seconds at 61.7 and 63.3 fps,
 * right after the 24 ms and 56 ms frames that caused them. A game that
 * occasionally plays at 103 % is a game that speeds up, which is what the user
 * reported.
 *
 * This drives the real loop shape - sleep to the slot, run the frame's work,
 * come back - and asserts two things: no two frames ever start closer together
 * than a frame period (minus the 2 ms tolerance), and no one-second window
 * contains more frames than 59.7275 Hz allows. */
static void test_no_catch_up(void)
{
	pace_state p = { 0 };
	const uint64_t t0 = 1000;
	const uint64_t work = 12 * MS;      /* a comfortable frame */
	const uint64_t hitch = 33 * MS;     /* the once-a-second SD write */
	uint64_t now = t0;
	static uint64_t start[400];
	unsigned n = 0;
	signed long long closest = (signed long long)FRAME;

	pace_restart(&p);

	for (unsigned i = 0; i < 400; i++)
	{
		now += pace_wait(&p, now);
		if (!pace_due(&p, now, FRAME))
			break;

		if (i > 0 && n > 0)
		{
			const signed long long gap =
				(signed long long)(now - start[n - 1]);
			if (gap < closest)
				closest = gap;
		}

		start[n++] = now;
		now += work + ((i == 30 || i == 31 || i == 32) ? hitch : 0);
	}

	check("a hitch does not make the emulator run fast",
	      closest >= (signed long long)(FRAME - PACE_SLIP_TICKS(FRAME)));

	/* Any one-second window: 400 frames at 12 ms of work plus three hitches
	 * is 7.1 s, so there is room for several windows to check. */
	unsigned worst_window = 0;

	for (unsigned a = 0; a < n; a++)
	{
		unsigned c = 0;

		for (unsigned b = a; b < n && start[b] - start[a] < SYSCLOCK_ARM11;
		     b++)
			c++;

		if (c > worst_window)
			worst_window = c;
	}

	printf("busiest second after the hitches: %u frames "
	       "(the Game Boy allows 59)\n", worst_window);
	check("no second contains more frames than the Game Boy has",
	      worst_window <= 60);

	printf("closest two frame starts: %.3f ms (a frame is %.3f ms)\n",
	       (double)closest * 1000.0 / (double)SYSCLOCK_ARM11,
	       (double)FRAME * 1000.0 / (double)SYSCLOCK_ARM11);

	check("the hitches were given up, not replayed", p.slip > 0);
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
	uint64_t slip = 0;

	pace_restart(&p);
	pace_due(&p, 0, FRAME);
	pace_due(&p, FRAME + 10 * MS, FRAME); /* one late */

	pace_take_counters(&p, &late, &resyncs, &slip);
	check("a late frame reaches the counter", late == 1);
	check("a late frame is a slot given up", resyncs == 1);
	check("the time it cost reaches the counter", slip == 10 * MS);

	pace_take_counters(&p, &late, &resyncs, &slip);
	check("the counters are cleared when read",
	      late == 0 && resyncs == 0 && slip == 0);
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

/* The idle wait: how long the loop sleeps between frames, and what it is
 * willing to wake for.
 *
 * This is the arithmetic that stands in for blocking on the panel's vblank
 * (§14). The two ways it could go wrong are quiet ones: a deadline already in
 * the past turning into a zero-length sleep (a busy spin, which on a handheld
 * is heat and battery), and the swap poll delaying the next frame instead of
 * fitting inside the wait. */
static void test_idle_wake(void)
{
	const uint64_t now = 1000000000ull;
	const uint64_t frame_at = now + 4 * FRAME;
	const uint64_t audio_at = now + 3 * FRAME;
	const uint64_t slice = FRAME / 64;

	/* Nothing pending: wake exactly at the frame. */
	check("with nothing pending the loop wakes at the next frame",
	      pace_wake(now, frame_at, 0, false, FRAME) == frame_at);

	/* A sound block due before the frame comes first. */
	check("a sound deadline before the frame is woken for",
	      pace_wake(now, frame_at, audio_at, false, FRAME) == audio_at);

	/* A sound deadline after the frame is not: the frame is first. */
	check("a sound deadline after the frame does not delay it",
	      pace_wake(now, frame_at, frame_at + FRAME, false, FRAME) ==
		      frame_at);

	/* The swap poll sleeps in slices, well short of the frame. */
	check("a pending swap wakes the loop in a slice",
	      pace_wake(now, frame_at, 0, true, FRAME) == now + slice);

	/* ...but never past the frame: a swap in flight is not worth a frame. */
	check("a pending swap does not delay the next frame",
	      pace_wake(now, now + slice / 2, 0, true, FRAME) ==
		      now + slice / 2);

	/* A deadline already in the past must not become a negative sleep. */
	check("a deadline in the past never returns a time before now",
	      pace_wake(now, now - FRAME, now - FRAME, false, FRAME) == now);
	check("a past sound deadline cannot spin the loop",
	      pace_wake(now, frame_at, now - FRAME, false, FRAME) == now);

	/* frame_ticks 0 cannot happen, but a division by it would be a crash. */
	check("a zero frame length does not divide by zero",
	      pace_wake(now, frame_at, 0, true, 0) == frame_at);

	/* Over a simulated second the loop must not spin: every wake is far
	 * enough ahead that svcSleepThread() actually sleeps. */
	{
		uint64_t t = now;
		int spins = 0;
		for (int i = 0; i < 200; i++)
		{
			const uint64_t w = pace_wake(t, t + FRAME, 0, true, FRAME);
			if (w <= t)
				spins++;
			else
				t = w;
		}
		check("polling a pending swap never busy-spins", spins == 0);
	}
}

int main(void)
{
	test_steady_state();
	test_one_minute();
	test_early_is_not_behind();
	test_one_late_frame();
	test_small_jitter_is_absorbed();
	test_late_wakes_never_run_fast();
	test_stall();
	test_no_catch_up();
	test_long_run();
	test_counters();
	test_autosave_interval();
	test_idle_wake();

	printf("pace: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}

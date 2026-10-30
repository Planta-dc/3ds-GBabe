/**
 * Frame pacing: when is the next emulated frame due?
 *
 * The Game Boy runs at 59.7275 Hz and the 3DS panel at 60 Hz, so the loop
 * cannot follow vblank - over a minute that drifts by half a second. It paces
 * itself off the ARM11 tick counter instead, on an absolute schedule: frame N
 * is due at `start + N * 16.744 ms`, and a frame that finishes early sleeps
 * until its slot, so the errors do not accumulate.
 *
 * This used to live inside main.c's frame_due(), where it could not be tested -
 * and it had a bug that only showed up in the numbers: the "fell too far
 * behind" test compared tick counts with an unsigned subtraction,
 *
 *     if (now - next_frame_tick > 4 * FRAME_TICKS)   // re-anchor
 *
 * which is *negative* logic in unsigned arithmetic. In the normal case the
 * frame starts slightly before the following slot, so `now - next_frame_tick`
 * wraps to about 2^64 and the test fired on every single frame. The schedule
 * was therefore re-anchored at the top of every frame, which quietly turned the
 * absolute schedule into "16.7 ms after the previous frame started": a frame
 * that overran shifted every frame after it, and the resync counter (which the
 * log now prints) counted 100 % of frames.
 *
 * Everything here is integer arithmetic on ticks, so it is tested on the host
 * against the real frame rate and real stall lengths - see
 * tools/host/pacetest.c.
 *
 * MIT licensed.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* How a frame may be late before it is worth counting. Half a frame is jitter
 * (the sleep granularity is coarser than 16.7 ms); a whole frame means the
 * previous frame overran its budget. */
#define PACE_LATE_TICKS(frame_ticks) ((frame_ticks) / 2)

/* How late a frame may start and still be caught up.
 *
 * Catching up means starting the next frame early, which is the emulator
 * running *faster* than the Game Boy: three frames of catch-up is three frames
 * played at up to 4x speed, back to back, right after a hitch. The console log
 * shows it - one-second windows at 61.7 and 63.3 fps immediately after a 24 ms
 * or 56 ms frame - and that is the "it sometimes speeds up" the user hears.
 *
 * So a frame that starts later than usual by more than this much is treated as
 * a hitch: the slots it missed are given up rather than replayed, and the next
 * frame is scheduled a full period from *now*, so the emulator never runs
 * faster than 100 %. The game clock lags the wall clock by the hitch,
 * permanently - the right way round, because the game's own speed is the thing
 * that must not lie. The sound is generated from the game's clock, so the pitch
 * does not move either way: re-anchoring shifts the phase once, it does not
 * slow the steady state.
 *
 * "Later than usual" and not "later than zero" is the whole of it. A loop that
 * wakes 0.4 ms after every deadline is not having hitches - it is on time, on
 * this hardware - and re-anchoring on that would cost 0.4 ms per frame, i.e.
 * 2.4 % slow, for ever. So the comparison is against `late_ref`, a slow average
 * of how late frames actually start here: steady lateness costs nothing, a jump
 * above it is a hitch and only the jump is given up.
 *
 * An eighth of a frame is 2.09 ms - ten times the wake-up overshoot this loop
 * actually sees (`wait - frame/8` in frame_due(), then a poll), small enough
 * that catching one up is invisible. */
#define PACE_SLIP_TICKS(frame_ticks) ((frame_ticks) / 8)

typedef struct
{
	uint64_t next;    /* tick the next frame is due; 0 = no schedule yet */
	unsigned late;    /* frames that started a whole frame or more late   */
	unsigned resyncs; /* schedules re-anchored instead of caught up       */
	uint64_t slip;    /* ticks of slot time given up rather than replayed */
	uint64_t late_ref;/* what starting on time looks like on this console */
} pace_state;

/* Forget the schedule but keep the counters: the loop was stopped (menu, ROM
 * load) and the next frame should start as soon as it is asked for. */
static inline void pace_restart(pace_state *p)
{
	p->next = 0;
	p->late_ref = 0;
}

/* The counters are read and cleared once a second by the HUD. */
static inline void pace_take_counters(pace_state *p, unsigned *late,
				      unsigned *resyncs, uint64_t *slip)
{
	if (late != NULL)
		*late = p->late;
	if (resyncs != NULL)
		*resyncs = p->resyncs;
	if (slip != NULL)
		*slip = p->slip;
	p->late = p->resyncs = 0;
	p->slip = 0;
}

/* Ticks until the next frame's slot; 0 if it is already due. */
static inline uint64_t pace_wait(const pace_state *p, uint64_t now)
{
	return (p->next > now) ? p->next - now : 0;
}

/* Is a frame due at `now`? Advances the schedule when it is, and counts a late
 * frame or a resync as it goes. `frame_ticks` is one Game Boy frame in ticks. */
static inline bool pace_due(pace_state *p, uint64_t now, uint64_t frame_ticks)
{
	if (p->next == 0)
		p->next = now; /* first frame after a restart: due now */

	if (now < p->next)
		return false; /* still early: the caller sleeps */

	const uint64_t slot = p->next;
	const uint64_t late_by = now - slot;

	/* The frame after this one is normally due one period after the slot
	 * this one was meant to use: an absolute schedule, so a sleeping loop
	 * that wakes late does not accumulate into the frame rate. */
	p->next = slot + frame_ticks;

	/* Started long after the slot it was meant to use, i.e. the previous
	 * frame overran. */
	if (late_by > PACE_LATE_TICKS(frame_ticks))
		p->late++;

	/* Later than this console usually starts, by more than the tolerance:
	 * a hitch. Keeping the schedule would mean replaying the backlog - the
	 * loop would start the next frame immediately, and the one after that,
	 * until it had caught up, which is the emulator running faster than the
	 * Game Boy. Give those slots up instead and start the next frame a
	 * whole period from now, so the emulator never exceeds its intended
	 * speed. `slip` says what that cost, `late_ref` restarts from what this
	 * frame's start says about the machine. */
	if (late_by > p->late_ref + PACE_SLIP_TICKS(frame_ticks))
	{
		p->slip += late_by - p->late_ref;
		p->next = now + frame_ticks;
		p->resyncs++;
		p->late_ref = late_by;
	}
	else if (late_by > p->late_ref)
	{
		p->late_ref += (late_by - p->late_ref) / 8;
	}
	else
	{
		p->late_ref -= (p->late_ref - late_by) / 8;
	}

	return true;
}

/* When the loop should wake from its idle wait.
 *
 * Between frames the loop is asleep until one of three things is worth waking
 * for, whichever comes first:
 *
 *   frame_at   the next emulated frame is due (pace_wait() above);
 *   audio_at   a sound block has to be submitted, or the FIFO runs dry;
 *   swap_pending  the panel has not taken the finished frame yet, in which case
 *              it is checked every `frame_ticks / 64` (~0.26 ms) rather than
 *              waited for. That is the vblank wait, spent out of the idle time
 *              instead of out of the next frame (docs/PORT-NOTES.md §14).
 *
 * The result may already have passed (`now` is returned in that case), which the
 * caller has to handle with a short sleep - waking is never deferred past
 * `frame_at`, so a pending swap cannot delay a frame, and a deadline in the past
 * cannot spin the loop on a zero sleep.
 *
 * `audio_at` 0 means "no deadline". `frame_ticks` is one Game Boy frame.
 * Pure arithmetic, so tools/host/pacing_test.c can hold it to the cases that
 * cannot be provoked on demand in a live loop.
 */
static inline uint64_t pace_wake(uint64_t now, uint64_t frame_at,
				 uint64_t audio_at, bool swap_pending,
				 uint64_t frame_ticks)
{
	uint64_t wake = (frame_at > now) ? frame_at : now;

	if (audio_at != 0 && audio_at < wake)
		wake = audio_at;

	if (swap_pending && frame_ticks > 0)
	{
		const uint64_t slice = frame_ticks / 64;

		if (slice > 0 && now + slice < wake)
			wake = now + slice;
	}

	/* A deadline that has already passed returns `now`, never something
	 * earlier: the caller turns that into a short sleep, and a wake time
	 * behind the clock would turn every iteration into one - a spin, which
	 * on a handheld is heat and battery for nothing. */
	return (wake > now) ? wake : now;
}

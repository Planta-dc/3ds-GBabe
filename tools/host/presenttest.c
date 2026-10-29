/**
 * The present path: wait for the panel, or get out of the way.
 *
 *   make -C tools/host presenttest
 *
 * video_end_frame() swaps the finished frame in; video_present_sync() waits for
 * the vblank that takes it, which is what keeps the picture from tearing (the
 * host stand-in counts a frame that asks for the top framebuffer while a swap
 * is still pending). The split matters because the wait is dead time: the
 * once-a-second work does not fit in the loop otherwise, and doing it after the
 * wait cost a dropped frame every second on the console. Doing it between the
 * two calls is free.
 *
 * A frame that has already overrun its own 16.74 ms budget skips the wait and
 * presents at once: one torn frame is better than halving the frame rate
 * exactly when the console is struggling.
 *
 * Both halves of that trade are tested here, because neither can be checked by
 * looking at a still picture and the second one only happens when the machine
 * is already late.
 *
 * Takes about a tenth of a second of wall clock: the overrun has to be real
 * time, or svcGetSystemTick() would not see it.
 *
 * NOT part of the 3DS build.
 */
#include <3ds.h>

#include <stdio.h>

#include "gb3ds.h"
#include "video.h"

static int failures;

static void check(const char *what, int ok)
{
	printf("%s: %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		failures++;
}

/* One frame that takes `work_us` microseconds of emulation, and `gap_us` of
 * once-a-second work between the swap and the wait for the panel. */
static void frame_gap(unsigned work_us, unsigned gap_us)
{
	static uint16_t picture[144 * 160];

	video_begin_frame();
	if (work_us > 0)
		svcSleepThread((s64)work_us * 1000);
	video_draw_frame(picture, 160);
	video_end_frame();

	if (gap_us > 0)
		svcSleepThread((s64)gap_us * 1000); /* ui_idle_work() + log flush */

	video_present_sync();
}

static void frame(unsigned work_us) { frame_gap(work_us, 0); }

int main(void)
{
	unsigned skipped, late;
	int tears;

	gfxInitDefault();
	video_init();
	video_set_scale(SCALE_FIT);

	/* --- on time: the wait happens, nothing tears ------------------------ */
	frame(0);
	frame(0);

	video_sync_take(&skipped, &late);
	tears = g_host_tears;

	printf("two quick frames: sync_skip=%u tears=%d\n", skipped, tears);
	check("an on-time frame does not skip the wait", skipped == 0);
	check("an on-time frame does not tear", tears == 0);

	/* --- over budget: present at once, and say so ------------------------ */
	frame(20000); /* 20 ms of work in a 16.74 ms period */

	video_sync_take(&skipped, &late);
	printf("a frame 3 ms over budget: sync_skip=%u\n", skipped);
	check("a frame over its own period skips the wait", skipped == 1);

	/* And because it skipped, the next frame starts on a buffer the panel
	 * has not taken yet: that is the tear, and it is deliberate. Getting
	 * here means the console was already too slow for the frame. */
	frame(0);
	printf("tears after the overrun: %d\n", g_host_tears);
	check("an overrun frame is allowed to tear, and counted",
	      g_host_tears > 0);

	/* --- work in the gap between the swap and the vblank is free -------- */
	/* 6 ms of redraw/log work per frame, which the loop does not have spare
	 * after the wait: it has to happen before the wait, where the panel
	 * absorbs it. The check is that a frame with the work in the gap still
	 * presents and still does not tear. */
	frame_gap(0, 6000);
	frame_gap(0, 6000);
	video_sync_take(&skipped, &late);
	printf("two frames with 6 ms in the gap: sync_skip=%u tears=%d\n",
	       skipped, g_host_tears);
	check("work in the gap does not make a frame skip the wait", skipped == 0);

	/* --- a caller that forgets to sync cannot tear ---------------------- */
	/* video_present_sync() is called by main.c for the reason above, but
	 * nothing else in the tree calls it: video_begin_frame() has to do it,
	 * or the host tools and the menus would draw into a buffer the panel
	 * still has. Drop that call and this check fails. */
	{
		static uint16_t picture[144 * 160];
		const int before = g_host_tears;

		video_begin_frame();
		video_draw_frame(picture, 160);
		video_end_frame();      /* swapped, not waited for */
		video_begin_frame();    /* must wait for it itself */
		video_draw_frame(picture, 160);
		video_end_frame();
		video_present_sync();

		printf("tears from an un-synced caller: %d\n",
		       g_host_tears - before);
		check("video_begin_frame() waits for a pending swap",
		      g_host_tears == before);
	}

	/* --- and it recovers ------------------------------------------------ */
	video_sync_take(&skipped, &late);
	frame(0);
	frame(0);

	video_sync_take(&skipped, &late);
	printf("two frames after the overrun: sync_skip=%u\n", skipped);
	check("the wait comes back on the next on-time frame", skipped == 0);

	video_exit();
	printf("present: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}

/**
 * The present path: wait for the panel, or get out of the way.
 *
 *   make -C tools/host presenttest
 *
 * video_end_frame() swaps the finished frame in and then waits for the vblank
 * that takes it, which is what keeps the picture from tearing (the host
 * stand-in counts a frame that asks for the top framebuffer while a swap is
 * still pending). But that wait costs a whole panel period, so a frame that has
 * already overrun its own 16.74 ms budget skips it and presents at once: one
 * torn frame is better than halving the frame rate exactly when the console is
 * struggling.
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

/* One frame that takes `work_us` microseconds between begin and end. */
static void frame(unsigned work_us)
{
	static uint16_t picture[144 * 160];

	video_begin_frame();
	if (work_us > 0)
		svcSleepThread((s64)work_us * 1000);
	video_draw_frame(picture, 160);
	video_end_frame();
}

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

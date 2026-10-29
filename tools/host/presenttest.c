/**
 * The present path: what a frame waits for, and when it is free.
 *
 *   make -C tools/host presenttest
 *
 * The contract video.c offers the frame loop:
 *
 *   video_begin_frame()   hands back a framebuffer, and does not hand back one
 *                         the panel is still scanning out. If the previous
 *                         swap has already been taken at a vblank it costs
 *                         nothing (`sync_free`); if not, it waits for the
 *                         vblank that takes it.
 *   video_end_frame()     flushes and swaps, and remembers the swap is pending.
 *
 * The order matters and is what main.c does: emulation first, panel last. A
 * frame that arrives at the present early waits - which is fine, the picture is
 * ready and the wait is the frame's own slack - and a frame that arrives late
 * finds the swap long taken and does not wait at all. Neither case can tear,
 * which is the difference from how this file used to be written: that version
 * had a "present at once and accept a torn frame" path for frames that
 * overran, and on hardware every frame overran, because the wait was at the top
 * of the frame instead of the end.
 *
 * The host stand-in counts a tear whenever the top framebuffer is handed out
 * while a swap is still pending, so a torn frame fails here instead of on a
 * television.
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

/* One frame in the frame loop's order: `work_us` of emulation (there is no
 * emulator here, so a sleep stands in for it), then the present, then the
 * once-a-second work in the gap after the swap. */
static void frame_gap(unsigned work_us, unsigned gap_us)
{
	static uint16_t picture[144 * 160];

	if (work_us > 0)
		svcSleepThread((s64)work_us * 1000);

	video_begin_frame();
	video_draw_frame(picture, 160);
	video_end_frame();

	if (gap_us > 0)
		svcSleepThread((s64)gap_us * 1000); /* ui_idle_work() + log flush */
}

static void frame(unsigned work_us) { frame_gap(work_us, 0); }

int main(void)
{
	unsigned free_waits, late;
	int tears;

	gfxInitDefault();
	video_init();
	video_set_scale(SCALE_FIT);

	/* --- the first frame has no previous swap to wait for ---------------- */
	frame(0);

	video_sync_take(&free_waits, &late);
	printf("first frame: sync_free=%u tears=%d\n", free_waits, g_host_tears);
	check("the first frame has nothing to wait for", free_waits == 0);
	check("the first frame does not tear", g_host_tears == 0);

	/* --- the panel took the swap while the frame was emulated ------------ */
	/* This is the steady state on a console that is keeping up: the swap is
	 * posted at the end of one frame and taken by a vblank during the next
	 * one's emulation, so by the time the next present needs that buffer
	 * back it is free. */
	g_host_present_take();
	frame(0);

	video_sync_take(&free_waits, &late);
	tears = g_host_tears;
	printf("panel took the swap first: sync_free=%u tears=%d\n",
	       free_waits, tears);
	check("a swap the panel already took costs no wait", free_waits == 1);
	check("... and does not tear", tears == 0);

	/* --- the panel has not taken it: the wait is real -------------------- */
	/* No g_host_present_take() this time, so begin_frame() must block on the
	 * vblank instead of drawing into the buffer that is still on screen. */
	frame(0);

	video_sync_take(&free_waits, &late);
	printf("panel has not taken it: sync_free=%u tears=%d\n",
	       free_waits, g_host_tears);
	check("a frame that arrives early waits for the vblank",
	      free_waits == 0);
	check("... and does not tear", g_host_tears == 0);

	/* --- and the wait can be polled instead of blocked ------------------- */
	/* main.c polls this in its idle time at the top of a frame, where the
	 * wait costs nothing at all. */
	{
		static uint16_t picture[144 * 160];

		video_begin_frame();
		video_draw_frame(picture, 160);
		video_end_frame();
	}

	printf("pending after a frame: %d\n", (int)video_swap_pending());
	check("a swap is pending until the panel takes it",
	      video_swap_pending());

	g_host_present_take();
	check("the poll sees the panel take it", !video_swap_pending());
	check("the polled wait is timed from the swap",
	      video_present_wait() > 0);

	/* --- a frame over its own period ------------------------------------- */
	/* 20 ms of work in a 16.74 ms period, which is the case that used to
	 * present at once and tear. The buffer it needs is the one the previous
	 * frame swapped in; the stub has not been told the panel took it, so
	 * this frame still waits - and still does not tear. */
	frame(20000);

	video_sync_take(&free_waits, &late);
	printf("a frame 3 ms over budget: tears=%d\n", g_host_tears);
	check("an overrun frame does not tear", g_host_tears == 0);

	/* --- work in the gap between the swap and the vblank is free -------- */
	/* 6 ms of redraw/log work per frame. It happens after the swap, so the
	 * panel's own vblank absorbs it and the next frame does not pay for
	 * it. */
	g_host_present_take();
	frame_gap(0, 6000);
	g_host_present_take();
	frame_gap(0, 6000);

	video_sync_take(&free_waits, &late);
	printf("two frames with 6 ms in the gap: sync_free=%u tears=%d\n",
	       free_waits, g_host_tears);
	check("work in the gap does not make a frame wait", free_waits == 2);
	check("... and does not tear", g_host_tears == 0);

	/* --- a caller that forgets to poll cannot tear ---------------------- */
	/* video_begin_frame() has to do the waiting itself, or the menus and the
	 * host tools would draw into a buffer the panel still has. */
	{
		static uint16_t picture[144 * 160];
		const int before = g_host_tears;

		video_begin_frame();
		video_draw_frame(picture, 160);
		video_end_frame();      /* swapped, not waited for */
		video_begin_frame();    /* must wait for it itself */
		video_draw_frame(picture, 160);
		video_end_frame();

		printf("tears from an un-synced caller: %d\n",
		       g_host_tears - before);
		check("video_begin_frame() waits for a pending swap",
		      g_host_tears == before);
	}

	/* --- and it recovers ------------------------------------------------ */
	video_sync_take(&free_waits, &late);
	g_host_present_take();
	frame(0);
	frame(0);

	video_sync_take(&free_waits, &late);
	printf("two frames after the overrun: sync_free=%u tears=%d\n",
	       free_waits, g_host_tears);
	check("the free path comes back after an overrun", free_waits >= 1);
	check("nothing tore for the whole run", g_host_tears == 0);

	video_exit();
	printf("present: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}

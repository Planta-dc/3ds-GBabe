/**
 * Top screen rendering.
 *
 * The 3DS top LCD is a 240x400 panel mounted sideways, so a 400x240 landscape
 * image lives in memory column-major: the pixel at landscape (x, y), with y
 * counted from the top of the screen, is at
 *
 *     addr = (x * 240 + (239 - y)) * bytes_per_pixel
 *
 * We render the 160x144 Game Boy frame straight into that layout, one
 * destination column at a time - no intermediate buffer and no GPU involved,
 * which keeps memory traffic (and therefore power) low.
 *
 * RGB565 is what the core draws (VIDEO_RGB565), so its output is copied
 * through untouched.
 *
 * MIT licensed.
 */
#include "video.h"

#include <3ds.h>

#include <string.h>

#ifndef VIDEO_FLIP_X
#define VIDEO_FLIP_X 0 /* set to 1 if the image comes out mirrored */
#endif
#ifndef VIDEO_FLIP_Y
#define VIDEO_FLIP_Y 0 /* set to 1 if the image comes out upside down */
#endif

static u8  *fb;              /* back buffer for the current frame */
static bool fb_ready;

/* Destination geometry, recomputed when the scale mode changes. */
static int dst_x0, dst_y0, dst_w, dst_h;
static int16_t map_x[LCD_TOP_W]; /* dst column -> source column */
static int16_t map_y[LCD_TOP_H]; /* dst row    -> source row    */
static int current_scale = -1;
static u64 present_wait;     /* ticks spent waiting for the panel last frame */
/* Which framebuffers already have a black letterbox, and for which geometry.
 * The bars never change while the scale does not, so clearing them on every
 * frame is ~63 KiB of memset per frame for nothing (it is inside the figure the
 * log calls `core`, which is why it was worth measuring at all). */
static u8 *cleared_fb[2];
static int cleared_scale = -1;

/* Ticks spent in video_draw_frame() since the last video_draw_take(). This is
 * the scaled transpose, and it used to be invisible: it happens inside
 * emu_run_frame(), so the log's `core` figure silently contained it while the
 * figure called `blit` measured only the cache flush and the swap. */
static u64 draw_ticks;


/* Diagnostics for the present path, see video_sync_take(). */
static unsigned sync_free;   /* frames whose panel wait cost nothing        */
static bool sync_pending;    /* the last swap has not been waited for yet    */
static u64  swap_tick;       /* when that swap was posted                    */
static unsigned sync_late;   /* swaps the panel did not take at the next vblank */

/* One Game Boy frame, in ARM11 ticks. Used to decide whether there is time to
 * wait for the panel: see video_end_frame(). */
#define FRAME_TICKS ((u64)GBABE_FRAME_TICKS)

static void recalc_geometry(int mode)
{
	switch (mode)
	{
	case SCALE_ORIG:
		dst_w = 160;
		dst_h = 144;
		break;

	case SCALE_WIDE:
		dst_w = LCD_TOP_W;
		dst_h = LCD_TOP_H;
		break;

	case SCALE_FIT:
	default:
		/* 144 * 240/144 fills the height exactly; 160 scaled by the same
		 * factor is 266.67, rounded to 267. */
		dst_h = LCD_TOP_H;
		dst_w = (160 * LCD_TOP_H + 144 / 2) / 144;
		break;
	}

	dst_x0 = (LCD_TOP_W - dst_w) / 2;
	dst_y0 = (LCD_TOP_H - dst_h) / 2;

	for (int x = 0; x < dst_w; x++)
	{
		int sx = (x * 160) / dst_w;
		if (sx > 159)
			sx = 159;
		map_x[x] = (int16_t)sx;
	}

	for (int y = 0; y < dst_h; y++)
	{
		int sy = (y * 144) / dst_h;
		if (sy > 143)
			sy = 143;
		map_y[y] = (int16_t)sy;
	}

	current_scale = mode;
}

void video_init(void)
{
	/* Default is BGR8 for both screens; switch both to RGB565 so 16-bit
	 * palette entries - and the UI canvas - can be written directly. The
	 * bottom screen is drawn by ui_gfx.c, which swaps its own buffer. */
	gfxInitDefault();
	gfxSet3D(false);
	gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
	gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
	gfxSetDoubleBuffering(GFX_TOP, true);
	gfxSetDoubleBuffering(GFX_BOTTOM, true);

	fb = NULL;
	fb_ready = false;
	current_scale = -1;

	/* Both framebuffers come from linearAlloc(): nothing is cleared yet, and
	 * gfxInit() has already presented one of them. video_clear() draws into
	 * the back buffer and swaps, so two calls black both of them. */
	video_clear();
	video_clear();
}

void video_exit(void)
{
	gfxExit();
}

void video_set_scale(int mode)
{
	if (mode < 0 || mode >= SCALE_COUNT)
		mode = SCALE_FIT;
	if (mode != current_scale)
	{
		recalc_geometry(mode);
		video_letterbox_unknown();
	}
}

void video_letterbox_unknown(void)
{
	cleared_fb[0] = cleared_fb[1] = NULL;
	cleared_scale = -1;
}

/* Has this buffer's letterbox already been blacked at the current geometry? */
static bool letterbox_black(u8 *f)
{
	if (cleared_scale != current_scale)
		return false;

	return cleared_fb[0] == f || cleared_fb[1] == f;
}

static void letterbox_remember(u8 *f)
{
	if (cleared_scale != current_scale)
	{
		cleared_fb[0] = cleared_fb[1] = NULL;
		cleared_scale = current_scale;
	}

	if (cleared_fb[0] == NULL || cleared_fb[0] == f)
		cleared_fb[0] = f;
	else
		cleared_fb[1] = f;
}

static inline u16 *fb_pixel(int x, int y)
{
#if VIDEO_FLIP_X
	x = LCD_TOP_W - 1 - x;
#endif
#if VIDEO_FLIP_Y
	y = LCD_TOP_H - 1 - y;
#endif
	return (u16 *)&fb[((x * FB_STRIDE) + (FB_STRIDE - 1 - y)) * sizeof(u16)];
}

/* Nothing outside the picture rectangle is ever written by video_draw_frame(),
 * and libctru hands out framebuffers straight from linearAlloc() without
 * clearing them (gfxInit() does not memset). So the bars beside and above the
 * picture would show uninitialised memory - coloured static - unless they are
 * cleared. With the default "fit height" scale that is ~66 px on each side.
 *
 * The layout makes this cheap: a column of the panel is FB_STRIDE contiguous
 * halfwords, so every column left of the picture is one contiguous memset.
 * Only the pixel-perfect mode has bars above and below as well, and there the
 * whole screen is cleared instead - it is the rare mode. */
static void clear_letterbox(void)
{
	if (!fb_ready)
		return;

	const int cols_left = dst_x0;
	const int cols_right = LCD_TOP_W - dst_x0 - dst_w;

	if (dst_h != LCD_TOP_H)
	{
		memset(fb, 0, LCD_TOP_W * FB_STRIDE * sizeof(u16));
		return;
	}

	if (cols_left > 0)
		memset(fb, 0, (size_t)cols_left * FB_STRIDE * sizeof(u16));

	if (cols_right > 0)
	{
		/* Byte offset of the first column of the bar: fb_pixel() would add
		 * the row offset for a single pixel, which is not where the block
		 * starts. */
		u8 *right = &fb[(size_t)(dst_x0 + dst_w) * FB_STRIDE *
				sizeof(u16)];
		memset(right, 0, (size_t)cols_right * FB_STRIDE * sizeof(u16));
	}
}

void video_begin_frame(void)
{
	/* The buffer this hands back is the one the panel has finished with, and
	 * it has finished with it exactly when the previous swap has been taken
	 * at a vblank. Ask before waiting: the frame loop presents *after* the
	 * emulation, so by now that swap landed milliseconds ago and the
	 * question costs two registers.
	 *
	 * This call used to block unconditionally, and the loop called it at
	 * the *top* of the frame - 1 ms after the previous swap was posted,
	 * when the answer is always "not yet". Every frame therefore paid a
	 * blocking vblank on the way into the emulation: 4.3 ms measured, and
	 * it was counted inside `core` in the log (docs/PORT-NOTES.md 17).
	 *
	 * Waiting here is not wasted, because a frame that has to wait is a
	 * frame that arrived early: the emulation is already done, the picture
	 * is ready in vbuf, and the only thing left to do with the frame is
	 * put it on the panel. If the loop is genuinely late the swap has long
	 * been taken and this costs nothing.
	 *
	 * No tear can come of this either, because drawing is what needs the
	 * buffer: video_draw_frame() is called straight after, by which point
	 * the buffer really has been released. (The old code kept a
	 * "present it anyway and maybe tear" path for frames that overran a
	 * panel period; that path is gone, because a frame that overran reaches
	 * this call with nothing to wait for.) */
	if (sync_pending)
	{
		if (video_swap_pending())
			video_present_sync();
		else
			sync_free++;
	}

	fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
	fb_ready = (fb != NULL);

	if (fb_ready && !letterbox_black(fb))
	{
		clear_letterbox();
		letterbox_remember(fb);
	}
}

/* Black the top screen and show it. Used at startup, so the browser is not
 * surrounded by whatever was in the framebuffer, and while the browser is up
 * (nothing is playing then). */
void video_clear(void)
{
	/* A swap from the last game frame may still be in flight; it has to
	 * land before either buffer is written. */
	video_present_sync();

	/* The swap is followed by a wait for the same reason as in
	 * video_end_frame(): the buffer becomes the back buffer immediately but
	 * is not safe to write until the panel has taken the other one. */
	for (int pass = 0; pass < 2; pass++)
	{
		u16 *f = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
		if (f == NULL)
			return;

		const size_t bytes = LCD_TOP_W * FB_STRIDE * sizeof(u16);
		memset(f, 0, bytes);
		GSPGPU_FlushDataCache(f, bytes);

		/* Black all over, so its letterbox is black too. */
		letterbox_remember((u8 *)f);

		gfxScreenSwapBuffers(GFX_TOP, true);
		gspWaitForVBlank();
	}
}

/* Destination rows written per pass, see video_draw_frame(). Sixteen rows is
 * exactly one 32-byte cache line of a destination column (16 halfwords), so a
 * band writes every line it touches once and completely, and the source rows a
 * band needs - a twentieth of the frame - stay in the 16 KiB L1 while all
 * dst_w columns of that band are written. */
#ifndef VIDEO_BAND
#define VIDEO_BAND 16
#endif

/* The band size is a constant on the 3DS, and a variable in the host tests,
 * which walk it past every remainder (1, 3, 7, 16, 17, 23 ...) and compare the
 * result against a straight per-pixel reference. A wrong band would show up as
 * a striped picture only in the last rows of the screen, which is exactly the
 * kind of thing a still screenshot of dmg-acid2 can miss. */
#ifdef GBABE_HOST_TESTS
#define VIDEO_BAND_MAX 64
static int band_rows = VIDEO_BAND;
void video_test_set_band(int rows)
{
	band_rows = (rows > 0 && rows <= VIDEO_BAND_MAX) ? rows : VIDEO_BAND;
}
#else
#define VIDEO_BAND_MAX VIDEO_BAND
#define band_rows VIDEO_BAND
#endif

void video_draw_frame(const uint16_t *frame565, int pitch)
{
	if (!fb_ready || frame565 == NULL)
		return;

	const u64 t0 = svcGetSystemTick();

	/* Banded, and the order matters. Writing one destination column at a time
	 * from top to bottom (as this did until now) reads the 46 KiB source with a
	 * 320-byte stride between reads, 144 cache lines per column and 267
	 * columns: the source never fits in L1, so every column re-fetches all of
	 * it - ~38,000 line fetches where the frame has 1,440. Going down 16 rows
	 * at a time instead reads ten source rows (3 KiB) per pass, and those are
	 * still in cache when the next of the 267 columns asks for them: ~1,500
	 * line fetches for the whole frame, and the destination is unchanged -
	 * still one contiguous run per column, still every line written once. */
	for (int y0 = 0; y0 < dst_h; y0 += band_rows)
	{
		const int n = (dst_h - y0 < band_rows) ? dst_h - y0 : band_rows;

		/* Addresses inside a column grow as the destination row number
		 * falls (the panel is rotated), and VIDEO_FLIP_Y turns that
		 * around, so the band is built in address order rather than in
		 * row order. */
		const uint16_t *row[VIDEO_BAND_MAX];
		int first_dy;

#if VIDEO_FLIP_Y
		first_dy = dst_y0 + y0;
		for (int k = 0; k < n; k++)
			row[k] = frame565 + map_y[y0 + k] * pitch;
#else
		first_dy = dst_y0 + y0 + n - 1;
		for (int k = 0; k < n; k++)
			row[k] = frame565 + map_y[y0 + n - 1 - k] * pitch;
#endif

		for (int dx = 0; dx < dst_w; dx++)
		{
			const int sx = map_x[dx];
			u16 *dst = fb_pixel(dst_x0 + dx, first_dy);

			for (int k = 0; k < n; k++)
				*dst++ = row[k][sx];
		}
	}

	draw_ticks += svcGetSystemTick() - t0;
}

uint64_t video_draw_take(void)
{
	const u64 t = draw_ticks;

	draw_ticks = 0;
	return t;
}

void video_end_frame(void)
{
	present_wait = 0;
	sync_pending = false;

	if (!fb_ready)
		return;

	/* Only the top screen. gfxFlushBuffers() and gfxSwapBuffers() both touch
	 * *both* screens: the former flushes 153 KiB of bottom-screen framebuffer
	 * that has not changed, and the latter flips the bottom buffer out from
	 * under ui_gfx.c, which swaps that screen on its own schedule. */
	GSPGPU_FlushDataCache(fb, LCD_TOP_W * FB_STRIDE * sizeof(u16));
	gfxScreenSwapBuffers(GFX_TOP, true);

	/* The swap is pending until a vblank takes it, and the buffer it
	 * releases is the one the next frame draws into: video_begin_frame()
	 * is what waits for it, at a point in the frame where waiting is
	 * free. */
	sync_pending = true;
	swap_tick = svcGetSystemTick();

	fb_ready = false;
}

/* Has the panel taken the swap yet? Answering this is what lets the frame loop
 * put the wait where it costs nothing.
 *
 * Waiting for the vblank right after the swap was the wrong place: the Game Boy
 * frame is 16.74 ms and the panel's is 16.67 ms, so a blocking wait at the end
 * of every frame pushes the next frame past its own slot - 95 % of frames
 * started late in the console log, and the loop drifted down to 57.8 fps. The
 * panel must not gate the emulator.
 *
 * So main.c polls this in the idle time *before* the next frame - time it would
 * have slept through anyway - and stops as soon as the answer is no. The wait
 * comes out of the idle budget instead of out of the next frame. */
bool video_swap_pending(void)
{
	if (!sync_pending)
		return false;

	if (gspIsPresentPending(GFX_TOP))
		return true;

	/* The panel took it. Time the wait from the swap, not from the poll, so
	 * the figure is comparable with the blocking path's. */
	present_wait = svcGetSystemTick() - swap_tick;
	sync_pending = false;
	return false;
}

uint64_t video_present_sync(void)
{
	if (!sync_pending)
		return 0;

	/* Blocking version, for the callers outside the frame loop (menus) and
	 * as the safety net in video_begin_frame(): draw only into a buffer the
	 * panel has released. */
	const u64 now = svcGetSystemTick();

	gspWaitForVBlank();

	/* A swap posted just before a vblank can miss it by a hair, in which
	 * case the panel keeps the old buffer for another 16.7 ms and the next
	 * draw would tear. gspIsPresentPending() says whether that happened;
	 * give it one more vblank rather than guess. */
	if (gspIsPresentPending(GFX_TOP))
	{
		sync_late++;
		gspWaitForVBlank();
	}

	sync_pending = false;
	present_wait = svcGetSystemTick() - now;
	return present_wait;
}

uint64_t video_present_wait(void)
{
	return present_wait;
}

void video_sync_take(unsigned *free_waits, unsigned *late_applies)
{
	if (free_waits != NULL)
		*free_waits = sync_free;
	if (late_applies != NULL)
		*late_applies = sync_late;

	sync_free = sync_late = 0;
}

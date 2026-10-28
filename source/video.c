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
static u64 frame_start;      /* when video_begin_frame() was called          */
static bool frame_timed;     /* frame_start is set (it can legitimately be 0) */

/* Diagnostics for the present path, see video_sync_take(). */
static unsigned sync_skips;  /* frames presented without waiting for the panel */
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
		recalc_geometry(mode);
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
	frame_start = svcGetSystemTick();
	frame_timed = true;
	fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
	fb_ready = (fb != NULL);
	clear_letterbox();
}

/* Black the top screen and show it. Used at startup, so the browser is not
 * surrounded by whatever was in the framebuffer, and while the browser is up
 * (nothing is playing then). */
void video_clear(void)
{
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
		gfxScreenSwapBuffers(GFX_TOP, true);
		gspWaitForVBlank();
	}
}

/* The panel address of a pixel grows as y shrinks, so a column is written from
 * its bottom row upwards, which makes the writes sequential. See video.h. */
void video_draw_frame(const uint16_t *frame565, int pitch)
{
	if (!fb_ready || frame565 == NULL)
		return;

	for (int dx = 0; dx < dst_w; dx++)
	{
		const int x = dst_x0 + dx;
		const uint16_t *src = frame565 + map_x[dx];

#if VIDEO_FLIP_Y
		/* Mirrored vertically: the address still grows with y. */
		u16 *dst = fb_pixel(x, dst_y0);
		for (int dy = 0; dy < dst_h; dy++)
			*dst++ = src[map_y[dy] * pitch];
#else
		u16 *dst = fb_pixel(x, dst_y0 + dst_h - 1);
		for (int dy = dst_h - 1; dy >= 0; dy--)
			*dst++ = src[map_y[dy] * pitch];
#endif
	}
}

void video_end_frame(void)
{
	present_wait = 0;

	if (!fb_ready)
		return;

	/* Only the top screen. gfxFlushBuffers() and gfxSwapBuffers() both touch
	 * *both* screens: the former flushes 153 KiB of bottom-screen framebuffer
	 * that has not changed, and the latter flips the bottom buffer out from
	 * under ui_gfx.c, which swaps that screen on its own schedule. */
	GSPGPU_FlushDataCache(fb, LCD_TOP_W * FB_STRIDE * sizeof(u16));
	gfxScreenSwapBuffers(GFX_TOP, true);

	/* Now wait for the panel to take it. Until this returns, the buffer the
	 * next frame would draw into is still being scanned out; drawing into it
	 * before the swap lands is exactly the tear that used to slide down the
	 * screen (see video.h).
	 *
	 * Only if there is time. The wait ends at the next vblank, so as long as
	 * the frame's own work fits in one frame period the cadence becomes
	 * exactly the panel's 60 Hz; but a frame that has already overrun would
	 * be pushed to the *following* vblank and the game would run at 30 fps.
	 * In that case the console is behind anyway: present immediately and let
	 * it tear, which is much less bad than halving the frame rate. */
	const u64 now = svcGetSystemTick();
	const bool slack = frame_timed && now - frame_start < FRAME_TICKS;

	if (slack)
	{
		gspWaitForVBlank();

		/* A swap posted just before a vblank can miss it by a hair, in
		 * which case the panel keeps the old buffer for another
		 * 16.7 ms and the next draw would tear. gspIsPresentPending()
		 * says whether that happened; give it one more vblank rather
		 * than guess. */
		if (gspIsPresentPending(GFX_TOP))
		{
			sync_late++;
			gspWaitForVBlank();
		}
	}
	else
	{
		sync_skips++;
	}

	present_wait = svcGetSystemTick() - now;
	fb_ready = false;
	frame_timed = false;
}

uint64_t video_present_wait(void)
{
	return present_wait;
}

void video_sync_take(unsigned *skipped, unsigned *late_applies)
{
	if (skipped != NULL)
		*skipped = sync_skips;
	if (late_applies != NULL)
		*late_applies = sync_late;

	sync_skips = sync_late = 0;
}

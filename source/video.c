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

static u64 frame_start;      /* when video_begin_frame() was called          */
static bool frame_timed;     /* frame_start is set (it can legitimately be 0) */

/* Diagnostics for the present path, see video_sync_take(). */
static unsigned sync_skips;  /* frames presented without waiting for the panel */
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
	/* Safety net for every caller that draws without asking: a swap the
	 * panel has not taken yet must land before the buffer it released is
	 * written to. main.c calls video_present_sync() itself, earlier, so
	 * that its once-a-second work rides the wait; this only catches the
	 * callers that do not (the host tools, the UI). */
	video_present_sync();

	frame_start = svcGetSystemTick();
	frame_timed = true;
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
	sync_pending = false;

	if (!fb_ready)
		return;

	/* Only the top screen. gfxFlushBuffers() and gfxSwapBuffers() both touch
	 * *both* screens: the former flushes 153 KiB of bottom-screen framebuffer
	 * that has not changed, and the latter flips the bottom buffer out from
	 * under ui_gfx.c, which swaps that screen on its own schedule. */
	GSPGPU_FlushDataCache(fb, LCD_TOP_W * FB_STRIDE * sizeof(u16));
	gfxScreenSwapBuffers(GFX_TOP, true);

	/* Whether the wait is affordable is decided here, from the frame's own
	 * work - not later, from video_present_sync(), because by then the
	 * caller has had time to do its bookkeeping and the frame would look
	 * slow when it was not. A frame that has already overrun a frame period
	 * is presented at once and may tear: waiting would push it to the
	 * *following* vblank and halve the frame rate at exactly the moment the
	 * console is struggling. */
	const u64 now = svcGetSystemTick();
	const bool slack = frame_timed && now - frame_start < FRAME_TICKS;

	if (slack)
	{
		sync_pending = true;
		swap_tick = now;
	}
	else
		sync_skips++;

	fb_ready = false;
	frame_timed = false;
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

void video_sync_take(unsigned *skipped, unsigned *late_applies)
{
	if (skipped != NULL)
		*skipped = sync_skips;
	if (late_applies != NULL)
		*late_applies = sync_late;

	sync_skips = sync_late = 0;
}

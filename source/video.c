/**
 * Top screen rendering.
 *
 * The 3DS top LCD is a 240x400 panel mounted sideways, so a 400x240 landscape
 * image lives in memory column-major: the pixel at landscape (x, y), with y
 * counted from the top of the screen, is at
 *
 *     addr = (x * 240 + (239 - y)) * bytes_per_pixel
 *
 * We render the 160x144 Game Boy frame straight into that layout, line by
 * line, from the core's lcd_draw_line callback - no intermediate buffer and no
 * GPU involved, which keeps memory traffic (and therefore power) low.
 *
 * RGB565 is used so that the core's own CGB palette output
 * (WALNUT_GB_RGB565_BIGENDIAN == 0, i.e. native little-endian RGB565) can be
 * copied through untouched.
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

/* Nothing outside the picture rectangle is ever written by video_draw_line(),
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
	fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
	fb_ready = (fb != NULL);
	clear_letterbox();
}

/* Black the top screen and show it. Used at startup, so the browser is not
 * surrounded by whatever was in the framebuffer, and while the browser is up
 * (nothing is playing then). */
void video_clear(void)
{
	u16 *f = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
	if (f == NULL)
		return;

	const size_t bytes = LCD_TOP_W * FB_STRIDE * sizeof(u16);
	memset(f, 0, bytes);
	GSPGPU_FlushDataCache(f, bytes);
	gfxScreenSwapBuffers(GFX_TOP, true);
}

void video_draw_line(const uint16_t *line565, int line)
{
	if (!fb_ready)
		return;

	/* Which destination rows does this source line cover? Nearest-neighbour,
	 * so with the default 5/3 scale some lines produce one row and some two. */
	int y0 = (line * dst_h + 143) / 144;
	int y1 = ((line + 1) * dst_h + 143) / 144;
	if (y1 <= y0)
		y1 = y0 + 1;
	if (y1 > dst_h)
		y1 = dst_h;

	for (int dy = y0; dy < y1; dy++)
	{
		const int y = dst_y0 + dy;
		u16 *base = fb_pixel(dst_x0, y);
		/* Step 240 pixels per destination column, i.e. one panel row. */
		for (int dx = 0; dx < dst_w; dx++)
			base[dx * FB_STRIDE] = line565[map_x[dx]];
	}
}

/* One call per frame instead of one per scanline: see video.h. The panel
 * address of a pixel grows as y shrinks, so a column is written from its
 * bottom row upwards, which makes the writes sequential. */
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
	if (!fb_ready)
		return;

	/* Only the top screen. gfxFlushBuffers() and gfxSwapBuffers() both touch
	 * *both* screens: the former flushes 153 KiB of bottom-screen framebuffer
	 * that has not changed, and the latter flips the bottom buffer out from
	 * under ui_gfx.c, which swaps that screen on its own schedule. */
	GSPGPU_FlushDataCache(fb, LCD_TOP_W * FB_STRIDE * sizeof(u16));
	gfxScreenSwapBuffers(GFX_TOP, true);
	fb_ready = false;
}

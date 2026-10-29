/**
 * The scaled transpose, against a reference written from the specification.
 *
 *   make -C tools/host drawtest
 *
 * video_draw_frame() writes the Game Boy picture into the top panel's
 * column-major buffer - 267x240 = 64,080 writes for a 23,040-pixel picture at
 * the default scale - and it does it in bands of destination rows so the
 * source stays in cache. Banding is an optimisation that can silently produce
 * a striped or shifted picture, and neither the dmg-acid2 comparison nor a
 * glance at the screen would catch a fault that only shows up in the last rows
 * of a band with an awkward height.
 *
 * So this walks the band height over every awkward size (1, 2, 3, 7, 16, 17,
 * 23, 64 and the default), for every scale mode, at several source pitches, and
 * compares every one of the 400x240 panel pixels against a reference computed
 * here from the documented geometry - not from video.c's own arithmetic:
 *
 *     source column = (dest column * 160) / dst_w        clamped to 159
 *     source row    = (dest row    * 144) / dst_h        clamped to 143
 *
 * Anything outside the picture rectangle must stay black, which is the other
 * half of the contract: the letterbox is cleared once per buffer and must not
 * be written by a frame.
 *
 * NOT part of the 3DS build.
 */
#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb3ds.h"
#include "video.h"

static int failures;

static void check(const char *what, int ok)
{
	printf("%-40s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		failures++;
}

/* The three scale modes, as video.h documents them. */
static void geometry(int mode, int *w, int *h)
{
	switch (mode)
	{
	case SCALE_ORIG:
		*w = 160;
		*h = 144;
		break;
	case SCALE_WIDE:
		*w = 400;
		*h = 240;
		break;
	case SCALE_FIT:
	default:
		*h = 240;
		*w = (160 * 240 + 144 / 2) / 144;
		break;
	}
}

/* A source frame that is different in every pixel, so a band written from the
 * wrong row shows up immediately. */
static void fill_source(uint16_t *px, int pitch, int h)
{
	for (int y = 0; y < h; y++)
		for (int x = 0; x < pitch; x++)
			px[y * pitch + x] = (uint16_t)(0x8000u | ((y * 211 + x * 7) & 0x7FFF));
}

/* The picture the panel should be showing after one frame, in landscape
 * coordinates (400 x 240, y counted from the top of the screen), written
 * from the geometry above rather than from video.c. */
static int draw_and_compare(int mode, int pitch, int band, int *bad_pixels)
{
	static const uint16_t black = 0;
	static uint16_t src[144 * 256];
	uint16_t expect[400 * 240];

	int dst_w, dst_h;

	geometry(mode, &dst_w, &dst_h);
	fill_source(src, pitch, 144);

	const int x0 = (400 - dst_w) / 2;
	const int y0 = (240 - dst_h) / 2;

	for (int y = 0; y < 240; y++)
	{
		for (int x = 0; x < 400; x++)
		{
			if (x < x0 || x >= x0 + dst_w ||
			    y < y0 || y >= y0 + dst_h)
			{
				expect[y * 400 + x] = black;
				continue;
			}

			int sx = ((x - x0) * 160) / dst_w;
			int sy = ((y - y0) * 144) / dst_h;

			if (sx > 159)
				sx = 159;
			if (sy > 143)
				sy = 143;

			expect[y * 400 + x] = src[sy * pitch + sx];
		}
	}

	/* A fresh pair of framebuffers, letterbox cleared by video_begin_frame()
	 * the way a scale change does it. Poisoning first makes a pixel the
	 * frame forgets to write visible as 0xDEAD rather than as the value the
	 * last test left there. */
	g_host_poison_framebuffers(0xDEAD);
	video_set_scale(mode);
	video_letterbox_unknown();
#ifdef GBABE_HOST_TESTS
	video_test_set_band(band);
#endif

	video_begin_frame();
	video_draw_frame(src, pitch);
	video_end_frame();
	g_host_present_take();

	*bad_pixels = 0;
	for (int y = 0; y < 240; y++)
	{
		for (int x = 0; x < 400; x++)
		{
			if (g_host_fb[y * 400 + x] != expect[y * 400 + x])
				(*bad_pixels)++;
		}
	}

	(void)band;
	return *bad_pixels == 0;
}

int main(void)
{
	static const int modes[] = { SCALE_FIT, SCALE_WIDE, SCALE_ORIG };
	static const char *names[] = { "fit", "wide", "orig" };
	/* 160 is the core's pitch; 256 is what upstream Gambatte's libretro
	 * wrapper uses; 161 is deliberately odd, so a banding bug that assumes
	 * a power of two cannot hide. */
	static const int pitches[] = { 160, 161, 256 };
	static const int bands[] = { 1, 2, 3, 7, 16, 17, 23, 64 };

	gfxInitDefault();
	video_init();

	printf("picture, every panel pixel, every band height\n");

	for (unsigned m = 0; m < sizeof modes / sizeof modes[0]; m++)
	{
		for (unsigned p = 0; p < sizeof pitches / sizeof pitches[0]; p++)
		{
			for (unsigned b = 0; b < sizeof bands / sizeof bands[0]; b++)
			{
				int bad;
				char what[64];

				snprintf(what, sizeof what, "%s, pitch %d, band %d",
					 names[m], pitches[p], bands[b]);

				if (!draw_and_compare(modes[m], pitches[p],
						      bands[b], &bad))
				{
					printf("%-40s %d pixels wrong\n",
					       what, bad);
					failures++;
				}
			}
		}
	}

	check("the picture is identical at every band height",
	      failures == 0);

	/* And the same frame twice must give the same picture: the bands write
	 * every destination pixel exactly once, so nothing may be carried over
	 * from the frame before. */
	int bad;
	draw_and_compare(SCALE_FIT, 160, 16, &bad);
	uint16_t first[400 * 240];
	memcpy(first, g_host_fb, sizeof first);

	draw_and_compare(SCALE_FIT, 160, 16, &bad);

	check("two frames in a row give the same picture",
	      memcmp(first, g_host_fb, sizeof first) == 0);
	check("the reference picture is not blank",
	      first[120 * 400 + 200] != 0);

	printf("\ndrawtest: %s\n", failures == 0 ? "ok" : "FAILED");
	return failures == 0 ? 0 : 1;
}

/**
 * The status strip, drawn in place on the game frame - see hud.h.
 *
 * The strip is two lines of 5x7 text on an opaque 16-row bar at the top of the
 * 160x144 image. It is drawn every presented frame, onto the framebuffer the
 * core just rendered, so it can never desynchronise from the image and never
 * needs a second present. The numbers are the ones the once-a-second profile
 * refresh has just published (ui_note_frame()): frame rate, frame time and worst
 * frame of the last window on line one, machine and the two costs a frame is
 * made of on line two.
 *
 * The font is a hand-set 5x7, one byte per row with the low five bits as the
 * pixels, covering exactly the characters the two lines use; every other byte
 * of the 128-entry table is zero, so a stray character draws as blank rather
 * than as garbage.
 *
 * MIT licensed.
 */
#include "hud.h"

#include <stdio.h>
#include <string.h>

#include "emu.h"
#include "gb3ds.h"
#include "ui_gfx.h"

#define HUD_W    160
#define HUD_H    144
#define HUD_TOP  16 /* strip rows 0..15 */

/* The strip's palette, the UI's own colours: the bar is the menu background,
 * the text the menu text, and the frame rate the one number that is good or
 * bad (55 fps is full speed with margin). */
#define HUD_BG  UI_RGB(14, 17, 23)
#define HUD_FG  UI_RGB(236, 239, 244)
#define HUD_OK  UI_RGB(100, 220, 100)
#define HUD_BAD UI_RGB(255, 120, 100)

#define HUD_GLYPH_W 5
#define HUD_ADV     6 /* 5 pixels + 1 spacing */
#define HUD_LINE    8 /* 7 pixels + 1 */

static const uint8_t hud_font[128][7] = {
	['0'] = { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },
	['1'] = { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
	['2'] = { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
	['3'] = { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
	['4'] = { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
	['5'] = { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
	['6'] = { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },
	['7'] = { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
	['8'] = { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
	['9'] = { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
	['.'] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C },
	[' '] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	['B'] = { 0x18, 0x12, 0x11, 0x18, 0x12, 0x12, 0x18 },
	['C'] = { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },
	['D'] = { 0x18, 0x12, 0x11, 0x11, 0x11, 0x12, 0x18 },
	['G'] = { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F },
	['M'] = { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },
	['W'] = { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 },
	['b'] = { 0x10, 0x10, 0x18, 0x12, 0x11, 0x12, 0x18 },
	['c'] = { 0x00, 0x00, 0x0E, 0x11, 0x10, 0x11, 0x0E },
	['f'] = { 0x06, 0x08, 0x08, 0x1F, 0x08, 0x08, 0x08 },
	['i'] = { 0x04, 0x00, 0x04, 0x04, 0x04, 0x04, 0x0E },
	['m'] = { 0x00, 0x00, 0x15, 0x1F, 0x15, 0x15, 0x15 },
	['p'] = { 0x00, 0x00, 0x18, 0x12, 0x12, 0x18, 0x10 },
	['s'] = { 0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x0E },
	['t'] = { 0x08, 0x08, 0x1F, 0x08, 0x08, 0x09, 0x06 },
};

static void hud_pixel(uint16_t *fb, int pitch, int x, int y, uint16_t c)
{
	if (x < 0 || x >= HUD_W || y < 0 || y >= HUD_H)
		return;

	fb[(size_t)y * pitch + x] = c;
}

static void hud_text(uint16_t *fb, int pitch, int x, int y,
		     const char *s, uint16_t c)
{
	for (; *s != '\0'; s++)
	{
		/* A character outside the table is all zeroes: blank. */
		const uint8_t *g = hud_font[(unsigned char)*s];

		for (int r = 0; r < 7; r++)
		{
			const uint8_t bits = g[r];

			for (int b = 0; b < HUD_GLYPH_W; b++)
			{
				if (bits & (0x10 >> b))
					hud_pixel(fb, pitch, x + b, y + r, c);
			}
		}

		x += HUD_ADV;

		if (x >= HUD_W)
			return;
	}
}

void hud_draw(uint16_t *fb565, int pitch)
{
	if (fb565 == NULL || pitch < HUD_W ||
	    !g_cfg.show_hud || !emu_loaded())
		return;

	/* Opaque: the strip replaces the top rows of the image rather than
	 * blending with them, so the game underneath is simply not drawn there.
	 * (Not memset(): that takes one byte, and would fill with the low byte
	 * of the colour, not the colour.) */
	for (int y = 0; y < HUD_TOP; y++)
	{
		uint16_t *row = fb565 + (size_t)y * pitch;

		for (int x = 0; x < HUD_W; x++)
			row[x] = HUD_BG;
	}

	char l1a[8], l1b[24], l2[32];

	/* Line one: the rate, the frame time and the worst frame of the
	 * window. The rate is the one figure that is good or bad, so it gets
	 * the colour; the rest is plain text. */
	snprintf(l1a, sizeof l1a, "%4.1f", (double)g_perf.fps);
	snprintf(l1b, sizeof l1b, "fps %4.1fms W%4.1f",
		 (double)g_perf.frame_ms, (double)g_perf.worst_ms);

	hud_text(fb565, pitch, 2, 1, l1a,
		 g_perf.fps >= 55.0f ? HUD_OK : HUD_BAD);
	hud_text(fb565, pitch, 2 + HUD_ADV * (int)strlen(l1a), 1, l1b, HUD_FG);

	/* Line two: the machine and the two costs a presented frame is made of
	 * (`core` is Gambatte + the decimator, `blit` the scaled transpose).
	 * The full split is in the once-a-second log line. */
	const emu_info *info = emu_get_info();

	snprintf(l2, sizeof l2, "%s %uMbit MBC%d c%.1f b%.1f",
		 info->cgb ? "CGB" : "DMG",
		 (unsigned)(info->rom_banks * 16 / 128), info->mbc,
		 (double)g_perf.core_ms, (double)g_perf.blit_ms);

	hud_text(fb565, pitch, 2, 1 + HUD_LINE, l2, HUD_FG);
}

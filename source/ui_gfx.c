/**
 * Software rasteriser for the bottom screen UI.
 *
 * See ui_gfx.h for why this is CPU-drawn. Edges are anti-aliased with integer
 * coverage: the squared distance to the shape's edge is compared against the
 * radii r-1 and r+1 and ramped between them, so there is no square root, no
 * floating point and no look-up table to keep in sync with the radius.
 *
 * MIT licensed.
 */
#include "ui_gfx.h"

#include <3ds.h>

#include <string.h>

uint16_t ui_canvas[UI_W * UI_H];

/* ------------------------------------------------------------------------- */
/* Colour helpers                                                            */
/* ------------------------------------------------------------------------- */

/* src over dst, a in 0..256. Signed on purpose: the interpolation goes both
 * ways, and a>>8 on a negative value is defined by GCC/ARM as an arithmetic
 * shift, which is what this wants. */
static inline uint16_t blend(uint16_t dst, uint16_t src, int a)
{
	int dr = (dst >> 11) & 0x1F, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
	const int sr = (src >> 11) & 0x1F, sg = (src >> 5) & 0x3F;
	const int sb = src & 0x1F;

	dr += ((sr - dr) * a) >> 8;
	dg += ((sg - dg) * a) >> 8;
	db += ((sb - db) * a) >> 8;

	return (uint16_t)((dr << 11) | (dg << 5) | db);
}

static inline void plot(int x, int y, uint16_t c, int a)
{
	if ((unsigned)x >= UI_W || (unsigned)y >= UI_H || a <= 0)
		return;
	uint16_t *p = &ui_canvas[y * UI_W + x];
	*p = (a >= 256) ? c : blend(*p, c, a);
}

/* ------------------------------------------------------------------------- */
/* Rectangles                                                                */
/* ------------------------------------------------------------------------- */

static void clip_rect(int *x, int *y, int *w, int *h)
{
	if (*x < 0) { *w += *x; *x = 0; }
	if (*y < 0) { *h += *y; *y = 0; }
	if (*x + *w > UI_W) *w = UI_W - *x;
	if (*y + *h > UI_H) *h = UI_H - *y;
}

void ui_fill(int x, int y, int w, int h, uint16_t c)
{
	clip_rect(&x, &y, &w, &h);
	for (int j = 0; j < h; j++)
	{
		uint16_t *p = &ui_canvas[(y + j) * UI_W + x];
		for (int i = 0; i < w; i++)
			p[i] = c;
	}
}

void ui_fill_alpha(int x, int y, int w, int h, uint16_t c, int alpha)
{
	clip_rect(&x, &y, &w, &h);
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			plot(x + i, y + j, c, alpha);
}

void ui_vgradient(int x, int y, int w, int h, uint16_t top, uint16_t bottom)
{
	if (h < 1)
		return;
	for (int j = 0; j < h; j++)
		ui_fill(x, y + j, w, 1,
			blend(top, bottom, (h > 1) ? j * 256 / (h - 1) : 0));
}

void ui_clear(uint16_t c)
{
	ui_fill(0, 0, UI_W, UI_H, c);
}

/* ------------------------------------------------------------------------- */
/* Rounded rectangles, circles, arrows                                       */
/* ------------------------------------------------------------------------- */

/* 0..256 coverage of the pixel (px,py) inside a rounded rect whose corner
 * circles have radius r. Straight rows never get here (ui_rrect fills them in
 * bulk), so the one division is only paid on the edges. */
static int rrect_cov(int px, int py, int x, int y, int w, int h, int r)
{
	/* Coordinates of the pixel centre in "corner space". */
	int cx = 2 * px + 1, cy = 2 * py + 1;

	/* Distance from the nearest corner circle centre, doubled. */
	int dx = 0, dy = 0;
	if (cx < 2 * (x + r)) dx = 2 * (x + r) - cx;
	else if (cx > 2 * (x + w - r)) dx = cx - 2 * (x + w - r);
	if (cy < 2 * (y + r)) dy = 2 * (y + r) - cy;
	else if (cy > 2 * (y + h - r)) dy = cy - 2 * (y + h - r);
	if (dx == 0 && dy == 0)
		return 256;

	/* squared distance in 1/4 pixel units against (2r)^2 */
	int d2 = dx * dx + dy * dy;
	int r2 = 4 * r * r;
	if (d2 >= r2)
		return 0;

	/* Linear ramp across roughly one pixel of edge: compare against the
	 * circle of radius r-0.5 and r+0.5. */
	int inner = 4 * (r - 1) * (r - 1);
	int outer = 4 * (r + 1) * (r + 1);
	if (d2 <= inner)
		return 256;
	if (d2 >= outer)
		return 0;
	return (outer - d2) * 256 / ((outer - inner) ? (outer - inner) : 1);
}

void ui_rrect(int x, int y, int w, int h, int r, uint16_t c)
{
	if (w <= 0 || h <= 0)
		return;
	if (r <= 0)
	{
		ui_fill(x, y, w, h, c);
		return;
	}
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;

	for (int j = y; j < y + h; j++)
	{
		/* Rows that are entirely inside the straight part of the shape. */
		if (j >= y + r && j < y + h - r)
		{
			ui_fill(x, j, w, 1, c);
			continue;
		}
		for (int i = x; i < x + w; i++)
		{
			int a = rrect_cov(i, j, x, y, w, h, r);
			plot(i, j, c, a);
		}
	}
}

/* Outline = coverage of the outer shape minus coverage of the shape inset by t,
 * which gives an anti-aliased stroke without a second rasteriser. */
void ui_rrect_outline(int x, int y, int w, int h, int r, int t, uint16_t c)
{
	if (w <= 0 || h <= 0 || t <= 0)
		return;

	const bool hollow = (w > 2 * t + 2 && h > 2 * t + 2);

	for (int j = y; j < y + h; j++)
	{
		for (int i = x; i < x + w; i++)
		{
			int a = rrect_cov(i, j, x, y, w, h, r);
			if (a == 0)
				continue;
			if (hollow)
			{
				int ri = r - t;
				if (ri < 1) ri = 1;
				a -= rrect_cov(i, j, x + t, y + t, w - 2 * t,
					       h - 2 * t, ri);
			}
			plot(i, j, c, a);
		}
	}
}

void ui_circle(int cx, int cy, int r, uint16_t c)
{
	if (r <= 0)
		return;

	for (int j = cy - r; j <= cy + r; j++)
	{
		for (int i = cx - r; i <= cx + r; i++)
		{
			int dx = 2 * (i - cx) + 1, dy = 2 * (j - cy) + 1;
			int d2 = dx * dx + dy * dy;
			int r2 = 4 * r * r;
			int a;

			if (d2 >= r2)
				continue;
			if (d2 <= 4 * (r - 1) * (r - 1))
				a = 256;
			else
			{
				int inner = 4 * (r - 1) * (r - 1);
				int outer = 4 * (r + 1) * (r + 1);
				a = (outer - d2) * 256 /
				    ((outer - inner) ? (outer - inner) : 1);
				if (a > 256) a = 256;
			}
			plot(i, j, c, a);
		}
	}
}

void ui_arrow(int cx, int cy, int size, int dir, uint16_t c)
{
	if (size <= 0)
		return;

	/* A triangle whose base is `size` from the tip on the opposite side:
	 * the row (or column) through the middle is the full width, the ones at
	 * the edges are a single pixel wide, on the tip's side. */
	if (dir == -1 || dir == 1)
	{
		for (int dy = -size; dy <= size; dy++)
		{
			const int len = size - (dy < 0 ? -dy : dy);
			const int x = (dir == -1) ? cx + size - 2 * len
						  : cx - size;
			ui_fill(x, cy + dy, 2 * len + 1, 1, c);
		}
	}
	else
	{
		for (int dx = -size; dx <= size; dx++)
		{
			const int len = size - (dx < 0 ? -dx : dx);
			const int y = (dir == 2) ? cy + size - 2 * len
						 : cy - size;
			ui_fill(cx + dx, y, 1, 2 * len + 1, c);
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Text                                                                      */
/* ------------------------------------------------------------------------- */

static int glyph_index(char ch)
{
	if (ch < 32 || ch > 126)
		ch = '?';
	return ch - 32;
}

int ui_text_w(const ui_font *f, const char *s)
{
	int w = 0;

	if (s == NULL)
		return 0;

	for (; *s; s++)
		w += f->g[glyph_index(*s)].adv;
	return w;
}

/* How many characters of s fit in max_w, and how wide they are. When max_w > 0
 * and the text does not fit, room is left for the three dots ui_text() will
 * add. *ellipsis tells the caller whether it has to. */
static int fit_prefix(const ui_font *f, const char *s, int max_w, int *used,
		      bool *ellipsis)
{
	const int dots = 3 * f->g[glyph_index('.')].adv;
	int w = 0, n = 0;

	*ellipsis = false;

	if (max_w <= 0)
	{
		*used = (int)strlen(s);
		return ui_text_w(f, s);
	}

	for (; s[n]; n++)
	{
		const int adv = f->g[glyph_index(s[n])].adv;
		if (w + adv > max_w)
			break;
		w += adv;
	}

	if (s[n])
	{
		*ellipsis = true;
		while (n > 0 && w + dots > max_w)
		{
			n--;
			w -= f->g[glyph_index(s[n])].adv;
		}
	}

	*used = n;
	return w;
}

static void draw_glyph(const ui_font *f, const ui_glyph *g, int x, int y,
		       uint16_t c)
{
	const unsigned char *bits = f->bits + g->off;

	for (int j = 0; j < g->h; j++)
	{
		for (int i = 0; i < g->w; i++)
		{
			unsigned idx = (unsigned)j * g->w + i;
			unsigned nib = (idx & 1) ? (bits[idx >> 1] >> 4)
						 : (bits[idx >> 1] & 0x0F);
			if (nib)
				plot(x + g->x + i, y + g->y + j, c,
				     (int)nib * 17);
		}
	}
}

int ui_text(const ui_font *f, int x, int y, const char *s, uint16_t c,
	    int max_w)
{
	int used = 0, cx = x;
	bool ellipsis;

	if (s == NULL)
		return x;

	const int w = fit_prefix(f, s, max_w, &used, &ellipsis);

	/* Nothing fits at all: say nothing rather than poke the dots out of the
	 * panel they were clipped to. */
	if (max_w > 0 && ellipsis && used == 0 &&
	    w + 3 * f->g[glyph_index('.')].adv > max_w)
		ellipsis = false;

	for (int i = 0; i < used; i++)
	{
		const ui_glyph *g = &f->g[glyph_index(s[i])];
		draw_glyph(f, g, cx, y, c);
		cx += g->adv;
	}

	if (ellipsis)
	{
		for (int i = 0; i < 3; i++)
		{
			const ui_glyph *g = &f->g[glyph_index('.')];
			draw_glyph(f, g, cx, y, c);
			cx += g->adv;
		}
	}

	return cx;
}

void ui_text_center(const ui_font *f, int cx, int y, const char *s, uint16_t c)
{
	ui_text(f, cx - ui_text_w(f, s) / 2, y, s, c, 0);
}

void ui_text_right(const ui_font *f, int rx, int y, const char *s, uint16_t c)
{
	ui_text(f, rx - ui_text_w(f, s), y, s, c, 0);
}

int ui_text_vc(const ui_font *f, int x, int y, int h, const char *s, uint16_t c,
	       int max_w)
{
	int ty = y + (h - f->ascent) / 2;
	return ui_text(f, x, ty, s, c, max_w);
}

/* ------------------------------------------------------------------------- */
/* Present                                                                   */
/* ------------------------------------------------------------------------- */

/* The bottom-screen present, in bands.
 *
 * The whole-screen present is a transpose of 76800 pixels plus a 153 KiB cache
 * flush, and on the console that is about 16 ms - a whole frame, once a second,
 * for a status readout. Doing it in one go means the frame it lands in overruns
 * its vblank and the game drops a frame (see docs/PORT-NOTES.md §13).
 *
 * So the copy and the flush are split into bands of panel *columns*, which are
 * contiguous in the framebuffer: each band is one 19 KiB region and one
 * GSPGPU_FlushDataCache() call. ui_present_begin() / ui_present_band() /
 * ui_present_swap() let the caller spend one band per frame, in the dead time
 * before the vblank wait, where it is free. The panel shows the previous buffer
 * until the swap, so a banded update is never visible half-done.
 */
static u16 *present_fb;
static int present_x;
static int present_cols;

bool ui_present_begin(void)
{
	present_fb = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
	present_x = 0;
	present_cols = 0;
	return present_fb != NULL;
}

bool ui_present_band(int cols)
{
	if (present_fb == NULL)
		return true;

	if (cols < 1)
		cols = 1;

	int x0 = present_x;
	int x1 = x0 + cols;
	if (x1 > UI_W)
		x1 = UI_W;

	/* The bottom LCD is a 240x320 panel mounted sideways, so landscape pixel
	 * (x, y) lives at fb[x * UI_H + (UI_H - 1 - y)] - a transpose with the
	 * rows flipped. Doing it tile by tile keeps the source rows in cache:
	 * within a tile the destination writes for one source column are 16
	 * consecutive halfwords, and the next column is a full panel row away. */
	for (int tx = x0; tx < x1; tx += 16)
	{
		int tx_end = tx + 16;
		if (tx_end > x1)
			tx_end = x1;

		for (int x = tx; x < tx_end; x++)
		{
			u16 *dst = &present_fb[x * UI_H + (UI_H - 1)];

			for (int y = 0; y < UI_H; y++)
				*dst-- = ui_canvas[y * UI_W + x];
		}
	}

	/* gfxScreenSwapBuffers() presents but does not clean the data cache; the
	 * GPU would otherwise read stale pixels. Whole buffers are contiguous in
	 * the panel layout, so a band of columns is one flush of one region -
	 * flushing the whole 153 KiB once a second is what cost 16 ms. */
	GSPGPU_FlushDataCache(&present_fb[x0 * UI_H],
			      (u32)((x1 - x0) * UI_H * sizeof(u16)));

	present_x = x1;
	present_cols += x1 - x0;
	return present_x >= UI_W;
}

int ui_present_band_width(void)
{
	return present_cols;
}

void ui_present_swap(void)
{
	if (present_fb != NULL)
		gfxScreenSwapBuffers(GFX_BOTTOM, false);

	present_fb = NULL;
	present_x = present_cols = 0;
}

void ui_present(void)
{
	/* Whatever was in flight is abandoned: a full present overwrites every
	 * band anyway, and only the swap makes it visible. */
	if (!ui_present_begin())
		return;

	ui_present_band(UI_W);
	ui_present_swap();
}

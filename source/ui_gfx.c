/**
 * Software rasteriser for the bottom screen UI.
 *
 * See ui_gfx.h for why this is CPU-drawn. Shapes are drawn with integer
 * coverage so the edges are smooth without a per-pixel float square root:
 * rounded corners and circles use a small distance table built once.
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

static inline uint16_t blend(uint16_t dst, uint16_t src, int a)
{
	unsigned dr = (dst >> 11) & 0x1F, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
	unsigned sr = (src >> 11) & 0x1F, sg = (src >> 5) & 0x3F, sb = src & 0x1F;

	dr += (int)(sr - (int)dr) * a >> 8;
	dg += (int)(sg - (int)dg) * a >> 8;
	db += (int)(sb - (int)db) * a >> 8;
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
 * circles have radius r. Integer only, so no libm and no division in the loop. */
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
	for (; *s; s++)
		w += f->g[glyph_index(*s)].adv;
	return w;
}

/* Width of s clipped to max_w, including the ellipsis if it has to clip. */
static int clipped_w(const ui_font *f, const char *s, int max_w, int *used)
{
	int w = 0, n = 0;

	if (max_w <= 0)
	{
		*used = (int)strlen(s);
		return ui_text_w(f, s);
	}

	for (; s[n]; n++)
	{
		int adv = f->g[glyph_index(s[n])].adv;
		if (w + adv > max_w)
			break;
		w += adv;
	}
	*used = n;

	if (s[n])
	{
		/* make room for "..." */
		int dots = 3 * f->g[glyph_index('.')].adv;
		while (n > 0 && w + dots > max_w)
		{
			n--;
			w -= f->g[glyph_index(s[n])].adv;
		}
		*used = n;
		return ui_text_w(f, s) > 0 ? w : 0;
	}
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
	int used = 0;
	int w = clipped_w(f, s, max_w, &used);
	int cx = x;

	for (int i = 0; i < used; i++)
	{
		const ui_glyph *g = &f->g[glyph_index(s[i])];
		draw_glyph(f, g, cx, y, c);
		cx += g->adv;
	}

	if (max_w > 0 && s[used])
	{
		for (int i = 0; i < 3; i++)
		{
			const ui_glyph *g = &f->g[glyph_index('.')];
			draw_glyph(f, g, cx, y, c);
			cx += g->adv;
		}
	}

	return w ? cx : x;
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

void ui_present(void)
{
	u16 *fb = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
	if (fb == NULL)
		return;

	/*
	 * The bottom LCD is a 240x320 panel mounted sideways, so landscape pixel
	 * (x, y) lives at fb[x * UI_H + (UI_H - 1 - y)] - a transpose with the
	 * rows flipped. Doing it tile by tile keeps the source rows in cache:
	 * within a tile the destination writes for one source column are 16
	 * consecutive halfwords, and the next column is a full panel row away.
	 */
	for (int ty = 0; ty < UI_H; ty += 16)
	{
		for (int tx = 0; tx < UI_W; tx += 16)
		{
			for (int x = tx; x < tx + 16; x++)
			{
				const uint16_t *src = &ui_canvas[ty * UI_W + x];
				u16 *dst = &fb[x * UI_H + (UI_H - 1 - ty)];

				for (int y = 0; y < 16; y++)
				{
					*dst-- = src[y * UI_W];
				}
			}
		}
	}

	/* gfxScreenSwapBuffers() presents but does not clean the data cache; the
	 * GPU would otherwise read stale pixels. Only the bottom screen is
	 * flushed (153 KiB) rather than gfxFlushBuffers()'s 300 KiB. */
	GSPGPU_FlushDataCache(fb, UI_W * UI_H * sizeof(uint16_t));
	gfxScreenSwapBuffers(GFX_BOTTOM, false);
}

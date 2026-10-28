/**
 * Drawing for the bottom screen. See ui_screens.h.
 *
 * Layout is in landscape pixels on a fixed 320x240 canvas, so nothing here
 * scales or depends on the top screen. Screens are drawn only when something
 * changes; drawing a whole screen costs well under a millisecond on an Old 3DS
 * and the present is a single cached framebuffer write.
 *
 * MIT licensed.
 */
#include "ui_screens.h"

#include <stdio.h>
#include <string.h>

#define C_BG      UI_RGB(14, 17, 23)
#define C_HEADER  UI_RGB(23, 28, 39)
#define C_PANEL   UI_RGB(24, 30, 41)
#define C_PANEL2  UI_RGB(31, 38, 52)
#define C_LINE    UI_RGB(44, 53, 70)
#define C_TEXT    UI_RGB(236, 239, 244)
#define C_DIM     UI_RGB(139, 149, 167)
#define C_ACCENT  UI_RGB(78, 141, 255)
#define C_ACCENT2 UI_RGB(143, 107, 255)
#define C_SEL     UI_RGB(30, 45, 74) /* accent over panel, flattened */
#define C_GOOD    UI_RGB(94, 200, 140)
#define C_BAD     UI_RGB(235, 110, 110)

#define MARGIN   10
#define CONTENT_W (UI_W - 2 * MARGIN)


/* ------------------------------------------------------------------------- */
/* Shared chrome                                                             */
/* ------------------------------------------------------------------------- */

/* Screens take optional strings; draw nothing rather than crash on a NULL. */
static const char *or_empty(const char *s)
{
	return (s != NULL) ? s : "";
}

static void draw_header(const char *title, const char *right)
{
	if (title == NULL)
		title = "";

	ui_vgradient(0, 0, UI_W, UI_HEADER_H, C_HEADER, C_BG);
	ui_fill(0, UI_HEADER_H - 1, UI_W, 1, C_LINE);

	/* A short accent bar makes the header read as a header at a glance. */
	ui_rrect(MARGIN, 11, 3, 10, 1, C_ACCENT);

	ui_text_vc(&ui_font_title, MARGIN + 10, 0, UI_HEADER_H, title, C_TEXT,
		   CONTENT_W - 100);

	if (right != NULL && right[0] != '\0')
		ui_text_right(&ui_font_small, UI_W - MARGIN, 10, right, C_DIM);
}

static void draw_footer(const char *const *hints, int n)
{
	int y = 209;

	for (int i = 0; i < n; i++)
	{
		ui_text(&ui_font_small, MARGIN, y, hints[i], C_DIM, CONTENT_W);
		y += 13;
	}
}

/* Elide the middle of a path so both "sdmc:/" and the last folder stay
 * readable: sdmc:/.../Games/Pokemon. */
static void path_short(char *out, size_t out_len, const char *path)
{
	const size_t len = strlen(path);

	if (out_len < 12)
	{
		/* Too small to elide anything; snprintf still terminates it. */
		snprintf(out, out_len, "%s", path);
		return;
	}

	if (len + 1 <= out_len)
	{
		snprintf(out, out_len, "%s", path);
		return;
	}

	const char *tail = strrchr(path, '/');
	if (tail == NULL || tail[1] == '\0')
	{
		snprintf(out, out_len, "%s", path);
		return;
	}

	/* Keep the leading part and as much of the tail as fits. */
	const size_t room = out_len - 1;
	const size_t lead = 6;
	const size_t tail_len = strlen(tail);

	if (tail_len + lead + 4 >= room)
	{
		/* Even the tail is too long: keep its right-hand side. */
		snprintf(out, out_len, "...%s", tail_len > room - 4
						? tail + tail_len - (room - 3)
						: tail);
		return;
	}

	snprintf(out, out_len, "%.*s...%s", (int)lead, path, tail);
}

/* ------------------------------------------------------------------------- */
/* File browser                                                              */
/* ------------------------------------------------------------------------- */

void scr_browser(const char *right, const char *path, const char *const *names,
		 const bool *isdir, int n, int sel, int top, bool ok)
{
	ui_clear(C_BG);
	draw_header("GBabe", right);

	/* Path panel; a tap here goes up a folder. */
	ui_rrect(MARGIN, UI_PATH_Y, CONTENT_W, UI_PATH_H, 8, C_PANEL);

	char short_path[48];
	path_short(short_path, sizeof short_path, path);
	ui_text_vc(&ui_font_body, MARGIN + 8, UI_PATH_Y, UI_PATH_H, short_path,
		   C_TEXT, CONTENT_W - 74);

	if (n > 0)
	{
		char count[24];
		snprintf(count, sizeof count, "%d/%d", sel + 1, n);
		ui_text_right(&ui_font_small, UI_W - MARGIN - 8,
			      UI_PATH_Y + 7, count, C_DIM);
	}

	if (n == 0)
	{
		ui_text(&ui_font_body, MARGIN + 4, UI_LIST_Y + 6,
			ok ? "No ROMs here." : "Cannot open this folder.", C_DIM,
			CONTENT_W);
		ui_text(&ui_font_small, MARGIN + 4, UI_LIST_Y + 26,
			ok ? "Put .gb / .gbc files in sdmc:/, or tap the path."
			   : "The card was removed, or the folder is gone.",
			C_DIM, CONTENT_W);
	}
	else
	{
		for (int i = 0; i < UI_LIST_ROWS && top + i < n; i++)
		{
			const int idx = top + i;
			const int y = UI_LIST_Y + i * UI_LIST_PITCH;
			const bool is_sel = (idx == sel);

			if (is_sel)
			{
				ui_rrect(MARGIN, y, CONTENT_W, UI_LIST_H, 7,
					 C_SEL);
				ui_rrect(MARGIN, y + 3, 3, UI_LIST_H - 6, 1,
					 C_ACCENT);
			}

			const uint16_t col = is_sel ? C_TEXT : C_DIM;
			const int name_w = CONTENT_W - 46;

			ui_text_vc(&ui_font_body, MARGIN + 10, y, UI_LIST_H,
				   or_empty(names != NULL ? names[idx] : NULL), col,
				   name_w);

			if (isdir != NULL && isdir[idx])
			{
				ui_arrow(UI_W - MARGIN - 12,
					 y + UI_LIST_H / 2, 3, 1,
					 is_sel ? C_ACCENT : C_DIM);
			}
			else
			{
				/* Extension tag, right aligned, sized to fit. */
				const char *dot = strrchr(
					or_empty(names != NULL ? names[idx] : NULL),
					'.');
				char tag[6];
				snprintf(tag, sizeof tag, "%s", dot ? dot + 1 : "");

				for (char *p = tag; *p; p++)
					*p = (char)((*p >= 'a' && *p <= 'z')
						    ? *p - 'a' + 'A'
						    : *p);

				if (tag[0] != '\0')
				{
					const int tw = ui_text_w(&ui_font_small,
								 tag);
					const int pill_w = tw + 10;
					const int pill_x = UI_W - MARGIN - 8 -
							   pill_w;

					ui_rrect(pill_x, y + 4, pill_w, 11, 4,
						 is_sel ? C_ACCENT : C_PANEL2);
					ui_text(&ui_font_small, pill_x + 5,
						y + 3, tag,
						is_sel ? C_BG : C_DIM, 0);
				}
			}
		}
	}

	const char *hints[] = {
		"D-Pad move   L/R page   A open   B up a folder",
		"START quits   tap the path to go up",
	};
	draw_footer(hints, 2);
}

int scr_browser_hit(int y, int top, int n)
{
	if (y < UI_LIST_Y)
		return -1;

	const int rel = y - UI_LIST_Y;
	const int row = rel / UI_LIST_PITCH;

	/* Rows are 19px tall with a 2px gap: a tap in the gap is not a tap on
	 * the row below it. */
	if (row < 0 || row >= UI_LIST_ROWS || rel % UI_LIST_PITCH >= UI_LIST_H)
		return -1;
	if (top + row >= n)
		return -1;
	return top + row;
}

/* ------------------------------------------------------------------------- */
/* Pause menu                                                                */
/* ------------------------------------------------------------------------- */

#define TAB_Y   34
#define TAB_H   20
#define ROW_Y   60
#define ROW_PITCH 20
#define ROW_H   18

/* Only this many tabs / rows fit on the screen; a larger count would walk off
 * the caller's array and draw over the footer. */
#define UI_MAX_TABS 4
#define UI_MAX_ROWS 7

static int clamp(int v, int hi)
{
	if (v < 0) return 0;
	if (v > hi) return hi;
	return v;
}


int scr_menu_hit_tab(int x, int y, const char *const *tabs, int n_tabs)
{
	if (tabs == NULL || y < TAB_Y || y >= TAB_Y + TAB_H)
		return -1;

	int tx = MARGIN;
	for (int i = 0; i < clamp(n_tabs, UI_MAX_TABS); i++)
	{
		const int w = ui_text_w(&ui_font_small, or_empty(tabs[i])) + 20;
		if (x >= tx && x < tx + w)
			return i;
		tx += w + 6;
	}
	return -1;
}

int scr_menu_hit_row(int y, int n_rows)
{
	if (y < ROW_Y)
		return -1;

	const int rel = y - ROW_Y;
	const int row = rel / ROW_PITCH;

	/* Same as the browser: the 3px gap between rows is not a row. */
	if (row >= clamp(n_rows, UI_MAX_ROWS) || rel % ROW_PITCH >= ROW_H)
		return -1;
	return row;
}

static void draw_tabs(const ui_menu *m)
{
	int x = MARGIN;

	for (int i = 0; i < clamp(m->n_tabs, UI_MAX_TABS); i++)
	{
		const int w = ui_text_w(&ui_font_small, or_empty(m->tabs[i])) + 20;
		const bool on = (i == m->tab);

		ui_rrect(x, TAB_Y, w, TAB_H, 10, on ? C_ACCENT : C_PANEL);
		ui_text(&ui_font_small, x + 10, TAB_Y + 5,
			or_empty(m->tabs[i]), on ? C_BG : C_DIM, 0);
		x += w + 6;
	}
}

static void draw_toggle(int right, int y, bool on)
{
	const int w = 30, h = 14;
	const int x = right - w;

	ui_rrect(x, y, w, h, 7, on ? C_ACCENT : C_PANEL2);
	if (!on)
		ui_rrect_outline(x, y, w, h, 7, 1, C_LINE);

	ui_circle(x + (on ? w - h / 2 - 1 : h / 2 + 1), y + h / 2, 5,
		  on ? C_TEXT : C_DIM);
}

void scr_menu(const ui_menu *m)
{
	const int n_rows = clamp(m->n_rows, UI_MAX_ROWS);

	ui_clear(C_BG);
	draw_header(m->title, m->status);
	draw_tabs(m);

	for (int i = 0; i < n_rows; i++)
	{
		const ui_row *r = &m->rows[i];
		const int y = ROW_Y + i * ROW_PITCH;
		const bool is_sel = (i == m->sel);

		if (is_sel)
		{
			ui_rrect(MARGIN, y, CONTENT_W, ROW_H, 7, C_SEL);
			ui_rrect(MARGIN, y + 3, 3, ROW_H - 6, 1, C_ACCENT);
		}

		ui_text_vc(&ui_font_body, MARGIN + 10, y, ROW_H,
			   or_empty(r->label), is_sel ? C_TEXT : C_DIM, 170);

		if (r->kind == UI_ROW_TOGGLE)
		{
			draw_toggle(UI_W - MARGIN - 8, y + (ROW_H - 14) / 2,
				    r->value != NULL && !strcmp(r->value, "on"));
		}
		else if (r->kind == UI_ROW_CHOICE)
		{
			const char *val = or_empty(r->value);
			const int vw = ui_text_w(&ui_font_body, val);
			/* The right-hand arrow keeps its slot whether or not the row
			 * is selected, so the value never jumps about. */
			const int rx = UI_W - MARGIN - 12;

			ui_text_right(&ui_font_body, rx, y + 4, val,
				      is_sel ? C_ACCENT : C_DIM);

			if (is_sel)
			{
				ui_arrow(rx - vw - 9, y + ROW_H / 2, 3, -1,
					 C_ACCENT);
				ui_arrow(rx + 8, y + ROW_H / 2, 3, 1, C_ACCENT);
			}
		}
	}

	/* The note goes right under the rows, and only if it fits above the
	 * footer: with a full tab there is no room, and one line of help is not
	 * worth pushing the key hints off the screen. */
	const int note_y = ROW_Y + n_rows * ROW_PITCH + 3;

	if (m->note != NULL && m->note[0] != '\0' && note_y <= 201)
		ui_text(&ui_font_small, MARGIN, note_y, m->note, C_DIM,
			CONTENT_W);

	const char *hints[] = {
		"D-Pad move   Left/Right change   A select",
		"B resume   L/R or a tap switches tab",
	};
	draw_footer(hints, 2);
}

/* ------------------------------------------------------------------------- */
/* Status readout                                                            */
/* ------------------------------------------------------------------------- */

/* A 0..1 fraction as a rounded bar, with the core part in the accent colour
 * and the blit part stacked on top of it. */
static void draw_bar(int x, int y, int w, int h, float core, float blit)
{
	ui_rrect(x, y, w, h, h / 2, C_PANEL2);

	float total = core + blit;
	if (total < 0.0f)
		total = 0.0f;
	if (total > 1.0f)
		total = 1.0f;

	int cw = (int)((core > 0.0f ? core : 0.0f) * (float)w);
	int bw = (int)((blit > 0.0f ? blit : 0.0f) * (float)w);

	if (cw > w) cw = w;
	if (cw + bw > w) bw = w - cw;
	if (cw >= h)
		ui_rrect(x, y, cw, h, h / 2, C_ACCENT);
	if (bw >= h)
		ui_rrect(x + cw, y, bw, h, h / 2, C_ACCENT2);
}

void scr_hud(const ui_hud *h)
{
	ui_clear(C_BG);

	/* Header: what is running, and how it is running. */
	ui_vgradient(0, 0, UI_W, 44, C_HEADER, C_BG);
	ui_fill(0, 43, UI_W, 1, C_LINE);
	ui_rrect(MARGIN, 9, 3, 26, 1, C_ACCENT);
	ui_text(&ui_font_title, MARGIN + 10, 5, or_empty(h->title), C_TEXT,
		CONTENT_W - 60);
	if (h->sub != NULL && h->sub[0] != '\0')
		ui_text(&ui_font_small, MARGIN + 10, 25, h->sub, C_DIM,
			CONTENT_W - 60);

	/* The headline: presented frames per second. */
	ui_rrect(MARGIN, 50, CONTENT_W, 74, 12, C_PANEL);

	char buf[32];
	snprintf(buf, sizeof buf, "%.1f", h->fps);
	ui_text(&ui_font_big, MARGIN + 12, 57, buf, C_TEXT, 0);
	ui_text(&ui_font_body, MARGIN + 12 + ui_text_w(&ui_font_big, buf) + 4,
		77, "fps", C_DIM, 0);

	snprintf(buf, sizeof buf, "%.1f ms/frame", h->frame_ms);
	ui_text_right(&ui_font_body, UI_W - MARGIN - 12, 55, buf,
		      h->fps >= 55.0f ? C_GOOD : C_BAD);

	snprintf(buf, sizeof buf, "worst %.1f ms", h->worst_ms);
	ui_text_right(&ui_font_small, UI_W - MARGIN - 12, 71, buf, C_DIM);

	/* Say what is limiting it, or that nothing is: an fps number on its own
	 * tells the player nothing they can act on. */
	const bool full = (h->fps >= 55.0f);
	if (h->limit != NULL)
		ui_text_right(&ui_font_small, UI_W - MARGIN - 12, 83, h->limit,
			      full ? C_DIM : C_BAD);

	/* Frame budget: how much of a 16.7 ms frame each part takes. Anything
	 * the three known parts do not account for shows up in the tail, which
	 * is how a service call or a syscall in the loop becomes visible. */
	const float core = h->core_ms / 16.74f;
	const float blit = h->blit_ms / 16.74f;
	const float other = h->other_ms / 16.74f;

	draw_bar(MARGIN + 12, 98, CONTENT_W - 24, 8, core, blit);

	if (other > 0.0f)
	{
		const int w = (int)(other * (float)(CONTENT_W - 24));
		if (w > 3)
			ui_fill(MARGIN + 12 + CONTENT_W - 24 - w, 98,
				w < CONTENT_W - 24 ? w : CONTENT_W - 24, 8,
				C_BAD);
	}

	/* Legend for the bar, in the same order the segments are stacked. */
	int lx = MARGIN + 12;
	ui_rrect(lx, 110, 7, 7, 2, C_ACCENT);
	lx += 11;
	ui_text(&ui_font_small, lx, 107, "core", C_DIM, 0);
	lx += ui_text_w(&ui_font_small, "core") + 12;
	ui_rrect(lx, 110, 7, 7, 2, C_ACCENT2);
	lx += 11;
	ui_text(&ui_font_small, lx, 107, "blit", C_DIM, 0);
	ui_text_right(&ui_font_small, UI_W - MARGIN - 12, 107,
		      h->other_ms > 0.5f ? "other / wait" : "wait", C_DIM);

	/* Two columns of label/value pairs. */
	for (int i = 0; i < h->n_stats && i < UI_HUD_STATS; i++)
	{
		if (h->stat_label[i] == NULL && h->stat_value[i] == NULL)
			continue;

		const int col = i & 1;
		const int row = i >> 1;
		const int x = MARGIN + col * (CONTENT_W / 2 + 4);
		const int y = 134 + row * 19;

		ui_text(&ui_font_small, x, y + 2, or_empty(h->stat_label[i]),
			C_DIM, 0);
		ui_text_right(&ui_font_small, x + CONTENT_W / 2 - 8, y + 2,
			      or_empty(h->stat_value[i]), C_TEXT);
	}

	/* Touch target for the menu. */
	ui_rrect(MARGIN, 200, CONTENT_W, 24, 12, C_PANEL);
	ui_text_center(&ui_font_small, UI_W / 2, 207,
		       "Touch here, or L + R, for the menu", C_DIM);
}

/* ------------------------------------------------------------------------- */
/* Messages                                                                  */
/* ------------------------------------------------------------------------- */

void scr_message(const char *title, const char *l1, const char *l2,
		 const char *hint)
{
	ui_clear(C_BG);
	draw_header(title, "");

	ui_rrect(MARGIN, 52, CONTENT_W, 80, 12, C_PANEL);
	ui_rrect(MARGIN, 52, 4, 80, 2, C_BAD);

	int y = 66;
	if (l1 != NULL)
	{
		ui_text(&ui_font_body, MARGIN + 14, y, l1, C_TEXT,
			CONTENT_W - 28);
		y += 20;
	}
	if (l2 != NULL)
		ui_text(&ui_font_small, MARGIN + 14, y, l2, C_DIM,
			CONTENT_W - 28);

	if (hint != NULL)
		ui_text_center(&ui_font_small, UI_W / 2, 210, hint, C_DIM);
}

void scr_blank(void)
{
	ui_clear(C_BG);
}

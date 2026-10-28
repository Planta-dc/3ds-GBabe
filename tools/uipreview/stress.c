/**
 * Hostile-input test for the UI drawing code.
 *
 *   make -C tools/uipreview asan
 *
 * Draws every screen with strings and counts at and past their limits - over-
 * long names, empty names, NULL optionals, row/tab counts larger than the
 * layout can hold, radii and rectangles that are negative or off-screen - under
 * AddressSanitizer and UndefinedBehaviorSanitizer, then checks that ui_present()
 * puts the canvas on the bottom screen pixel for pixel.
 *
 * The point is not to be pretty: it is that nothing in ui_gfx.c or
 * ui_screens.c may read or write out of bounds whatever a caller passes, since
 * on the 3DS that is a crash on the user's screen with no debugger attached.
 *
 * NOT part of the 3DS build.
 */
#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_gfx.h"
#include "ui_screens.h"

static void long_strings(char *name, size_t name_len, char *path,
			 size_t path_len)
{
	memset(name, 'A', name_len - 1);
	name[name_len - 1] = '\0';

	strcpy(path, "sdmc:/");
	for (int i = 0; i < 40 && strlen(path) + 32 < path_len - 1; i++)
		strcat(path, "averyverylongfoldername/");
	strcat(path, "Game.gbc");
}

/* `n`, `n_rows` and `n_tabs` promise how many entries the caller's arrays hold;
 * they are the contract the screens rely on. What the screens must survive is
 * any count the layout cannot fit, so the arrays here are deliberately bigger
 * than the screen (the browser shows 7 rows, the menu 7) and the counts are
 * bigger still. */
#define BIG 128

static void all_screens(const char *long_name, const char *long_path)
{
	const char *names[BIG];
	bool isdir[BIG];

	for (int i = 0; i < BIG; i++)
	{
		names[i] = (i & 1) ? "b.gb" : long_name;
		isdir[i] = (i % 5) == 0;
	}

	scr_browser(long_name, long_path, names, isdir, 4, 1, 0, true);
	scr_browser("", "", names, isdir, 4, 0, 0, false);
	scr_browser("v0.1.0", long_path, NULL, NULL, 0, 0, 0, false);
	/* far more entries than fit, scrolled to the end and past it */
	scr_browser("v0.1.0", long_path, names, isdir, BIG, 3, 0, true);
	scr_browser("v0.1.0", long_path, names, isdir, BIG, BIG - 1, BIG - 7, true);
	scr_browser("v0.1.0", long_path, names, isdir, BIG, BIG - 1, BIG + 50, true);
	ui_present();

	scr_message(long_name, long_name, long_name, long_name);
	scr_message("", "", "", "");
	scr_message("t", NULL, NULL, NULL);
	ui_present();

	ui_hud h;
	memset(&h, 0, sizeof h);
	h.title = long_name;
	h.sub = long_name;
	h.fps = 0.0f;
	h.frame_ms = 999.0f;
	h.core_ms = 500.0f;
	h.blit_ms = -5.0f;
	h.wait_ms = 1e9f;
	for (int i = 0; i < UI_HUD_STATS; i++)
	{
		h.stat_label[i] = long_name;
		h.stat_value[i] = long_name;
	}
	h.n_stats = 99; /* more than the array holds */
	scr_hud(&h);
	ui_present();

	ui_hud z;
	memset(&z, 0, sizeof z);
	z.n_stats = -3;
	scr_hud(&z);
	z.n_stats = 6;
	scr_hud(&z);
	ui_present();

	ui_row rows[BIG] = {
		{ long_name, long_name, UI_ROW_ACTION },
		{ long_name, long_name, UI_ROW_CHOICE },
		{ long_name, long_name, UI_ROW_TOGGLE },
		{ long_name, NULL, UI_ROW_ACTION },
		{ NULL, NULL, UI_ROW_CHOICE },
		{ long_name, long_name, UI_ROW_TOGGLE },
		{ long_name, long_name, UI_ROW_ACTION },
		{ long_name, long_name, UI_ROW_CHOICE },
	};
	const char *tabs[BIG] = { long_name, "", long_name, long_name,
					 long_name, long_name, long_name, NULL };
	const char *const *tabp = tabs;

	ui_menu m = { long_name, long_name, tabp, 3, 2, rows, 3, 2, long_name };
	scr_menu(&m);

	m.n_tabs = 0;
	m.n_rows = 0;
	m.note = NULL;
	scr_menu(&m);

	m.n_rows = 99; /* past the array and past the layout */
	m.sel = 200;
	m.n_tabs = 99;
	m.tab = 99;
	scr_menu(&m);
	ui_present();

	ui_menu empty;
	memset(&empty, 0, sizeof empty);
	empty.tabs = tabp;
	empty.rows = rows;
	scr_menu(&empty);
	ui_present();

	/* hit-testing must not report anything for impossible input */
	for (int y = -20; y < 300; y += 3)
	{
		if (scr_browser_hit(y, 0, 4) > 3)
		{
			printf("scr_browser_hit(%d) out of range\n", y);
			exit(1);
		}
		if (scr_menu_hit_row(y, 5) > 4)
		{
			printf("scr_menu_hit_row(%d) out of range\n", y);
			exit(1);
		}
		(void)scr_menu_hit_tab(y, y, tabp, 3);
		(void)scr_menu_hit_tab(y, y, NULL, 0);
	}
}

static void shapes_and_text(void)
{
	for (int w = -3; w < 40; w += 7)
	{
		for (int r = -2; r < 20; r += 3)
		{
			ui_rrect(-5, -5, w, w + 5, r, 0xFFFF);
			ui_rrect_outline(-5, -5, w, w + 5, r, r, 0x1234);
			ui_rrect_outline(300, 230, w, w, r, 100, 0x1234);
			ui_circle(-20, 400, r, 0xABCD);
			ui_circle(160, 120, r, 0xABCD);
			ui_arrow(300, 230, r, r % 3 - 1, 0x0F0F);
			ui_arrow(-10, -10, r, 2, 0x0F0F);
		}
	}
	ui_vgradient(0, -10, 400, 1, 0, 0xFFFF);
	ui_vgradient(0, -10, 400, 0, 0, 0xFFFF);
	ui_fill_alpha(-50, -50, 500, 500, 0xFFFF, 300);
	ui_fill_alpha(0, 0, 320, 240, 0, -5);

	const ui_font *fonts[] = { &ui_font_small, &ui_font_body, &ui_font_bold,
				   &ui_font_title, &ui_font_big };

	for (int f = 0; f < 5; f++)
	{
		for (int mw = -2; mw < 40; mw += 3)
		{
			ui_text(fonts[f], 0, 0, "Wg 12345", 0xFFFF, mw);
			ui_text(fonts[f], -40, -40, "offscreen", 0xFFFF, mw);
			/* non-printable and out-of-range bytes */
			ui_text(fonts[f], 300, 230, "Wg\x7f\x1f\xe9\x01", 0xFFFF, mw);
			ui_text_vc(fonts[f], 0, 0, 0, "zero height", 0xFFFF, mw);
			ui_text_vc(fonts[f], 0, 0, -5, "negative", 0xFFFF, mw);
			ui_text(fonts[f], 0, 0, NULL, 0xFFFF, mw);
		}
		ui_text_center(fonts[f], 160, 100, "centred", 0xFFFF);
		ui_text_right(fonts[f], 0, 0, "right aligned", 0xFFFF);
		ui_text_w(fonts[f], "");
		ui_text_w(fonts[f], NULL);
	}
}

/* ui_present() must copy the canvas to the bottom screen unrotated: after the
 * stub undoes the panel rotation, the two have to match pixel for pixel. Any
 * index error in the transpose shows up here for all 76 800 pixels. */
static int check_present(void)
{
	for (int i = 0; i < UI_W * UI_H; i++)
		ui_canvas[i] = (uint16_t)(i * 2654435761u >> 13);

	ui_present();

	int bad = 0;
	for (int y = 0; y < 240; y++)
	{
		for (int x = 0; x < 320; x++)
		{
			if (g_host_bottom_fb[y * 320 + x] != ui_canvas[y * 320 + x])
			{
				if (bad++ < 5)
					printf("present mismatch at %d,%d\n", x, y);
			}
		}
	}

	printf("present: %s (%d bad pixels)\n", bad ? "FAIL" : "ok", bad);
	return bad == 0;
}

int main(void)
{
	char long_name[600];
	char long_path[4096];

	gfxInitDefault();
	long_strings(long_name, sizeof long_name, long_path, sizeof long_path);

	for (int pass = 0; pass < 3; pass++)
	{
		all_screens(long_name, long_path);
		shapes_and_text();
		ui_present();
	}

	if (!check_present())
		return 1;

	puts("ui stress: ok");
	return 0;
}

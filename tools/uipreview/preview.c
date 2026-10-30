/**
 * Renders every bottom-screen layout to a PPM file on the host.
 *
 *   make -C tools/uipreview
 *   ./tools/uipreview/uipreview out/     (writes browser.ppm, menu.ppm, ...)
 *
 * The real ui_gfx.c / ui_screens.c are used, with tools/host/stub.c standing in
 * for libctru, so the images are pixel-for-pixel what the 3DS shows - the
 * rotation through the framebuffer included.
 *
 * NOT part of the 3DS build.
 */
#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb3ds.h"
#include "hud.h"
#include "hud_test.h"
#include "ui_gfx.h"
#include "ui_screens.h"

static void rgb565_bytes(u16 p, unsigned char rgb[3])
{
	rgb[0] = (unsigned char)(((p >> 11) & 0x1F) * 255 / 31);
	rgb[1] = (unsigned char)(((p >> 5) & 0x3F) * 255 / 63);
	rgb[2] = (unsigned char)((p & 0x1F) * 255 / 31);
}

static void write_ppm_fb(const char *path, const uint16_t *fb, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (f == NULL)
	{
		fprintf(stderr, "cannot write %s\n", path);
		return;
	}

	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int i = 0; i < w * h; i++)
	{
		unsigned char rgb[3];

		rgb565_bytes(fb[i], rgb);
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

static void write_ppm(const char *path)
{
	write_ppm_fb(path, g_host_bottom_fb, 320, 240);
}

/* Draw the canvas, push it through the framebuffer path, dump the result. */
static void emit(const char *dir, const char *name)
{
	char path[512];

	ui_present();
	snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
	write_ppm(path);
	printf("%s\n", path);
}

static void show_browser(const char *dir)
{
	static const char *names[] = {
		"Games", "Pokemon Crystal.gbc", "Tetris.gb",
		"Super Mario Land.gb", "Zelda - Link's Awakening DX.gbc",
		"Metroid II.gb", "Wario Land 3.gbc",
	};
	static bool isdir[] = { true, false, false, false, false, false, false };

	scr_browser("v" GBABE_VERSION, "sdmc:/3ds/gbabe/Games", names, isdir,
		    7, 1, 0, true);
	emit(dir, "browser");

	/* Empty folder. */
	scr_browser("v" GBABE_VERSION, "sdmc:/", NULL, NULL, 0, 0, 0, false);
	emit(dir, "browser-empty");
}

static void show_menu(const char *dir)
{
	static const char *tabs[] = { "Game", "Settings" };
	static const ui_row game[] = {
		{ "Resume", NULL, UI_ROW_ACTION },
		{ "Reset game", NULL, UI_ROW_ACTION },
		{ "Load another ROM", NULL, UI_ROW_ACTION },
		{ "Save and quit", NULL, UI_ROW_ACTION },
	};
	static const ui_row settings[] = {
		{ "Palette", "green", UI_ROW_CHOICE },
		{ "Video", "fit height", UI_ROW_CHOICE },
		{ "Frameskip", "off", UI_ROW_CHOICE },
		{ "Sound", "on", UI_ROW_TOGGLE },
		{ "Status on screen", "off", UI_ROW_TOGGLE },
		{ "Debug log", "on", UI_ROW_TOGGLE },
	};

	ui_menu m = {
		.title = "GBabe " GBABE_VERSION,
		.status = "paused",
		.tabs = tabs,
		.n_tabs = 2,
		.tab = 1,
		.rows = settings,
		.n_rows = 6,
		.sel = 1,
		.note = NULL, /* the Settings tab is full: no room for a note */
	};
	scr_menu(&m);
	emit(dir, "menu-settings");

	m.tab = 0;
	m.rows = game;
	m.n_rows = 4;
	m.sel = 1;
	m.note = "Saved games go to sdmc:/3ds/gbabe/saves.";
	scr_menu(&m);
	emit(dir, "menu-game");
}

/* The status strip replaced the status readout (hud.h): it is drawn onto the
 * game framebuffer itself, in place, so the preview renders it the way the
 * console sees it - a 160x144 game frame with the strip on top - rather than
 * as a bottom-screen layout. The three scenarios are the ones the readout
 * had: full speed, a console that cannot keep up, and a loop that is the
 * problem. */
static void show_strip(const char *dir)
{
	static uint16_t fb[144 * 160];

	g_cfg.show_hud = true;

	const struct
	{
		float fps, frame_ms, worst_ms, core_ms, blit_ms;
		const char *name;
	} scenarios[] = {
		{ 59.7f, 16.8f, 17.4f, 11.4f, 1.9f, "strip" },
		{ 43.1f, 23.2f, 31.8f, 19.6f, 1.7f, "strip-slow" },
		{ 51.0f, 19.6f, 28.2f, 9.1f, 1.2f, "strip-overhead" },
	};

	for (size_t i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++)
	{
		/* A game frame: the strip only replaces its top 16 rows. */
		for (int y = 0; y < 144; y++)
		{
			for (int x = 0; x < 160; x++)
				fb[y * 160 + x] = (uint16_t)(0x2143 + (y & 1));
		}

		g_perf.fps = scenarios[i].fps;
		g_perf.frame_ms = scenarios[i].frame_ms;
		g_perf.worst_ms = scenarios[i].worst_ms;
		g_perf.core_ms = scenarios[i].core_ms;
		g_perf.blit_ms = scenarios[i].blit_ms;

		hud_draw(fb, 160);

		char path[512];

		snprintf(path, sizeof path, "%s/%s.ppm", dir, scenarios[i].name);
		write_ppm_fb(path, fb, 160, 144);
		printf("%s\n", path);
	}
}

static void show_message(const char *dir)
{
	scr_message("Cannot open ROM",
		    "sdmc:/roms/crystal.gbc",
		    "The file is missing or the SD card was removed.",
		    "Press A or B");
	emit(dir, "message");
}

int main(int argc, char **argv)
{
	const char *dir = (argc > 1) ? argv[1] : ".";

	gfxInitDefault();
	show_browser(dir);
	show_menu(dir);
	show_strip(dir);
	show_message(dir);
	return 0;
}

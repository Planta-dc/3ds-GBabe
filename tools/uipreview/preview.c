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

#include "ui_gfx.h"
#include "ui_screens.h"

static void write_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (f == NULL)
	{
		fprintf(stderr, "cannot write %s\n", path);
		return;
	}

	fprintf(f, "P6\n320 240\n255\n");
	for (int i = 0; i < 320 * 240; i++)
	{
		const u16 p = g_host_bottom_fb[i];
		const unsigned char rgb[3] = {
			(unsigned char)(((p >> 11) & 0x1F) * 255 / 31),
			(unsigned char)(((p >> 5) & 0x3F) * 255 / 63),
			(unsigned char)((p & 0x1F) * 255 / 31),
		};
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
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

	scr_browser("v0.1.0", "sdmc:/3ds/gbabe/Games", names, isdir, 7, 1, 0,
		    true);
	emit(dir, "browser");

	/* Empty folder. */
	scr_browser("v0.1.0", "sdmc:/", NULL, NULL, 0, 0, 0, false);
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
	};

	ui_menu m = {
		.title = "GBabe 0.1.0",
		.status = "paused",
		.tabs = tabs,
		.n_tabs = 2,
		.tab = 1,
		.rows = settings,
		.n_rows = 5,
		.sel = 1,
		.note = "D-Pad / Circle Pad, then A to change.",
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

/* The stat table is built from the same numbers as the headline, so the
 * preview cannot show a HUD that disagrees with itself. */
static void fill_stats(ui_hud *h, const char *sound, const char *palette)
{
	static char core[16], blit[16], wait[16], frame[16];
	static const char *labels[] = { "core", "blit", "wait", "frame",
					"sound", "palette" };

	snprintf(core, sizeof core, "%.1f ms", h->core_ms);
	snprintf(blit, sizeof blit, "%.1f ms", h->blit_ms);
	snprintf(wait, sizeof wait, "%.1f ms", h->wait_ms);
	snprintf(frame, sizeof frame, "%.1f ms", h->frame_ms);

	h->stat_label[0] = labels[0];
	h->stat_value[0] = core;
	h->stat_label[1] = labels[1];
	h->stat_value[1] = blit;
	h->stat_label[2] = labels[2];
	h->stat_value[2] = wait;
	h->stat_label[3] = labels[3];
	h->stat_value[3] = frame;
	h->stat_label[4] = labels[4];
	h->stat_value[4] = sound;
	h->stat_label[5] = labels[5];
	h->stat_value[5] = palette;
	h->n_stats = 6;
}

static void show_hud(const char *dir)
{
	ui_hud h = {
		.title = "Pokemon Crystal",
		.sub = "CGB   2 Mbit   MBC5",
		.fps = 59.7f,
		.frame_ms = 16.8f,
		.worst_ms = 17.4f,
		.core_ms = 11.4f,
		.blit_ms = 1.9f,
		.wait_ms = 3.1f,
		.other_ms = 0.4f,
		.limit = "full speed",
	};
	fill_stats(&h, "42 ms", "green");
	scr_hud(&h);
	emit(dir, "hud");

	/* The case that matters on an Old 3DS: the interpreter cannot keep up. */
	h.fps = 43.1f;
	h.frame_ms = 23.2f;
	h.worst_ms = 31.8f;
	h.core_ms = 19.6f;
	h.blit_ms = 1.7f;
	h.wait_ms = 0.4f;
	h.other_ms = 1.5f;
	h.limit = "CPU-bound (interpreter)";
	fill_stats(&h, "18 ms", "green");
	scr_hud(&h);
	emit(dir, "hud-slow");

	/* And the case where the loop itself is the problem, which the old
	 * readout hid: core and blit are fine but the frames are still late. */
	h.fps = 51.0f;
	h.frame_ms = 19.6f;
	h.worst_ms = 28.2f;
	h.core_ms = 9.1f;
	h.blit_ms = 1.2f;
	h.wait_ms = 0.2f;
	h.other_ms = 9.1f;
	h.limit = "overhead (see other)";
	fill_stats(&h, "off", "auto");
	scr_hud(&h);
	emit(dir, "hud-overhead");
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
	show_hud(dir);
	show_message(dir);
	return 0;
}

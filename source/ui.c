/**
 * Bottom screen UI: the ROM browser, the pause menu and the status readout.
 *
 * Everything is drawn by our own rasteriser (ui_gfx.c) into a 320x240 canvas
 * and pushed to the bottom screen with one ui_present(). That replaced a
 * libctru text console, which was the reason the browser felt unusable on an
 * Old 3DS: the console flushes the 300 KiB framebuffer on every newline, and
 * every write re-parses escape sequences and redraws glyph by glyph. Drawing
 * the screen ourselves costs a fraction of a millisecond and only happens when
 * something actually changes.
 *
 * The directory is likewise only rescanned when it changes, and a name that
 * already has a ROM extension is never stat()'d (each stat() is a round trip to
 * the SD filesystem).
 *
 * MIT licensed.
 */
#include "ui.h"

#include <3ds.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "audio.h"
#include "emu.h"
#include "palette.h"
#include "ui_gfx.h"
#include "ui_screens.h"
#include "video.h"

#define MAX_ENTRIES 400

typedef struct
{
	char name[256];
	bool is_dir;
} dir_entry;

static dir_entry entries[MAX_ENTRIES];
static int       entry_count;
static char      cur_dir[512];
static bool      last_scan_ok;

/* HUD statistics. */
static unsigned hud_frames;
static u64      hud_last_tick;
static float    hud_fps;
static u64      hud_sum_core, hud_sum_blit, hud_sum_wait, hud_sum_frame;

/* ------------------------------------------------------------------------- */
/* Input                                                                     */
/* ------------------------------------------------------------------------- */

/* Blocking wait for the next input event. Returns the keys just pressed, with
 * KEY_TOUCH set (and `touch` filled in) for a tap; 0 means HOME was pressed and
 * the application should quit. */
static u32 wait_input(touchPosition *touch)
{
	while (aptMainLoop())
	{
		hidScanInput();

		u32 down = hidKeysDownRepeat();
		if (down != 0)
		{
			if ((down & KEY_TOUCH) && touch != NULL)
				hidTouchRead(touch);
			return down;
		}

		gspWaitForVBlank();
	}
	return 0;
}

static bool has_rom_extension(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (dot == NULL)
		return false;
	return !strcasecmp(dot, ".gb") || !strcasecmp(dot, ".gbc") ||
	       !strcasecmp(dot, ".sgb") || !strcasecmp(dot, ".dmg");
}

/* ------------------------------------------------------------------------- */
/* Directory scanning                                                        */
/* ------------------------------------------------------------------------- */

static int compare_entries(const void *a, const void *b)
{
	const dir_entry *x = a, *y = b;

	if (x->is_dir != y->is_dir)
		return x->is_dir ? -1 : 1;
	return strcasecmp(x->name, y->name);
}

static bool scan_dir(const char *path)
{
	entry_count = 0;

	DIR *dir = opendir(path);
	if (dir == NULL)
	{
		last_scan_ok = false;
		return false;
	}
	last_scan_ok = true;

	const size_t path_len = strlen(path);
	const bool trailing_slash = (path_len > 0 && path[path_len - 1] == '/');

	struct dirent *de;
	while ((de = readdir(dir)) != NULL && entry_count < MAX_ENTRIES)
	{
		if (de->d_name[0] == '.')
			continue;

		/* A name with a ROM extension is a file: no need to ask the
		 * filesystem, which costs an IPC round trip per entry. */
		bool is_dir = false;
		if (!has_rom_extension(de->d_name))
		{
			char full[768];
			snprintf(full, sizeof full, "%s%s%s", path,
				 trailing_slash ? "" : "/", de->d_name);

			struct stat st;
			is_dir = (stat(full, &st) == 0) && S_ISDIR(st.st_mode);
			if (!is_dir)
				continue; /* not a ROM and not a folder */
		}

		snprintf(entries[entry_count].name,
			 sizeof entries[entry_count].name, "%s", de->d_name);
		entries[entry_count].is_dir = is_dir;
		entry_count++;
	}
	closedir(dir);

	qsort(entries, entry_count, sizeof entries[0], compare_entries);
	return true;
}

static void join_path(char *out, size_t out_len, const char *dir,
		      const char *name)
{
	size_t n = strlen(dir);
	snprintf(out, out_len, "%s%s%s", dir,
		 (n > 0 && dir[n - 1] == '/') ? "" : "/", name);
}

/* Move up one directory, never above sdmc:/. Returns false at the root. */
static bool parent_dir(void)
{
	if (!strcmp(cur_dir, "sdmc:/"))
		return false;

	char *slash = strrchr(cur_dir, '/');
	if (slash != NULL && slash != cur_dir)
		*slash = '\0';
	if (strlen(cur_dir) < 6)
		strcpy(cur_dir, "sdmc:/");
	return true;
}

/* First visit: open the folder the last ROM came from, so a ROM that failed to
 * load does not mean walking the whole card again. */
static void dir_from_last_rom(void)
{
	if (g_cfg.rom_path[0] != '\0')
	{
		gbabe_copy(cur_dir, sizeof cur_dir, g_cfg.rom_path);
		char *slash = strrchr(cur_dir, '/');
		if (slash != NULL && slash != cur_dir)
		{
			*slash = '\0';
			return;
		}
	}
	strcpy(cur_dir, "sdmc:/");
}

/* ------------------------------------------------------------------------- */
/* File browser                                                              */
/* ------------------------------------------------------------------------- */

bool ui_browse_rom(char *out, size_t out_len)
{
	if (cur_dir[0] == '\0')
		dir_from_last_rom();

	int sel = 0;
	int top = 0;
	bool rescan = true;
	bool dirty = true;

	for (;;)
	{
		if (rescan)
		{
			scan_dir(cur_dir);
			rescan = false;
			dirty = true;

			if (sel >= entry_count)
				sel = (entry_count > 0) ? entry_count - 1 : 0;
		}

		if (dirty)
		{
			if (sel < top)
				top = sel;
			if (sel >= top + UI_LIST_ROWS)
				top = sel - UI_LIST_ROWS + 1;
			if (top < 0)
				top = 0;

			/* Build the array of name pointers the screen wants. */
			const char *names[MAX_ENTRIES];
			bool isdir[MAX_ENTRIES];
			for (int i = 0; i < entry_count; i++)
			{
				names[i] = entries[i].name;
				isdir[i] = entries[i].is_dir;
			}

			scr_browser("v" GBABE_VERSION, cur_dir, names, isdir,
				    entry_count, sel, top, last_scan_ok);
			ui_present();
			dirty = false;
		}

		touchPosition touch = { 0, 0 };
		u32 k = wait_input(&touch);
		if (k == 0 || (k & KEY_START))
			return false;

		const int prev_sel = sel;

		if (k & KEY_TOUCH)
		{
			if (touch.py < UI_LIST_Y)
			{
				/* Tapping the header or the path goes up a
				 * folder, which is the touch equivalent of B. */
				if (!parent_dir())
					return false;
				sel = 0;
				top = 0;
				rescan = true;
				continue;
			}

			int hit = scr_browser_hit(touch.py, top, entry_count);
			if (hit < 0)
				continue;
			if (hit != sel)
			{
				sel = hit;
				dirty = true;
				continue;
			}
			/* Second tap on the selected row opens it. */
			k = KEY_A;
		}

		if (k & (KEY_DDOWN | KEY_CPAD_DOWN))
		{
			if (sel + 1 < entry_count)
				sel++;
		}
		else if (k & (KEY_DUP | KEY_CPAD_UP))
		{
			if (sel > 0)
				sel--;
		}
		else if (k & (KEY_DRIGHT | KEY_CPAD_RIGHT | KEY_R))
		{
			sel = (sel + UI_LIST_ROWS < entry_count)
				      ? sel + UI_LIST_ROWS
				      : entry_count - 1;
			if (sel < 0)
				sel = 0;
		}
		else if (k & (KEY_DLEFT | KEY_CPAD_LEFT | KEY_L))
		{
			sel = (sel > UI_LIST_ROWS) ? sel - UI_LIST_ROWS : 0;
		}
		else if (k & KEY_B)
		{
			if (!parent_dir())
				return false; /* B at the root cancels */
			sel = 0;
			top = 0;
			rescan = true;
		}
		else if (k & KEY_A)
		{
			if (entry_count == 0)
				continue;

			char full[768];
			join_path(full, sizeof full, cur_dir, entries[sel].name);

			if (entries[sel].is_dir)
			{
				gbabe_copy(cur_dir, sizeof cur_dir, full);
				sel = 0;
				top = 0;
				rescan = true;
			}
			else
			{
				/* Leave cur_dir on the ROM's folder so the
				 * browser reopens there next time. */
				gbabe_copy(out, out_len, full);
				return true;
			}
		}

		if (sel != prev_sel)
			dirty = true;
	}
}

/* ------------------------------------------------------------------------- */
/* Pause menu                                                                */
/* ------------------------------------------------------------------------- */

static const char *scale_label(int m)
{
	switch (m)
	{
	case SCALE_ORIG: return "original";
	case SCALE_WIDE: return "stretch";
	default:         return "fit height";
	}
}

static const char *frameskip_label(int m)
{
	return (m == 0) ? "off" : "auto";
}

enum
{
	ACT_RESUME,
	ACT_RESET,
	ACT_LOAD,
	ACT_QUIT,
	ACT_PALETTE,
	ACT_VIDEO,
	ACT_SKIP,
	ACT_SOUND,
	ACT_HUD,
};

static const ui_row game_rows[] = {
	{ "Resume", NULL, UI_ROW_ACTION },
	{ "Reset game", NULL, UI_ROW_ACTION },
	{ "Load another ROM", NULL, UI_ROW_ACTION },
	{ "Save and quit", NULL, UI_ROW_ACTION },
};
#define GAME_ROWS ((int)(sizeof game_rows / sizeof game_rows[0]))

static const ui_row settings_rows[] = {
	{ "Palette", NULL, UI_ROW_CHOICE },
	{ "Video", NULL, UI_ROW_CHOICE },
	{ "Frameskip", NULL, UI_ROW_CHOICE },
	{ "Sound", NULL, UI_ROW_TOGGLE },
	{ "Status on screen", NULL, UI_ROW_TOGGLE },
};
#define SETTINGS_ROWS ((int)(sizeof settings_rows / sizeof settings_rows[0]))

static const int game_actions[] = { ACT_RESUME, ACT_RESET, ACT_LOAD,
				    ACT_QUIT };
static const int settings_actions[] = { ACT_PALETTE, ACT_VIDEO, ACT_SKIP,
					ACT_SOUND, ACT_HUD };

static const char *const menu_tabs[] = { "Game", "Settings" };
#define MENU_TABS ((int)(sizeof menu_tabs / sizeof menu_tabs[0]))

static const char *menu_note(int tab, int sel)
{
	if (tab == 0)
	{
		switch (game_actions[sel])
		{
		case ACT_RESET: return "Restart the game from the beginning.";
		case ACT_LOAD: return "Saves the current game first.";
		case ACT_QUIT: return "Writes the save file, then exits.";
		default: return "Back to the game.";
		}
	}

	switch (settings_actions[sel])
	{
	case ACT_PALETTE: return "Colour scheme for monochrome games.";
	case ACT_VIDEO: return "How the 160x144 screen is scaled up.";
	case ACT_SKIP: return "Fold an occasional frame when running late.";
	case ACT_SOUND: return "Turn the emulated audio output on or off.";
	default: return "The status readout on this screen.";
	}
}

static void draw_menu(int tab, int sel)
{
	ui_row rows[SETTINGS_ROWS];

	const ui_row *src = (tab == 0) ? game_rows : settings_rows;
	const int n = (tab == 0) ? GAME_ROWS : SETTINGS_ROWS;

	for (int i = 0; i < n; i++)
		rows[i] = src[i];

	rows[0].value = palette_name(palette_get());
	rows[1].value = scale_label(g_cfg.scale);
	rows[2].value = frameskip_label(g_cfg.frameskip);
	rows[3].value = g_cfg.audio_on ? "on" : "off";
	rows[4].value = g_cfg.show_hud ? "on" : "off";

	ui_menu m = {
		.title = "GBabe " GBABE_VERSION,
		.status = "paused",
		.tabs = menu_tabs,
		.n_tabs = MENU_TABS,
		.tab = tab,
		.rows = rows,
		.n_rows = n,
		.sel = sel,
		.note = menu_note(tab, sel),
	};

	scr_menu(&m);
	ui_present();
}

/* Run one menu action. delta is -1 / +1 for a change, 0 for "activate".
 * Returns false when the application should exit. */
static bool run_action(int act, int delta, bool *resume)
{
	switch (act)
	{
	case ACT_RESUME:
		*resume = true;
		return true;

	case ACT_RESET:
		emu_reset();
		config_save();
		*resume = true;
		return true;

	case ACT_LOAD:
	{
		char path[768];
		if (ui_browse_rom(path, sizeof path))
		{
			const char *err = emu_load(path);
			if (err != NULL)
			{
				ui_message("Cannot open ROM", err,
					   "Press A or B.");
				return true;
			}

			gbabe_copy(g_cfg.rom_path, sizeof g_cfg.rom_path, path);
			config_save();
			*resume = true;
		}
		return true;
	}

	case ACT_QUIT:
		config_save();
		return false;

	case ACT_PALETTE:
		delta = (delta == 0) ? 1 : delta;
		palette_set((palette_get() + delta + palette_count()) %
			    palette_count());
		g_cfg.palette = palette_get();
		config_save();
		return true;

	case ACT_VIDEO:
		delta = (delta == 0) ? 1 : delta;
		g_cfg.scale = (g_cfg.scale + delta + SCALE_COUNT) % SCALE_COUNT;
		video_set_scale(g_cfg.scale);
		config_save();
		return true;

	case ACT_SKIP:
		g_cfg.frameskip = (g_cfg.frameskip + 1) % 2;
		config_save();
		return true;

	case ACT_SOUND:
		g_cfg.audio_on = !g_cfg.audio_on;
		audio_set_enabled(g_cfg.audio_on);
		config_save();
		return true;

	case ACT_HUD:
		g_cfg.show_hud = !g_cfg.show_hud;
		config_save();
		return true;

	default:
		return true;
	}
}

bool ui_in_game_menu(void)
{
	int tab = 0;
	int sel = 0;
	bool dirty = true;
	bool resume = false;

	while (aptMainLoop())
	{
		if (dirty)
		{
			draw_menu(tab, sel);
			dirty = false;
		}

		touchPosition touch = { 0, 0 };
		u32 k = wait_input(&touch);
		if (k == 0)
			return false; /* HOME pressed */

		if (k & KEY_B)
		{
			config_save();
			if (!g_cfg.show_hud)
				ui_blank();
			return true;
		}

		if (k & KEY_TOUCH)
		{
			const int t = scr_menu_hit_tab(touch.px, touch.py,
						       menu_tabs, MENU_TABS);
			if (t >= 0)
			{
				if (t != tab)
				{
					tab = t;
					sel = 0;
				}
				dirty = true;
				continue;
			}

			const int n = (tab == 0) ? GAME_ROWS : SETTINGS_ROWS;
			const int row = scr_menu_hit_row(touch.py, n);
			if (row < 0)
				continue;

			if (row != sel)
			{
				sel = row;
				dirty = true;
				continue;
			}
			k = KEY_A; /* second tap activates */
		}

		if (k & (KEY_DDOWN | KEY_CPAD_DOWN))
		{
			const int n = (tab == 0) ? GAME_ROWS : SETTINGS_ROWS;
			sel = (sel + 1) % n;
			dirty = true;
			continue;
		}
		if (k & (KEY_DUP | KEY_CPAD_UP))
		{
			const int n = (tab == 0) ? GAME_ROWS : SETTINGS_ROWS;
			sel = (sel == 0) ? n - 1 : sel - 1;
			dirty = true;
			continue;
		}
		if (k & (KEY_L | KEY_R | KEY_DLEFT | KEY_DRIGHT | KEY_CPAD_LEFT |
			 KEY_CPAD_RIGHT))
		{
			/* L/R and left/right both move: L/R switch tabs, the
			 * D-Pad changes the value on the current row. */
			if (k & (KEY_L | KEY_R))
			{
				tab = (tab + 1) % MENU_TABS;
				sel = 0;
				dirty = true;
				continue;
			}

			const int dir = (k & (KEY_DRIGHT | KEY_CPAD_RIGHT)) ? 1
									     : -1;
			const int act = (tab == 0) ? game_actions[sel]
						   : settings_actions[sel];
			if (!run_action(act, dir, &resume))
				return false;
			dirty = true;
			continue;
		}

		if (k & (KEY_A | KEY_START))
		{
			const int act = (tab == 0) ? game_actions[sel]
						   : settings_actions[sel];
			if (!run_action(act, 0, &resume))
				return false;

			if (resume)
			{
				if (!g_cfg.show_hud)
					ui_blank();
				return true;
			}
			dirty = true;
		}
	}

	return false;
}

/* ------------------------------------------------------------------------- */
/* Messages                                                                  */
/* ------------------------------------------------------------------------- */

void ui_message(const char *title, const char *line1, const char *line2)
{
	scr_message(title, line1, line2, "Press A or B");
	ui_present();

	for (;;)
	{
		u32 k = wait_input(NULL);
		if (k == 0 || (k & (KEY_A | KEY_B | KEY_START | KEY_TOUCH)))
			return;
	}
}

/* ------------------------------------------------------------------------- */
/* Status readout                                                            */
/* ------------------------------------------------------------------------- */

void ui_draw_hud(void)
{
	const emu_info *info = emu_get_info();
	char fps[16], frame[24];
	char stack[6][24];

	const char *labels[] = { "core", "blit", "wait", "frame", "sound",
				 "palette" };
	const char *values[6];

	snprintf(fps, sizeof fps, "%.1f", g_perf.fps);
	snprintf(frame, sizeof frame, "%.1f ms/frame", g_perf.frame_ms);

	snprintf(stack[0], sizeof stack[0], "%.1f ms", g_perf.core_ms);
	snprintf(stack[1], sizeof stack[1], "%.1f ms", g_perf.blit_ms);
	snprintf(stack[2], sizeof stack[2], "%.1f ms", g_perf.wait_ms);
	snprintf(stack[3], sizeof stack[3], "%.1f ms", g_perf.frame_ms);
	snprintf(stack[4], sizeof stack[4], "%u ms", audio_queue_ms());
	snprintf(stack[5], sizeof stack[5], "%s", palette_name(palette_get()));

	for (int i = 0; i < 6; i++)
		values[i] = stack[i];

	char sub[64];
	snprintf(sub, sizeof sub, "%s   %u Mbit   MBC%d",
		 info->cgb ? "CGB" : "DMG",
		 (unsigned)(info->rom_banks * 16 / 128), info->mbc);

	ui_hud h = {
		.title = info->title,
		.sub = sub,
		.fps = g_perf.fps,
		.frame_ms = g_perf.frame_ms,
		.core_ms = g_perf.core_ms,
		.blit_ms = g_perf.blit_ms,
		.wait_ms = g_perf.wait_ms,
		.stat_label = { labels[0], labels[1], labels[2], labels[3],
				labels[4], labels[5] },
		.stat_value = { values[0], values[1], values[2], values[3],
				values[4], values[5] },
		.n_stats = 6,
	};

	scr_hud(&h);
	ui_present();
}

void ui_note_frame(u64 core_ticks, u64 blit_ticks, u64 wait_ticks,
		   u64 frame_ticks)
{
	hud_frames++;
	hud_sum_core += core_ticks;
	hud_sum_blit += blit_ticks;
	hud_sum_wait += wait_ticks;
	hud_sum_frame += frame_ticks;

	u64 now = svcGetSystemTick();
	if (hud_last_tick == 0)
	{
		hud_last_tick = now;
		return;
	}

	const double elapsed = (double)(now - hud_last_tick) / SYSCLOCK_ARM11;
	if (elapsed < 1.0)
		return;

	hud_fps = (float)(hud_frames / elapsed);

	/* Milliseconds per presented frame, from the tick sums. */
	const double ticks_to_ms = 1000.0 / SYSCLOCK_ARM11;
	const double n = (hud_frames > 0) ? (double)hud_frames : 1.0;
	g_perf.fps = hud_fps;
	g_perf.core_ms = (float)((double)hud_sum_core * ticks_to_ms / n);
	g_perf.blit_ms = (float)((double)hud_sum_blit * ticks_to_ms / n);
	g_perf.wait_ms = (float)((double)hud_sum_wait * ticks_to_ms / n);
	g_perf.frame_ms = (float)((double)hud_sum_frame * ticks_to_ms / n);

	hud_frames = 0;
	hud_sum_core = hud_sum_blit = hud_sum_wait = hud_sum_frame = 0;
	hud_last_tick = now;

	/* The status readout is on the bottom screen, so it costs a redraw once
	 * a second - nothing during the frames in between. */
	if (g_cfg.show_hud && emu_loaded())
		ui_draw_hud();
}

/* ------------------------------------------------------------------------- */
/* Setup                                                                     */
/* ------------------------------------------------------------------------- */

void ui_blank(void)
{
	scr_blank();
	ui_present();
}

void ui_init(void)
{
	ui_blank();
}

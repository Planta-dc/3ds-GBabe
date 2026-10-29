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
#include "log.h"
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

/* HUD statistics, accumulated over a one second window. */
static unsigned hud_frames;
static u64      hud_last_tick;
static bool     hud_due;         /* the status readout needs a redraw */
static float    hud_fps;
static u64      hud_sum_core, hud_sum_blit, hud_sum_sync, hud_sum_post,
                hud_sum_wait, hud_sum_frame;
static u64      hud_worst; /* slowest single frame in the window */
static unsigned hud_late;    /* frames that started after their slot      */
static unsigned hud_resyncs; /* thrown-away backlogs (visible jumps)      */

/* ------------------------------------------------------------------------- */
/* Input                                                                     */
/* ------------------------------------------------------------------------- */

/* Blocking wait for the next input event. Returns the keys just pressed, with
 * KEY_TOUCH set (and `touch` filled in) for a tap; 0 means HOME was pressed and
 * the application should quit.
 *
 * Buttons repeat while held (hidKeysDownRepeat), but the touch bit must not:
 * libctru's repeat logic re-emits whatever is in the key state, and on the touch
 * screen "held" means a finger resting on the glass. Left in, a finger held on a
 * menu row would select it and then activate it 15 frames later, and a finger on
 * a folder would walk into it over and over. So the press comes from
 * hidKeysDown() (first frame only) and repeats are buttons only. */
static u32 wait_input(touchPosition *touch)
{
	while (aptMainLoop())
	{
		hidScanInput();

		const u32 down = hidKeysDown();
		const u32 rep = hidKeysDownRepeat() & ~KEY_TOUCH;
		const u32 k = down | rep;

		if (k != 0)
		{
			if ((k & KEY_TOUCH) && touch != NULL)
				hidTouchRead(touch);
			return k;
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
			if (touch.py >= UI_PATH_Y && touch.py < UI_PATH_Y + UI_PATH_H)
			{
				/* Tapping the path goes up a folder (the touch
				 * equivalent of B). At sdmc:/ there is nowhere to
				 * go, so do nothing - quitting the application on
				 * a stray tap would be a nasty surprise. B still
				 * cancels from here, as it always did. */
				if (parent_dir())
				{
					sel = 0;
					top = 0;
					rescan = true;
				}
				continue;
			}

			const int hit = scr_browser_hit(touch.py, top, entry_count);
			if (hit < 0)
				continue; /* footer, gap, past the end */

			/* One tap opens the row - folders included. Selecting
			 * something and then requiring a second tap to use it is
			 * a fine desktop idiom and a bad stylus one. */
			sel = hit;
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
	ACT_LOG,
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
	{ "Debug log", NULL, UI_ROW_TOGGLE },
};
#define SETTINGS_ROWS ((int)(sizeof settings_rows / sizeof settings_rows[0]))

static const int game_actions[] = { ACT_RESUME, ACT_RESET, ACT_LOAD,
				    ACT_QUIT };
static const int settings_actions[] = { ACT_PALETTE, ACT_VIDEO, ACT_SKIP,
					ACT_SOUND, ACT_HUD, ACT_LOG };

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
	case ACT_LOG: return "Writes sdmc:/3ds/gbabe/debug.log.";
	default: return "The status readout on this screen.";
	}
}

static void draw_menu(int tab, int sel)
{
	/* A copy, because the settings tab shows live values rather than the
	 * table above. Only the rows this tab actually has are filled in. */
	ui_row rows[SETTINGS_ROWS];

	const bool settings = (tab != 0);
	const ui_row *src = settings ? settings_rows : game_rows;
	const int n = settings ? SETTINGS_ROWS : GAME_ROWS;

	for (int i = 0; i < n; i++)
		rows[i] = src[i];

	if (settings)
	{
		rows[0].value = palette_name(palette_get());
		rows[1].value = scale_label(g_cfg.scale);
		rows[2].value = frameskip_label(g_cfg.frameskip);
		rows[3].value = g_cfg.audio_on ? "on" : "off";
		rows[4].value = g_cfg.show_hud ? "on" : "off";
		rows[5].value = g_cfg.debug_log ? "on" : "off";
	}

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

/* Run one menu action. delta is -1 / +1 when a value is being stepped, 0 when
 * the row was activated. *close is set by the three rows that end the menu
 * (resume, reset, load another ROM) and never by a row that just holds a value -
 * tapping "Sound" must flip it *in* the menu, not leave the menu.
 * Returns false when the application should exit. */
static bool run_action(int act, int delta, bool *close)
{
	switch (act)
	{
	case ACT_RESUME:
		*close = true;
		return true;

	case ACT_RESET:
		emu_reset();
		config_save();
		*close = true;
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
			*close = true;
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
		emu_palette_changed();
		g_cfg.palette = palette_get();
		log_line("setting palette=%d (%s)", g_cfg.palette,
			 palette_name(palette_get()));
		config_save();
		return true;

	case ACT_VIDEO:
		delta = (delta == 0) ? 1 : delta;
		g_cfg.scale = (g_cfg.scale + delta + SCALE_COUNT) % SCALE_COUNT;
		video_set_scale(g_cfg.scale);
		log_line("setting scale=%d", g_cfg.scale);
		config_save();
		return true;

	case ACT_SKIP:
		g_cfg.frameskip = (g_cfg.frameskip + 1) % 2;
		log_line("setting frameskip=%d", g_cfg.frameskip);
		config_save();
		return true;

	case ACT_SOUND:
		g_cfg.audio_on = !g_cfg.audio_on;
		audio_set_enabled(g_cfg.audio_on);
		log_line("setting sound=%d", g_cfg.audio_on);
		config_save();
		return true;

	case ACT_HUD:
		g_cfg.show_hud = !g_cfg.show_hud;
		log_line("setting hud=%d", g_cfg.show_hud);
		config_save();
		return true;

	case ACT_LOG:
		g_cfg.debug_log = !g_cfg.debug_log;
		/* Turning it on has to reopen the file, which needs the path. */
		if (g_cfg.debug_log)
			log_init(GBABE_LOG_PATH, true);
		else
			log_set_enabled(false);
		config_save();
		return true;

	default:
		return true;
	}
}

/* What kind of row is selected? The tables above are static, so this is a
 * lookup rather than a field - the screens only need the value, and ui.c needs
 * to know whether left/right makes sense there. */
static ui_row_kind row_kind(int tab, int sel)
{
	if (tab != 0)
		return settings_rows[sel].kind;
	return UI_ROW_ACTION;
}

bool ui_in_game_menu(void)
{
	int tab = 0;
	int sel = 0;
	bool dirty = true;

	for (;;)
	{
		const int n_rows = (tab == 0) ? GAME_ROWS : SETTINGS_ROWS;

		if (sel < 0 || sel >= n_rows)
			sel = 0;

		if (dirty)
		{
			draw_menu(tab, sel);
			dirty = false;
		}

		touchPosition touch = { 0, 0 };
		u32 k = wait_input(&touch);
		if (k == 0)
			return false; /* HOME pressed */

		/* B always means "back to the game", from anywhere. */
		if (k & KEY_B)
		{
			config_save();
			if (!g_cfg.show_hud)
				ui_blank();
			return true;
		}

		/* delta == 0 means "the row was activated"; -1/+1 means a value
		 * is being stepped. Nothing else distinguishes the two below. */
		int delta = 0;

		if (k & KEY_TOUCH)
		{
			const int t = scr_menu_hit_tab(touch.px, touch.py,
						       menu_tabs, MENU_TABS);
			if (t >= 0)
			{
				tab = t;
				sel = 0;
				dirty = true;
				continue;
			}

			const int row = scr_menu_hit_row(touch.py,
							 (tab == 0) ? GAME_ROWS
								    : SETTINGS_ROWS);
			if (row < 0)
				continue; /* the note, the footer, empty space */

			if (row != sel)
			{
				/* First tap selects. Menu rows are not like
				 * browser rows: one of them restarts the game
				 * and another quits, and neither should happen
				 * because a thumb brushed the glass. */
				sel = row;
				dirty = true;
				continue;
			}
			/* Tapping the row that is already selected uses it. */
			dirty = true;
		}
		else if (k & (KEY_DDOWN | KEY_CPAD_DOWN))
		{
			sel = (sel + 1) % n_rows;
			dirty = true;
			continue;
		}
		else if (k & (KEY_DUP | KEY_CPAD_UP))
		{
			sel = (sel + n_rows - 1) % n_rows;
			dirty = true;
			continue;
		}
		else if (k & (KEY_L | KEY_R))
		{
			tab = (tab + 1) % MENU_TABS;
			sel = 0;
			dirty = true;
			continue;
		}
		else if (k & (KEY_DLEFT | KEY_DRIGHT | KEY_CPAD_LEFT |
			      KEY_CPAD_RIGHT))
		{
			/* Left/right changes a value, and does nothing at all on a
			 * row that is an action: a nudge on "Save and quit" must
			 * not quit, and one on "Reset game" must not reset. */
			if (row_kind(tab, sel) == UI_ROW_ACTION)
				continue;

			delta = (k & (KEY_DRIGHT | KEY_CPAD_RIGHT)) ? 1 : -1;
		}
		else if (!(k & (KEY_A | KEY_START)))
		{
			continue; /* a key this screen does not use */
		}

		const int act = (tab == 0) ? game_actions[sel]
					   : settings_actions[sel];

		bool close = false;
		if (!run_action(act, delta, &close))
			return false; /* asked to quit */

		if (close)
		{
			if (!g_cfg.show_hud)
				ui_blank();
			return true;
		}

		/* A value changed: stay in the menu and redraw it. */
		dirty = true;
	}
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

/* One sentence about what is costing the frames, from the profile itself. The
 * thresholds are deliberately generous: a frame has 16.7 ms and the Game Boy
 * runs at 59.7275 Hz, so ~15.5 ms of work is still full speed with margin. */
static const char *limit_text(float fps, float core, float blit, float other)
{
	if (fps >= 55.0f)
		return "full speed";

	if (core >= blit && core >= other)
		return "CPU-bound (interpreter)";
	if (blit >= other)
		return "draw-bound (blit)";
	return "overhead (see other)";
}

/* Draw the status readout into the canvas. Cheap: it is text and rectangles in
 * RAM. Presenting it is what costs - see ui_idle_work(). */
static void hud_render(void)
{
	const emu_info *info = emu_get_info();
	char fps[16], frame[24];
	char stack[6][24];

	const char *labels[] = { "core", "blit", "wait", "frame", "sound",
				 "palette" };
	const char *values[6];

	/* The panel shows one idle figure ("wait"); the log line keeps the two
	 * kinds apart, because "waiting for the panel" and "pacing to 59.7275
	 * Hz" are different problems even though both are time spent asleep. */
	const float idle_ms = g_perf.sync_ms + g_perf.wait_ms;
	const float other = g_perf.frame_ms - (g_perf.core_ms + g_perf.blit_ms +
					       idle_ms);

	snprintf(fps, sizeof fps, "%.1f", g_perf.fps);
	snprintf(frame, sizeof frame, "%.1f ms/frame", g_perf.frame_ms);

	snprintf(stack[0], sizeof stack[0], "%.1f ms", g_perf.core_ms);
	snprintf(stack[1], sizeof stack[1], "%.1f ms", g_perf.blit_ms);
	snprintf(stack[2], sizeof stack[2], "%.1f ms", idle_ms);
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
		.worst_ms = g_perf.worst_ms,
		.other_ms = (other > 0.0f) ? other : 0.0f,
		.limit = limit_text(g_perf.fps, g_perf.core_ms, g_perf.blit_ms,
				    other),
		.core_ms = g_perf.core_ms,
		.blit_ms = g_perf.blit_ms,
		.wait_ms = idle_ms,
		.stat_label = { labels[0], labels[1], labels[2], labels[3],
				labels[4], labels[5] },
		.stat_value = { values[0], values[1], values[2], values[3],
				values[4], values[5] },
		.n_stats = 6,
	};

	scr_hud(&h);
}

void ui_draw_hud(void)
{
	hud_render();
	ui_present();
}

void ui_note_frame(u64 core_ticks, u64 blit_ticks, u64 sync_ticks,
		   u64 post_ticks, u64 wait_ticks, u64 frame_ticks,
		   unsigned late_frames, unsigned resyncs)
{
	hud_late += late_frames;
	hud_resyncs += resyncs;
	hud_frames++;
	hud_sum_core += core_ticks;
	hud_sum_blit += blit_ticks;
	hud_sum_sync += sync_ticks;
	hud_sum_post += post_ticks;
	hud_sum_wait += wait_ticks;
	hud_sum_frame += frame_ticks;
	if (frame_ticks > hud_worst)
		hud_worst = frame_ticks;

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
	g_perf.sync_ms = (float)((double)hud_sum_sync * ticks_to_ms / n);
	g_perf.post_ms = (float)((double)hud_sum_post * ticks_to_ms / n);
	g_perf.wait_ms = (float)((double)hud_sum_wait * ticks_to_ms / n);
	g_perf.frame_ms = (float)((double)hud_sum_frame * ticks_to_ms / n);
	g_perf.worst_ms = (float)((double)hud_worst * ticks_to_ms);

	/* The same numbers the status readout shows, in the log: this is the
	 * measurement a slow-console report needs, and a screenshot of the bottom
	 * screen is a lot to ask of someone. sync_skip/sync_late are the present
	 * path's own counters: a frame that could not afford the vblank wait
	 * (and may tear), and a swap the panel was a vblank late in taking.
	 * pace_late/pace_resync are the frame loop's: a frame that started after
	 * its own slot, and a thrown-away backlog. */
	unsigned sync_skip, sync_late;

	video_sync_take(&sync_skip, &sync_late);

	log_line("perf fps=%.1f frame=%.1f worst=%.1f core=%.1f blit=%.1f "
		 "post=%.1f sync=%.1f wait=%.1f other=%.1f audio=%ums "
		 "sync_skip=%u sync_late=%u pace_late=%u resync=%u", g_perf.fps,
		 g_perf.frame_ms, g_perf.worst_ms, g_perf.core_ms, g_perf.blit_ms,
		 g_perf.post_ms, g_perf.sync_ms, g_perf.wait_ms,
		 g_perf.frame_ms - (g_perf.core_ms + g_perf.blit_ms +
				    g_perf.post_ms + g_perf.sync_ms +
				    g_perf.wait_ms),
		 audio_queue_ms(), sync_skip, sync_late, hud_late, hud_resyncs);

	hud_frames = 0;
	hud_late = hud_resyncs = 0;
	hud_sum_core = hud_sum_blit = hud_sum_sync = hud_sum_post =
		hud_sum_wait = hud_sum_frame = 0;
	hud_worst = 0;
	hud_last_tick = now;

	/* The status readout is on the bottom screen, so it costs a redraw once
	 * a second - nothing during the frames in between. Not drawn here: this
	 * runs after the frame has already been handed to the panel, in the
	 * 0.1 ms of slack the loop has left, and a 1-second redraw there cost a
	 * dropped frame every second ("worst=33.4" once a second in the log).
	 * ui_idle_work() draws it instead, in the dead time before the vblank
	 * wait. */
	if (g_cfg.show_hud && emu_loaded())
		hud_due = true;
}

/* Everything worth doing exactly once a second, done where it is free.
 *
 * The status readout is the expensive part: presenting it means transposing
 * 76800 pixels and flushing 153 KiB of bottom-screen framebuffer, which the
 * console measured at ~16 ms - a whole frame. Moving that work into the vblank
 * wait was not enough (§12): the wait is only ~6 ms, so the frame still
 * overran. It is now spent one band of panel columns per frame: eight frames of
 * ~2 ms, each of which fits in the wait, and the panel keeps showing the
 * previous buffer until the swap at the end. */
void ui_idle_work(void)
{
	static bool presenting;
	static u64 render_ticks, band_ticks, worst_band;
	static unsigned bands;

	if (!presenting && hud_due)
	{
		hud_due = false;

		if (!g_cfg.show_hud || !emu_loaded())
			return;

		const u64 t0 = svcGetSystemTick();
		hud_render();
		render_ticks = svcGetSystemTick() - t0;

		if (!ui_present_begin())
			return;

		presenting = true;
		bands = 0;
		band_ticks = worst_band = 0;
	}

	if (!presenting)
		return;

	const u64 t0 = svcGetSystemTick();
	const bool done = ui_present_band(UI_W / 8);
	const u64 took = svcGetSystemTick() - t0;

	band_ticks += took;
	if (took > worst_band)
		worst_band = took;
	bands++;

	if (!done)
		return;

	ui_present_swap();
	presenting = false;

	/* The split, once a second: if `present` stays near 16 ms total but no
	 * band is over ~2 ms, the work is free and no frame can be lost to it. */
	const double ms = 1000.0 / SYSCLOCK_ARM11;
	log_line("hud: render %.1f ms + present %.1f ms in %u bands "
		 "(worst band %.1f ms)", (double)render_ticks * ms,
		 (double)band_ticks * ms, bands, (double)worst_band * ms);
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

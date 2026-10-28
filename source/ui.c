/**
 * Bottom screen UI: ROM browser, in-game menu and status readout.
 *
 * libctru's text console gives 30 columns x 40 rows of 8x8 characters on the
 * bottom screen, so every layout below stays inside that box. The console owns
 * the bottom framebuffer (it turns double buffering off for that screen); the
 * emulator keeps the top screen to itself.
 *
 * Everything here is drawn through a 1200-byte shadow screen that is written to
 * the console in a single call, for two reasons that matter a lot on an Old
 * 3DS and were the reason the file browser felt unusable:
 *
 *   - libctru's console calls gfxFlushBuffers() on every '\r' and '\n', and
 *     that flushes the 300 KiB bottom framebuffer;
 *   - a per-character or per-line write re-parses escape sequences and redraws
 *     every glyph one at a time.
 *
 * So: no newlines, no per-line cursor addressing, one write, one flush. The
 * directory is also only rescanned when it actually changes, instead of on
 * every key press, and ROM-named entries are never stat()'d (which is an IPC
 * round-trip to the SD filesystem each time).
 *
 * MIT licensed.
 */
#include "ui.h"

#include <3ds.h>

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "audio.h"
#include "emu.h"
#include "palette.h"
#include "video.h"

#define UI_COLS     30
#define UI_ROWS     40
#define MAX_ENTRIES 400

/* Rows 5..30 of the browser are the list; the rest is chrome. */
#define LIST_TOP    5
#define LIST_ROWS   26

typedef struct
{
	char name[256];
	bool is_dir;
} dir_entry;

static dir_entry entries[MAX_ENTRIES];
static int       entry_count;
static char      cur_dir[512];
static bool      last_scan_ok;

/* Shadow screen: exactly one console page, written in one go. */
static char screen_buf[UI_ROWS * UI_COLS];

/* HUD statistics. */
static unsigned hud_frames;
static u64      hud_last_tick;
static float    hud_fps;
static u64      hud_sum_core, hud_sum_blit, hud_sum_wait, hud_sum_frame;

/* ------------------------------------------------------------------------- */
/* Shadow screen                                                             */
/* ------------------------------------------------------------------------- */

static void scr_clear(void)
{
	memset(screen_buf, ' ', sizeof screen_buf);
}

/* Write text at a 1-based row/column, clipped to the console width. */
static void scr_line(int row, int col, const char *fmt, ...)
{
	char buf[UI_COLS + 1];
	va_list ap;

	if (row < 1 || row > UI_ROWS || col < 1 || col > UI_COLS)
		return;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);

	size_t len = strlen(buf);
	size_t room = (size_t)(UI_COLS - (col - 1));
	if (len > room)
		len = room;

	char *dst = &screen_buf[(row - 1) * UI_COLS + (col - 1)];
	memcpy(dst, buf, len);

	/* A multi-byte character cut in half by the clip would leave the
	 * console decoder in a bad state; pad with spaces instead. */
	for (size_t i = len; i < room && dst[i] != ' '; i++)
		dst[i] = ' ';
}

/* One write for the whole screen. No '\n' or '\r', so the console does not
 * flush its framebuffer per line, and no escape sequence per line. */
static void scr_flush(void)
{
	fputs("\x1b[H", stdout); /* home, no arguments */
	fwrite(screen_buf, 1, sizeof screen_buf, stdout);
	gfxFlushBuffers();
}

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* Blocking wait for a key press. Returns 0 when the user pressed HOME. */
static u32 wait_key(void)
{
	while (aptMainLoop())
	{
		hidScanInput();
		u32 down = hidKeysDownRepeat();
		if (down != 0)
			return down;
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
		 * filesystem, which costs an IPC per entry. */
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

/* Move up one directory, never above sdmc:/. */
static void parent_dir(void)
{
	if (!strcmp(cur_dir, "sdmc:/"))
		return;

	char *slash = strrchr(cur_dir, '/');
	if (slash != NULL && slash != cur_dir)
		*slash = '\0';
	if (strlen(cur_dir) < 6)
		strcpy(cur_dir, "sdmc:/");
}

/* First visit: open the folder the last ROM came from, so a ROM that failed to
 * load (or a first run after picking one once) does not mean walking the whole
 * card again. */
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

static void draw_browser(int sel, int first)
{
	scr_clear();
	scr_line(1, 1, "GBabe " GBABE_VERSION " - pick a ROM");
	scr_line(2, 1, "%.29s", cur_dir);
	scr_line(3, 1, "-----------------------------");

	if (entry_count == 0)
		scr_line(LIST_TOP + 1, 1, last_scan_ok ? "(no ROMs in here)"
						       : "(cannot open folder)");

	for (int i = 0; i < LIST_ROWS && first + i < entry_count; i++)
	{
		int idx = first + i;
		scr_line(LIST_TOP + i, 1, "%c%c%.27s",
			 (idx == sel) ? '>' : ' ',
			 entries[idx].is_dir ? '/' : ' ',
			 entries[idx].name);
	}

	if (entry_count > 0)
		scr_line(32, 1, "%d/%d", sel + 1, entry_count);

	scr_line(34, 1, "A open    B back");
	scr_line(35, 1, "START quit");
	scr_line(36, 1, "D-Pad/CPAD move, L/R page");
	scr_flush();
}

bool ui_browse_rom(char *out, size_t out_len)
{
	if (cur_dir[0] == '\0')
		dir_from_last_rom();

	int sel = 0;
	int first = 0;
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
			if (sel < first)
				first = sel;
			if (sel >= first + LIST_ROWS)
				first = sel - LIST_ROWS + 1;

			draw_browser(sel, first);
			dirty = false;
		}

		u32 k = wait_key();
		if (k == 0 || (k & KEY_START))
			return false;

		const int prev_sel = sel;

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
			sel = (sel + LIST_ROWS < entry_count) ? sel + LIST_ROWS
							      : entry_count - 1;
			if (sel < 0)
				sel = 0;
		}
		else if (k & (KEY_DLEFT | KEY_CPAD_LEFT | KEY_L))
		{
			sel = (sel > LIST_ROWS) ? sel - LIST_ROWS : 0;
		}
		else if (k & KEY_B)
		{
			if (!strcmp(cur_dir, "sdmc:/"))
				return false; /* B at the root cancels */

			parent_dir();
			sel = first = 0;
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
				sel = first = 0;
				rescan = true;
			}
			else
			{
				/* Leave cur_dir on the ROM's folder so the browser
				 * reopens there next time. */
				gbabe_copy(out, out_len, full);
				return true;
			}
		}

		if (sel != prev_sel)
			dirty = true;
	}
}

/* ------------------------------------------------------------------------- */
/* In-game menu                                                              */
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
	ROW_RESUME = 3,
	ROW_PALETTE,
	ROW_VIDEO,
	ROW_SKIP,
	ROW_SOUND,
	ROW_STATUS,
	ROW_RESET,
	ROW_LOAD,
	ROW_QUIT,
	MENU_ROWS = 9,
};

static void draw_menu(int sel)
{
	char rows[MENU_ROWS][32];

	snprintf(rows[0], 32, "Resume");
	snprintf(rows[1], 32, "Palette  <%s>", palette_name(palette_get()));
	snprintf(rows[2], 32, "Video    <%s>", scale_label(g_cfg.scale));
	snprintf(rows[3], 32, "Skip     <%s>", frameskip_label(g_cfg.frameskip));
	snprintf(rows[4], 32, "Sound    <%s>", g_cfg.audio_on ? "on" : "off");
	snprintf(rows[5], 32, "Status   <%s>", g_cfg.show_hud ? "on" : "off");
	snprintf(rows[6], 32, "Reset game");
	snprintf(rows[7], 32, "Load another ROM");
	snprintf(rows[8], 32, "Save and quit");

	scr_clear();
	scr_line(1, 1, "GBabe " GBABE_VERSION " - paused");

	for (int i = 0; i < MENU_ROWS; i++)
		scr_line(ROW_RESUME + i, 1, "%c%.28s", (sel == i) ? '>' : ' ',
			 rows[i]);

	const emu_info *info = emu_get_info();
	scr_line(15, 1, "%.16s %s", info->title, info->cgb ? "CGB" : "DMG");
	scr_line(16, 1, "%u Mbit  MBC%d",
		 (unsigned)(info->rom_banks * 16 / 128), info->mbc);

	scr_line(18, 1, "fps %.1f  frame %.0f ms", g_perf.fps, g_perf.frame_ms);
	scr_line(19, 1, "core %.1f  blit %.1f  wait %.1f",
		 g_perf.core_ms, g_perf.blit_ms, g_perf.wait_ms);

	scr_line(21, 1, "D-Pad      move");
	scr_line(22, 1, "L/R        change");
	scr_line(23, 1, "A/START    select");
	scr_line(24, 1, "B          resume");

	scr_line(26, 1, "In game: X turbo");
	scr_line(27, 1, "         Y palette");
	scr_line(28, 1, "Touch = this menu");
	scr_flush();
}

/* Blank the bottom screen (when the HUD is switched off, so the menu does not
 * linger behind the game). */
static void scr_blank(void)
{
	scr_clear();
	scr_flush();
}

bool ui_in_game_menu(void)
{
	int sel = 0;
	bool redraw = true;

	while (aptMainLoop())
	{
		if (redraw)
		{
			draw_menu(sel);
			redraw = false;
		}

		u32 k = wait_key();
		if (k == 0)
			return false; /* HOME pressed */

		if (k & (KEY_DDOWN | KEY_CPAD_DOWN))
		{
			sel = (sel + 1) % MENU_ROWS;
			redraw = true;
		}
		else if (k & (KEY_DUP | KEY_CPAD_UP))
		{
			sel = (sel == 0) ? MENU_ROWS - 1 : sel - 1;
			redraw = true;
		}
		else if (k & (KEY_DRIGHT | KEY_CPAD_RIGHT | KEY_R))
		{
			switch (sel)
			{
			case ROW_PALETTE - ROW_RESUME:
				palette_set((palette_get() + 1) % palette_count());
				g_cfg.palette = palette_get();
				break;
			case ROW_VIDEO - ROW_RESUME:
				g_cfg.scale = (g_cfg.scale + 1) % SCALE_COUNT;
				video_set_scale(g_cfg.scale);
				break;
			case ROW_SKIP - ROW_RESUME:
				g_cfg.frameskip = (g_cfg.frameskip + 1) % 2;
				break;
			case ROW_SOUND - ROW_RESUME:
				g_cfg.audio_on = true;
				audio_set_enabled(true);
				break;
			case ROW_STATUS - ROW_RESUME:
				g_cfg.show_hud = true;
				break;
			default:
				break;
			}
			redraw = true;
		}
		else if (k & (KEY_DLEFT | KEY_CPAD_LEFT | KEY_L))
		{
			switch (sel)
			{
			case ROW_PALETTE - ROW_RESUME:
				palette_set((palette_get() + palette_count() - 1) %
					    palette_count());
				g_cfg.palette = palette_get();
				break;
			case ROW_VIDEO - ROW_RESUME:
				g_cfg.scale = (g_cfg.scale + SCALE_COUNT - 1) %
					      SCALE_COUNT;
				video_set_scale(g_cfg.scale);
				break;
			case ROW_SKIP - ROW_RESUME:
				g_cfg.frameskip = (g_cfg.frameskip + 1) % 2;
				break;
			case ROW_SOUND - ROW_RESUME:
				g_cfg.audio_on = false;
				audio_set_enabled(false);
				break;
			case ROW_STATUS - ROW_RESUME:
				g_cfg.show_hud = false;
				break;
			default:
				break;
			}
			redraw = true;
		}
		else if (k & (KEY_A | KEY_START))
		{
			if (sel == 0 || sel == ROW_QUIT - ROW_RESUME)
			{
				/* Resume, or save and quit. */
				config_save();
				if (!g_cfg.show_hud)
					scr_blank();
				return sel == 0;
			}

			if (sel == ROW_RESET - ROW_RESUME)
			{
				emu_reset();
				config_save();
				scr_blank();
				return true;
			}

			if (sel == ROW_LOAD - ROW_RESUME)
			{
				char path[768];
				if (ui_browse_rom(path, sizeof path))
				{
					const char *err = emu_load(path);
					if (err == NULL)
					{
						gbabe_copy(g_cfg.rom_path,
							  sizeof g_cfg.rom_path,
							  path);
						config_save();
						scr_blank();
						return true;
					}

					scr_clear();
					scr_line(2, 1, "%.28s", err);
					scr_line(5, 1, "Press any button.");
					scr_flush();
					wait_key();
				}
				redraw = true;
				continue;
			}

			/* Value rows: A flips the setting. */
			switch (sel)
			{
			case ROW_SOUND - ROW_RESUME:
				g_cfg.audio_on = !g_cfg.audio_on;
				audio_set_enabled(g_cfg.audio_on);
				break;
			case ROW_STATUS - ROW_RESUME:
				g_cfg.show_hud = !g_cfg.show_hud;
				break;
			case ROW_VIDEO - ROW_RESUME:
				g_cfg.scale = (g_cfg.scale + 1) % SCALE_COUNT;
				video_set_scale(g_cfg.scale);
				break;
			case ROW_SKIP - ROW_RESUME:
				g_cfg.frameskip = (g_cfg.frameskip + 1) % 2;
				break;
			default:
				break;
			}
			redraw = true;
		}
		else if (k & (KEY_B | KEY_TOUCH))
		{
			config_save();
			if (!g_cfg.show_hud)
				scr_blank();
			return true;
		}
	}

	return false;
}

/* ------------------------------------------------------------------------- */
/* HUD                                                                       */
/* ------------------------------------------------------------------------- */

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

	if (g_cfg.show_hud && emu_loaded())
		ui_draw_hud();
}

void ui_draw_hud(void)
{
	const emu_info *info = emu_get_info();

	scr_clear();
	scr_line(1, 1, "%.16s %s", info->title, info->cgb ? "CGB" : "DMG");
	scr_line(2, 1, "%.1f fps   %.0f ms/frame", g_perf.fps, g_perf.frame_ms);
	scr_line(3, 1, "core %.1f  blit %.1f", g_perf.core_ms, g_perf.blit_ms);
	scr_line(4, 1, "wait %.1f  sound %s", g_perf.wait_ms,
		 g_cfg.audio_on ? "on" : "off");
	scr_line(5, 1, "out %u ms   skip %s", audio_queue_ms(),
		 frameskip_label(g_cfg.frameskip));

	scr_line(7, 1, "pal %.24s", palette_name(palette_get()));
	scr_line(8, 1, "vid %s", scale_label(g_cfg.scale));

	scr_line(10, 1, "A/B Start/Select");
	scr_line(11, 1, "D-Pad or Circle Pad");
	scr_line(12, 1, "X turbo, Y palette");
	scr_line(13, 1, "Touch here for menu");

	scr_line(15, 1, "rom %u ram %u banks", info->rom_banks, info->ram_banks);
	scr_flush();
}

void ui_init(void)
{
	consoleInit(GFX_BOTTOM, NULL);
	scr_clear();
	scr_flush();
}

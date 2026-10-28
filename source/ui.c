/**
 * Bottom screen UI: ROM browser, in-game menu and status readout.
 *
 * libctru's text console gives 30 columns x 40 rows of 8x8 characters on the
 * bottom screen, so every layout below stays inside that box. The console owns
 * the bottom framebuffer (it turns double buffering off for that screen); the
 * emulator keeps the top screen to itself.
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
#define MAX_ENTRIES 400
#define LIST_ROWS   22

typedef struct
{
	char name[256];
	bool is_dir;
} dir_entry;

static dir_entry entries[MAX_ENTRIES];
static int       entry_count;
static char      cur_dir[512];

/* HUD statistics. */
static unsigned hud_frames;
static u64      hud_last_tick;
static float    hud_fps;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* Row/column addressing, 1-based like the VT sequences the console supports. */
static void at(int row, int col)
{
	printf("\x1b[%d;%dH", row, col);
}

static void line_at(int row, int col, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void line_at(int row, int col, const char *fmt, ...)
{
	char buf[UI_COLS + 1];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);

	at(row, col);
	fputs(buf, stdout);
}

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

static void scan_dir(const char *path)
{
	entry_count = 0;

	DIR *dir = opendir(path);
	if (dir == NULL)
		return;

	struct dirent *de;
	while ((de = readdir(dir)) != NULL && entry_count < MAX_ENTRIES)
	{
		if (de->d_name[0] == '.')
			continue;

		char full[768];
		snprintf(full, sizeof full, "%s%s%s", path,
			 (path[strlen(path) - 1] == '/') ? "" : "/", de->d_name);

		struct stat st;
		bool is_dir = (stat(full, &st) == 0) && S_ISDIR(st.st_mode);

		if (!is_dir && !has_rom_extension(de->d_name))
			continue;

		snprintf(entries[entry_count].name,
			 sizeof entries[entry_count].name, "%s", de->d_name);
		entries[entry_count].is_dir = is_dir;
		entry_count++;
	}
	closedir(dir);

	qsort(entries, entry_count, sizeof entries[0], compare_entries);
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

/* ------------------------------------------------------------------------- */
/* File browser                                                              */
/* ------------------------------------------------------------------------- */

bool ui_browse_rom(char *out, size_t out_len)
{
	if (cur_dir[0] == '\0')
		strcpy(cur_dir, "sdmc:/");

	int sel = 0;
	int first = 0;

	for (;;)
	{
		scan_dir(cur_dir);

		consoleClear();
		line_at(1, 1, "GBabe " GBABE_VERSION " - pick a ROM");
		line_at(2, 1, "%.29s", cur_dir);
		line_at(3, 1, "-----------------------------");

		if (entry_count == 0)
			line_at(6, 1, "(no ROMs here)");

		if (sel < first)
			first = sel;
		if (sel >= first + LIST_ROWS)
			first = sel - LIST_ROWS + 1;

		for (int i = 0; i < LIST_ROWS && first + i < entry_count; i++)
		{
			int idx = first + i;
			line_at(5 + i, 1, "%c%c%.26s",
				(idx == sel) ? '>' : ' ',
				entries[idx].is_dir ? '/' : ' ',
				entries[idx].name);
		}

		line_at(29, 1, "A:open   B:back");
		line_at(30, 1, "START:quit");

		u32 k = wait_key();
		if (k == 0 || (k & KEY_START))
			return false;

		if (k & (KEY_DDOWN | KEY_CPAD_DOWN))
			sel = (sel + 1 < entry_count) ? sel + 1 : sel;
		else if (k & (KEY_DUP | KEY_CPAD_UP))
			sel = (sel > 0) ? sel - 1 : 0;
		else if (k & (KEY_DRIGHT | KEY_CPAD_RIGHT))
			sel = (sel + LIST_ROWS < entry_count) ? sel + LIST_ROWS
							     : entry_count - 1;
		else if (k & (KEY_DLEFT | KEY_CPAD_LEFT))
			sel = (sel > LIST_ROWS) ? sel - LIST_ROWS : 0;
		else if (k & KEY_B)
		{
			if (!strcmp(cur_dir, "sdmc:/"))
				return false; /* B at the root cancels */

			parent_dir();
			sel = first = 0;
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
			}
			else
			{
				/* Leave cur_dir on the ROM's folder so the browser
				 * reopens there next time. */
				gbabe_copy(out, out_len, full);
				return true;
			}
		}
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
	consoleClear();
	line_at(1, 1, "GBabe " GBABE_VERSION " - paused");

	const char *rows[MENU_ROWS];
	char buf[MENU_ROWS][32];

	snprintf(buf[0], 32, "Resume");
	snprintf(buf[1], 32, "Palette  <%s>", palette_name(palette_get()));
	snprintf(buf[2], 32, "Video    <%s>", scale_label(g_cfg.scale));
	snprintf(buf[3], 32, "Skip     <%s>", frameskip_label(g_cfg.frameskip));
	snprintf(buf[4], 32, "Sound    <%s>", g_cfg.audio_on ? "on" : "off");
	snprintf(buf[5], 32, "Status   <%s>", g_cfg.show_hud ? "on" : "off");
	snprintf(buf[6], 32, "Reset game");
	snprintf(buf[7], 32, "Load another ROM");
	snprintf(buf[8], 32, "Save and quit");

	for (int i = 0; i < MENU_ROWS; i++)
		rows[i] = buf[i];

	for (int i = 0; i < MENU_ROWS; i++)
		line_at(ROW_RESUME + i, 1, "%c%.28s", (sel == i) ? '>' : ' ',
			rows[i]);

	const emu_info *info = emu_get_info();
	line_at(15, 1, "%.16s %s", info->title, info->cgb ? "CGB" : "DMG");
	line_at(16, 1, "%u Mbit  MBC%d", (unsigned)(info->rom_banks * 16 / 128),
		info->mbc);

	line_at(20, 1, "D-Pad      move");
	line_at(21, 1, "L/R        change");
	line_at(22, 1, "A/START    select");
	line_at(23, 1, "B          resume");
	line_at(25, 1, "In game: X turbo");
	line_at(26, 1, "         Y palette");
	line_at(27, 1, "Touch = this menu");
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
		else if (k & (KEY_DRIGHT | KEY_CPAD_RIGHT))
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
		else if (k & (KEY_DLEFT | KEY_CPAD_LEFT))
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
				consoleClear();
				return sel == 0;
			}

			if (sel == ROW_RESET - ROW_RESUME)
			{
				emu_reset();
				config_save();
				consoleClear();
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
						consoleClear();
						return true;
					}

					consoleClear();
					line_at(2, 1, "%.28s", err);
					line_at(5, 1, "Press any button.");
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
			consoleClear();
			return true;
		}
	}

	return false;
}

/* ------------------------------------------------------------------------- */
/* HUD                                                                       */
/* ------------------------------------------------------------------------- */

void ui_note_frame(void)
{
	hud_frames++;

	u64 now = svcGetSystemTick();
	if (hud_last_tick == 0)
	{
		hud_last_tick = now;
		return;
	}

	double elapsed = (double)(now - hud_last_tick) / SYSCLOCK_ARM11;
	if (elapsed < 1.0)
		return;

	hud_fps = (float)(hud_frames / elapsed);
	hud_frames = 0;
	hud_last_tick = now;

	if (g_cfg.show_hud && emu_loaded())
		ui_draw_hud();
}

void ui_draw_hud(void)
{
	const emu_info *info = emu_get_info();

	consoleClear();
	line_at(1, 1, "%.16s %s", info->title, info->cgb ? "CGB" : "DMG");
	line_at(2, 1, "%d.%d fps", (int)hud_fps, (int)(hud_fps * 10) % 10);
	line_at(3, 1, "sound %s  out %ums", g_cfg.audio_on ? "on" : "off",
		audio_queue_ms());
	line_at(4, 1, "skip  %s", frameskip_label(g_cfg.frameskip));

	line_at(6, 1, "pal %.18s", palette_name(palette_get()));
	line_at(7, 1, "vid %s", scale_label(g_cfg.scale));

	line_at(9, 1, "A/B Start/Select");
	line_at(10, 1, "D-Pad or Circle Pad");
	line_at(11, 1, "X turbo, Y palette");
	line_at(12, 1, "Touch here for menu");

	line_at(14, 1, "rom %u ram %u banks", info->rom_banks, info->ram_banks);
}

void ui_init(void)
{
	consoleInit(GFX_BOTTOM, NULL);
	consoleClear();
}

/** Menus and file browser drawn on the bottom screen. MIT. */
#pragma once

#include "gb3ds.h"

#include <stdbool.h>

void ui_init(void);

/* Blocking file browser. Returns true and fills `out` when a ROM was picked,
 * false when the user backed out. */
bool ui_browse_rom(char *out, size_t out_len);

/* Blocking in-game menu. Returns true to resume the game, false to quit the
 * application. */
bool ui_in_game_menu(void);

/* Redraw the bottom screen status lines (title, fps, palette, controls). */
void ui_draw_hud(void);

/* Per-frame bookkeeping for the HUD: account the ticks spent emulating,
 * presenting and waiting, and refresh the profile (and the HUD) once a second.
 * A tick is 1/SYSCLOCK_ARM11 s. */
void ui_note_frame(uint64_t core_ticks, uint64_t blit_ticks,
		   uint64_t wait_ticks, uint64_t frame_ticks);

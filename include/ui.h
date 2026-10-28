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

/* Frame counter bookkeeping for the HUD. */
void ui_note_frame(void);

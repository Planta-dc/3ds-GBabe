/**
 * Menus and file browser drawn on the bottom screen.
 *
 * Everything is drawn into a 320x240 RGB565 canvas by ui_gfx.c and presented
 * with a single framebuffer write per redraw - see the comment at the top of
 * ui.c for why that replaced libctru's text console.
 *
 * MIT licensed.
 */
#pragma once

#include "gb3ds.h"

#include <stdbool.h>

void ui_init(void);

/* Blocking message (load errors and the like). Returns once the user
 * acknowledges it, or immediately if HOME was pressed. */
void ui_message(const char *title, const char *line1, const char *line2);

/* Blank the bottom screen (used when the status readout is switched off, so a
 * menu does not linger behind the game). */
void ui_blank(void);

/* Blocking file browser. Returns true and fills `out` when a ROM was picked,
 * false when the user backed out. */
bool ui_browse_rom(char *out, size_t out_len);

/* Blocking in-game menu. Returns true to resume the game, false to quit the
 * application. */
bool ui_in_game_menu(void);

/* Per-frame bookkeeping for the HUD: account the ticks spent emulating,
 * blitting, doing the once-a-second work and waiting for the panel, and refresh
 * the profile once a second. A tick is 1/SYSCLOCK_ARM11 s.
 *
 * The split it is given (see gbabe_frame_cost) stops where blame starts:
 * `core` for the whole of emu_run_frame(), then `emu`/`audio`/`draw` for the
 * parts of it worth telling apart, `blit` for the flush and swap, `post` for
 * ui_idle_work() - work that costs nothing when it happens before the wait for
 * the vblank, and a dropped frame when it happens after it - and `wait` for
 * time asleep. `late`/`resyncs` belong to the caller, which resets them. */
void ui_note_frame(const gbabe_frame_cost *cost);

/* Write the whole session as one line to the debug log - frames, average and
 * worst whole-second frame rate, average and worst frame time, average core
 * time, average CGB palette conversions, and how many frames started late.
 * Called when a game is unloaded and on quit; it resets the totals, so each
 * session gets its own line. This is what a "how did it run?" report should
 * quote. */
void ui_log_session(void);

/* Do the once-a-second work that is too expensive for a frame boundary: redraw
 * the bottom-screen status readout if it is due, and spend one band of its
 * present. Call it in the idle time between frames, where the loop would
 * otherwise be asleep - it is ~16 ms of work in 8 slices of ~2 ms, and in one
 * go it costs a whole frame (docs/PORT-NOTES.md §13). */
void ui_idle_work(void);

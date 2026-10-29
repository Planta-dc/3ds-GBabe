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

/* Redraw the bottom screen status lines (title, fps, palette, controls). */
void ui_draw_hud(void);

/* Per-frame bookkeeping for the HUD: account the ticks spent emulating,
 * blitting, doing the once-a-second work and waiting for the panel, and refresh
 * the profile once a second. A tick is 1/SYSCLOCK_ARM11 s.
 *
 * post_ticks is the time spent in ui_idle_work() - work that costs nothing when
 * it happens before the wait for the vblank, and a dropped frame when it
 * happens after it.
 *
 * late_frames is how many frames in this window started after their own slot
 * (the previous frame overran), resyncs how many times more than four frames of
 * backlog were thrown away. Both are owed to the caller, which resets them. */
void ui_note_frame(uint64_t core_ticks, uint64_t blit_ticks,
		   uint64_t sync_ticks, uint64_t post_ticks,
		   uint64_t wait_ticks, uint64_t frame_ticks,
		   unsigned late_frames, unsigned resyncs);

/* Do the once-a-second work that is too expensive for a frame boundary: redraw
 * the bottom-screen status readout if it is due. Call it between
 * video_end_frame() and video_present_sync(), where the wait for the panel
 * absorbs it - after that wait there is no time left and it costs a frame. */
void ui_idle_work(void);

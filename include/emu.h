/** Emulator glue: ROM loading, save RAM, run loop hooks. MIT. */
#pragma once

#include "gb3ds.h"

#include <stdbool.h>
#include <stdint.h>

/* Included from C++ as well (the Gambatte backend is C++). */
#ifdef __cplusplus
extern "C" {
#endif

/* Build-time info about the loaded cartridge, for the HUD. */
typedef struct
{
	char title[17];
	bool cgb;
	int  mbc;
	unsigned rom_banks;
	unsigned ram_banks;
	uint8_t  auto_hash; /* gb_colour_hash(), drives the "Auto" palette */
} emu_info;

/* Load a ROM from an SD path. Returns nullptr on success, otherwise a
 * human readable error string. */
const char *emu_load(const char *path);

/* Free ROM/save RAM and reset the front-end state. */
void emu_unload(void);

bool emu_loaded(void);
const emu_info *emu_get_info(void);

/* Set the currently pressed buttons (bitmask of enum gb_button). */
void emu_set_joypad(uint8_t pressed);

/* Re-run the power-on sequence without reloading the ROM. */
void emu_reset(void);

/* Run exactly one Game Boy frame (~70224 cycles). Rendering happens while
 * rendering is enabled; disabling it implements frame skip without leaving
 * half-updated buffers behind. The finished 160x144 RGB565 picture is left in
 * emu_frame_buffer(); *presenting* it is the caller's job, and deliberately so:
 * the scaled transpose into the panel is the last thing a frame does, so that
 * video_begin_frame() can hand back a framebuffer the panel has really finished
 * with instead of waiting for one (docs/PORT-NOTES.md section 17). */
void emu_run_frame(void);
void emu_set_rendering(bool on);

/* The 160x144 RGB565 picture from the last emu_run_frame(), `pitch` pixels per
 * line, or NULL when the last frame ran with rendering disabled. Owned by the
 * backend; valid until the next emu_run_frame(). */
const uint16_t *emu_frame_buffer(void);

/* How emu_run_frame() spent its time since the last call, in ARM11 ticks, in
 * three parts: emulation (Gambatte's CPU+PPU+APU), the audio decimation and
 * resampling, and handing the result to CSND (a FIFO copy plus, every other
 * frame, a service call). They have nothing in common except that they all
 * happen inside emu_run_frame(), which is why `core` used to hide all three.
 * Zero when nothing is loaded; any pointer may be NULL. */
void emu_timing_take(uint64_t *emu_ticks, uint64_t *audio_ticks,
		     uint64_t *push_ticks);

/* The DMG palette selection changed: re-apply the colours to the core. It is
 * handed the colour values directly, so this has to be pushed rather than
 * looked up per pixel. Safe to call when nothing is loaded. */
void emu_palette_changed(void);

/* Write cartridge RAM (if any) back to the .sav file. Safe to call often. */
void emu_flush_save(void);

/* The last error the emulator core reported (bad opcode, unmapped read, ...),
 * or NULL when there is nothing new. Reading it clears it: the front-end polls
 * this after each frame and shows the message to the user, because the core's
 * callback happens deep inside a frame and must not block there. */
const char *emu_last_error(void);

#ifdef __cplusplus
}
#endif

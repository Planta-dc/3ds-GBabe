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

/* Run exactly one Game Boy frame (~70224 cycles). Rendering to the top screen
 * only happens while rendering is enabled; disabling it implements frame skip
 * without leaving half-updated buffers behind. */
void emu_run_frame(void);
void emu_set_rendering(bool on);

/* How emu_run_frame() spent its time since the last call, in ARM11 ticks:
 * emulation (Gambatte's CPU+PPU+APU) and the audio decimator, separately.
 * Zero when nothing is loaded. */
void emu_timing_take(uint64_t *emu_ticks, uint64_t *audio_ticks);

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

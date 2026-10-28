/** Emulator glue: ROM loading, save RAM, run loop hooks. MIT. */
#pragma once

#include "gb3ds.h"

#include <stdbool.h>
#include <stdint.h>

/* 160x144 RGB565 framebuffer, filled by the core's lcd_draw_line callback. */
extern uint16_t gb_fb[144][160];

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

/* Write cartridge RAM (if any) back to the .sav file. Safe to call often. */
void emu_flush_save(void);

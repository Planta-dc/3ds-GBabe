/**
 * Walnut-CGB glue for the 3DS front-end.
 *
 * The core is a single-header library, so the whole emulator is compiled into
 * this translation unit. Only this file (plus audio.c for the APU) may include
 * walnut_cgb.h, otherwise every symbol would be duplicated.
 *
 * MIT licensed.
 */
#include "gb3ds.h"

#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "emu.h"
#include "video.h"
#include "audio.h"
#include "log.h"
#include "palette.h"

/* The core wants these two before it is included. */
#ifndef ENABLE_SOUND
#define ENABLE_SOUND 1
#endif
#ifndef ENABLE_LCD
#define ENABLE_LCD 1
#endif

#include "walnut_cgb.h"

struct gb_s gb; /* ~50 KiB of state; kept off the stack and out of .bss churn */

static uint8_t *rom;
static size_t   rom_size;
static uint8_t *bootrom;   /* optional, sdmc:/3ds/gbabe/bootrom */
static uint8_t *cart_ram;
static size_t   cart_ram_size;
static uint32_t cart_ram_mask; /* cart_ram_size - 1; every GB RAM size is 2^n */
static char     save_path[512];
static bool     sram_dirty;
static bool     loaded;
static emu_info info;

/* ------------------------------------------------------------------------- */
/* Memory card callbacks                                                     */
/* ------------------------------------------------------------------------- */

/* ROM and save RAM are straight memory copies, so all the core callbacks are
 * one-liners. The 16/32-bit variants must cope with unaligned addresses
 * because the Game Boy happily reads across alignment boundaries. */
static uint8_t rom_byte(struct gb_s *g, const uint_fast32_t addr)
{
	(void)g;
	return (addr < rom_size) ? rom[addr] : 0xFF;
}

static uint16_t rom_word(struct gb_s *g, const uint_fast32_t addr)
{
	(void)g;
	uint16_t v = 0xFFFF;
	if (addr + 1 < rom_size)
		memcpy(&v, &rom[addr], sizeof v);
	return v;
}

static uint32_t rom_dword(struct gb_s *g, const uint_fast32_t addr)
{
	(void)g;
	uint32_t v = 0xFFFFFFFF;
	if (addr + 3 < rom_size)
		memcpy(&v, &rom[addr], sizeof v);
	return v;
}

/* Cartridge RAM is a power of two bytes (2K..128K), so wrap with a mask: a
 * modulo here would compile to a software divide in the hot read callback. */
static uint8_t cart_ram_byte(struct gb_s *g, const uint_fast32_t addr)
{
	(void)g;
	if (cart_ram == NULL)
		return 0xFF;
	return cart_ram[addr & cart_ram_mask];
}

static void cart_ram_write_byte(struct gb_s *g, const uint_fast32_t addr,
				const uint8_t val)
{
	(void)g;
	if (cart_ram == NULL)
		return;
	cart_ram[addr & cart_ram_mask] = val;
	sram_dirty = true;
}

static uint8_t bootrom_byte(struct gb_s *g, const uint_fast16_t addr)
{
	(void)g;
	/* 0xFF keeps the core's built-in post-boot state for anything the
	 * supplied boot ROM does not cover. */
	if (bootrom == NULL)
		return 0xFF;
	return (addr < 0x900) ? bootrom[addr] : 0xFF;
}

/* Both boot ROMs are optional and not distributed here; drop a legally
 * obtained dmg_boot.bin / cgb_boot.bin in roms/bootrom (see README) and the
 * games get the real boot animation plus correct initial hardware state. */
static void try_load_bootrom(bool cgb)
{
	static const char *const names[2] =
	{
		GBABE_ROOT "/bootrom/dmg_boot.bin",
		GBABE_ROOT "/bootrom/cgb_boot.bin",
	};

	const char *path = names[cgb ? 1 : 0];
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return;

	uint8_t buf[0x900];
	size_t n = fread(buf, 1, sizeof buf, f);
	fclose(f);

	if (n < 0x100)
		return;

	free(bootrom);
	bootrom = malloc(sizeof buf);
	if (bootrom == NULL)
		return;

	memset(bootrom, 0xFF, sizeof buf);
	memcpy(bootrom, buf, n);

	gb_set_bootrom(&gb, bootrom_byte);
	gb_reset(&gb);
}

/* The core reports errors from inside a frame, so nothing here may block or
 * draw: the message is stored and the main loop picks it up afterwards. */
static char gb_error_text[64];
static bool gb_error_pending;

const char *emu_last_error(void)
{
	if (!gb_error_pending)
		return NULL;

	gb_error_pending = false;
	return gb_error_text;
}

static void gb_error_cb(struct gb_s *g, const enum gb_error_e err,
			const uint16_t addr)
{
	(void)g;
	if (err == GB_UNKNOWN_ERROR)
		return;

	snprintf(gb_error_text, sizeof gb_error_text,
		 "Core error %d at 0x%04X", (int)err, (unsigned)addr);
	gb_error_pending = true;

	/* The log gets it even if the user dismisses the message on screen. */
	log_line("core: %s", gb_error_text);
}

/* ------------------------------------------------------------------------- */
/* Frame rendering                                                           */
/* ------------------------------------------------------------------------- */

static void lcd_draw_line_cb(struct gb_s *g, const uint8_t *pixels,
			     const uint_fast8_t line)
{
	uint16_t out[LCD_WIDTH];

	if (g->cgb.cgbMode)
	{
		/* CGB: pixel is already a 6-bit index into the fixed palette,
		 * which the core pre-converted to native RGB565. */
		for (unsigned x = 0; x < LCD_WIDTH; x++)
			out[x] = g->cgb.fixPalette[pixels[x]];
	}
	else
	{
		/* DMG: low two bits are the shade, bits 4-5 are the layer, which
		 * gives the classic 3 x 4 (BG / OBJ0 / OBJ1) colouring. */
		const uint16_t *pal = palette_dmg565();
		for (unsigned x = 0; x < LCD_WIDTH; x++)
			out[x] = pal[((pixels[x] & 18) >> 1) | (pixels[x] & 3)];
	}

	video_draw_line(out, line);
}

/* ------------------------------------------------------------------------- */
/* Save files                                                                */
/* ------------------------------------------------------------------------- */

static void make_save_path(const char *rom_path)
{
	size_t n = strlen(rom_path);
	size_t keep = n;

	if (keep > sizeof save_path - 5)
		keep = sizeof save_path - 5;
	memcpy(save_path, rom_path, keep);
	save_path[keep] = '\0';

	char *dot = strrchr(save_path, '.');
	if (dot != NULL && dot != save_path)
		*dot = '\0';
	strcat(save_path, ".sav");
}

static void load_save(void)
{
	if (cart_ram_size == 0)
		return;

	FILE *f = fopen(save_path, "rb");
	if (f == NULL)
		return;

	size_t n = fread(cart_ram, 1, cart_ram_size, f);
	(void)n;
	fclose(f);
	sram_dirty = false;
}

void emu_flush_save(void)
{
	if (!loaded || cart_ram == NULL || cart_ram_size == 0 || !sram_dirty)
		return;

	FILE *f = fopen(save_path, "wb");
	if (f == NULL)
		return;

	if (fwrite(cart_ram, 1, cart_ram_size, f) == cart_ram_size)
		sram_dirty = false;
	fclose(f);
}

/* ------------------------------------------------------------------------- */
/* Loading / running                                                         */
/* ------------------------------------------------------------------------- */

void emu_unload(void)
{
	if (loaded)
		emu_flush_save();

	if (rom != NULL)
	{
		free(rom);
		rom = NULL;
	}
	if (bootrom != NULL)
	{
		free(bootrom);
		bootrom = NULL;
	}
	if (cart_ram != NULL)
	{
		free(cart_ram);
		cart_ram = NULL;
	}
	rom_size = 0;
	cart_ram_size = 0;
	cart_ram_mask = 0;
	loaded = false;
	memset(&info, 0, sizeof info);
}

const char *emu_load(const char *path)
{
	gb_error_pending = false;
	emu_unload();

	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return "Cannot open ROM.";

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size < 0x150)
	{
		fclose(f);
		return "File is too small to be a ROM.";
	}

	/* ROMs live in the app heap; the core only does byte/word loads from
	 * them, so plain memory is fine (no DMA involved). */
	rom_size = (size_t)size;
	rom = malloc(rom_size);
	if (rom == NULL)
	{
		fclose(f);
		return "Not enough memory for the ROM.";
	}

	if (fread(rom, 1, rom_size, f) != rom_size)
	{
		fclose(f);
		emu_unload();
		return "Short read while loading the ROM.";
	}
	fclose(f);

	enum gb_init_error_e err = gb_init(&gb, rom_byte, rom_word, rom_dword,
					   cart_ram_byte, cart_ram_write_byte,
					   gb_error_cb, NULL);
	switch (err)
	{
	case GB_INIT_NO_ERROR:
		break;
	case GB_INIT_CARTRIDGE_UNSUPPORTED:
		emu_unload();
		return "Unsupported cartridge (MBC not implemented).";
	case GB_INIT_INVALID_CHECKSUM:
		emu_unload();
		return "Invalid ROM: header checksum mismatch.";
	default:
		emu_unload();
		return "Unknown error while initialising the core.";
	}

	/* Cartridge RAM. */
	cart_ram_size = 0;
	if (gb_get_save_size_s(&gb, &cart_ram_size) < 0)
		cart_ram_size = 0;

	if (cart_ram_size > 0)
	{
		cart_ram = calloc(1, cart_ram_size);
		if (cart_ram == NULL)
		{
			cart_ram_size = 0;
			emu_unload();
			return "Not enough memory for the save RAM.";
		}
		/* 0 when the size is not a power of two, which gives the plain
		 * truncating behaviour of the modulo it replaces. */
		cart_ram_mask = (uint32_t)cart_ram_size - 1;
	}

	make_save_path(path);
	load_save();

	/* RTC: Pokémon Gold/Silver and friends read this at boot. */
	{
		time_t now = time(NULL);
		struct tm *tmv = localtime(&now);
		if (tmv != NULL)
			gb_set_rtc(&gb, tmv);
	}

	gb_get_rom_name(&gb, info.title);
	info.cgb = gb.cgb.cgbMode != 0;
	info.mbc = gb.mbc;
	info.rom_banks = (unsigned)(gb.num_rom_banks_mask + 1);
	info.ram_banks = gb.num_ram_banks;

	info.auto_hash = gb_colour_hash(&gb);
	palette_set_auto_hash(info.auto_hash);

	/* Optional boot ROM; re-runs the power-on sequence when present. */
	try_load_bootrom(info.cgb);

	gb_init_lcd(&gb, lcd_draw_line_cb);

	loaded = true;
	sram_dirty = false;
	return NULL;
}

bool emu_loaded(void)
{
	return loaded;
}

const emu_info *emu_get_info(void)
{
	return &info;
}

void emu_set_joypad(uint8_t pressed)
{
	/* The core stores the pad active-low: a cleared bit means pressed. */
	gb.direct.joypad = (uint8_t)~pressed;
}

void emu_reset(void)
{
	if (!loaded)
		return;
	emu_flush_save();
	gb_error_pending = false; /* a stale error is not this run's problem */
	gb_reset(&gb);
	gb_init_lcd(&gb, lcd_draw_line_cb);
}

void emu_palette_changed(void)
{
	/* The line callback reads palette_dmg565() every line, so a new choice is
	 * picked up on the next frame with nothing to do here. */
}

void emu_set_rendering(bool on)
{
	gb.display.lcd_draw_line = on ? lcd_draw_line_cb : NULL;
}

void emu_run_frame(void)
{
	if (!loaded)
		return;

	gb_run_frame_dualfetch(&gb);
	audio_frame();
}

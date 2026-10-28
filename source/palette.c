/**
 * Colour palettes for monochrome Game Boy games.
 *
 * Each entry holds three groups of four colours: object palette 0, object
 * palette 1 and the background palette. The core reports both the shade and the
 * palette of every pixel, so DMG games get the same three-layer colouring the
 * Game Boy Color applies to them.
 *
 * MIT licensed.
 */
#include "palette.h"

#include <string.h>

#define PAL_COLOURS 12

typedef struct
{
	const char *name;
	uint32_t rgb[3][4]; /* 0xRRGGBB, OBJ0 / OBJ1 / BG, 4 shades each */
} palette_def;

/* Shade order is lightest (0) to darkest (3).
 * Row order is OBJ0 (sprite palette 0), OBJ1, then BG, matching the palette
 * index the core reports for DMG pixels. */
static const palette_def palettes[] =
{
	/* 0: auto - resolved from the cartridge header at load time */
	{ "Auto (per game)",
	  { { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } } },

	{ "GB Green",
	  { { 0x9BBC0F, 0x8BAC0F, 0x306230, 0x0F380F },
	    { 0x9BBC0F, 0x8BAC0F, 0x306230, 0x0F380F },
	    { 0x9BBC0F, 0x8BAC0F, 0x306230, 0x0F380F } } },

	{ "GB Pocket",
	  { { 0xC4CFA1, 0x8C926B, 0x4B5132, 0x181818 },
	    { 0xC4CFA1, 0x8C926B, 0x4B5132, 0x181818 },
	    { 0xC4CFA1, 0x8C926B, 0x4B5132, 0x181818 } } },

	{ "GBC Grape",
	  { { 0xFFFFFF, 0xFFFF00, 0xFF0000, 0x000000 },
	    { 0xFFFFFF, 0xFFFF00, 0xFF0000, 0x000000 },
	    { 0xFFFFFF, 0xFFFF00, 0xFF0000, 0x000000 } } },

	{ "GBC Blue",
	  { { 0xFFFFFF, 0x63A5FF, 0x0000FF, 0x000000 },
	    { 0xFFFFFF, 0x7BFF31, 0x008400, 0x000000 },
	    { 0xFFFFFF, 0xFFFF00, 0x7B4A00, 0x000000 } } },

	{ "Amber CRT",
	  { { 0xFFD8A0, 0xE8A060, 0x905020, 0x201000 },
	    { 0xFFD8A0, 0xE8A060, 0x905020, 0x201000 },
	    { 0xFFD8A0, 0xE8A060, 0x905020, 0x201000 } } },

	{ "Teal LCD",
	  { { 0x9BFFF1, 0x5CC0B0, 0x2A7A70, 0x0E2A28 },
	    { 0x9BFFF1, 0x5CC0B0, 0x2A7A70, 0x0E2A28 },
	    { 0x9BFFF1, 0x5CC0B0, 0x2A7A70, 0x0E2A28 } } },

	{ "Mono",
	  { { 0xFFFFFF, 0xB0B0B0, 0x585858, 0x000000 },
	    { 0xFFFFFF, 0xB0B0B0, 0x585858, 0x000000 },
	    { 0xFFFFFF, 0xB0B0B0, 0x585858, 0x000000 } } },
};

#define PALETTE_COUNT ((int)(sizeof palettes / sizeof palettes[0]))

/* Palette used for "Auto" when the game is not in the table below. */
#define AUTO_DEFAULT 1 /* GB Green */

/* Cartridge header hashes (gb_colour_hash()) and the palette the Game Boy Color
 * picks for them. A small sample of the well known ones; extend freely. */
static const struct
{
	uint8_t hash;
	uint8_t palette;
} auto_table[] =
{
	{ 0x15, 3 }, /* Pokemon Yellow */
	{ 0xDB, 3 }, /* Tetris */
	{ 0x95, 3 },
	{ 0x61, 4 }, /* Pokemon Blue */
	{ 0x45, 4 },
	{ 0x71, 1 }, /* Balloon Kid */
	{ 0xFF, 1 }, /* Tetris Blast */
	{ 0x19, 5 }, /* Donkey Kong */
};

static uint16_t lut[PALETTE_COUNT][PAL_COLOURS];
static uint16_t auto_lut[PAL_COLOURS];
static int current = 0;
static int auto_pick = AUTO_DEFAULT;

static uint16_t rgb888_to_565(uint32_t c)
{
	uint16_t r = (uint16_t)((c >> 19) & 0x1F);
	uint16_t g = (uint16_t)((c >> 10) & 0x3F);
	uint16_t b = (uint16_t)((c >> 3) & 0x1F);
	return (uint16_t)((r << 11) | (g << 5) | b);
}

static void build_lut(uint16_t *dst, const uint32_t src[3][4])
{
	for (int row = 0; row < 3; row++)
		for (int shade = 0; shade < 4; shade++)
			dst[row * 4 + shade] = rgb888_to_565(src[row][shade]);
}

void palette_init(void)
{
	for (int i = 0; i < PALETTE_COUNT; i++)
		build_lut(lut[i], palettes[i].rgb);

	palette_set_auto_hash(0xFF);
}

void palette_set_auto_hash(uint8_t h)
{
	int pick = AUTO_DEFAULT;

	for (unsigned i = 0; i < sizeof auto_table / sizeof auto_table[0]; i++)
	{
		if (auto_table[i].hash == h)
		{
			pick = auto_table[i].palette;
			break;
		}
	}

	auto_pick = pick;
	build_lut(auto_lut, palettes[pick].rgb);
}

const uint16_t *palette_dmg565(void)
{
	if (current == 0)
		return auto_lut;
	return lut[current];
}

int palette_count(void)
{
	return PALETTE_COUNT;
}

int palette_get(void)
{
	return current;
}

void palette_set(int index)
{
	if (index < 0 || index >= PALETTE_COUNT)
		index = 0;
	current = index;
}

const char *palette_name(int index)
{
	if (index == 0)
		return palettes[auto_pick].name; /* what "Auto" resolved to */
	if (index < 0 || index >= PALETTE_COUNT)
		index = 0;
	return palettes[index].name;
}

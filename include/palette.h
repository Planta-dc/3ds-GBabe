/** DMG (original Game Boy) colour palettes. MIT. */
#pragma once

#include "gb3ds.h"

#include <stdint.h>

/* Included from C++ as well (the Gambatte backend is C++). */
#ifdef __cplusplus
extern "C" {
#endif

/* Build the RGB565 lookup tables. Call once at start-up. */
void palette_init(void);

/* 12 RGB565 entries, ordered OBJ0 shades 0-3, OBJ1 shades 0-3, BG shades 0-3,
 * which is the index layout the core documents for DMG pixels. */
const uint16_t *palette_dmg565(void);

int  palette_count(void);
int  palette_get(void);
void palette_set(int index);
const char *palette_name(int index);

/* "Auto" (index 0) follows the cartridge header hash, the same way the Game Boy
 * Color colourises monochrome games. */
void palette_set_auto_hash(uint8_t hash);

#ifdef __cplusplus
}
#endif

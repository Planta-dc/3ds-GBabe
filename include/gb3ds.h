/**
 * gbabe - a Game Boy / Game Boy Color emulator for the Nintendo 3DS.
 *
 * Shared declarations for the platform layer. This header must be included
 * *before* walnut_cgb.h (emu.c does that) because the core needs
 * `struct gb_s` and the audio hooks declared up front.
 *
 * MIT licensed. See LICENSE.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define GBABE_VERSION       "0.1.0"
#define GBABE_APP_TITLE     "GBabe"
#define GBABE_ROOT          "sdmc:/3ds/gbabe"
#define GBABE_CONFIG_PATH   GBABE_ROOT "/config.cfg"
#define GBABE_SAVE_DIR      GBABE_ROOT "/saves"
#define GBABE_STATE_DIR     GBABE_ROOT "/states"
#define GBABE_LAST_ROM_PATH GBABE_ROOT "/last-rom"

/* ------------------------------------------------------------------------- */
/* Screen / scaling                                                          */
/* ------------------------------------------------------------------------- */

/* The 3DS top screen in landscape: 400x240. libctru reports the framebuffer as
 * 240x400 because the LCD is physically rotated. */
#define LCD_TOP_W       400
#define LCD_TOP_H       240
#define FB_STRIDE       240   /* framebuffer "width" as returned by libctru */

enum video_scale
{
	SCALE_FIT = 0,  /* full height, keeps the Game Boy aspect ratio (default) */
	SCALE_WIDE,     /* stretch to 400x240 */
	SCALE_ORIG,     /* 160x144, pixel perfect */
	SCALE_COUNT
};

/* ------------------------------------------------------------------------- */
/* Runtime profile, refreshed once a second by the HUD                       */
/* ------------------------------------------------------------------------- */

typedef struct
{
	float fps;      /* presented frames per second                          */
	float frame_ms; /* wall time per presented frame (all of it)            */
	float core_ms;  /* emulation: CPU + PPU + APU                           */
	float blit_ms;  /* scaled copy into the framebuffer + present           */
	float wait_ms;  /* paced off waiting for the next Game Boy frame        */
} gbabe_perf;

extern gbabe_perf g_perf;

/* ------------------------------------------------------------------------- */

typedef struct
{
	int  scale;        /* enum video_scale */
	int  palette;      /* index into the palette table, 0 = auto */
	int  frameskip;    /* 0 = off (default), 1 = skip when running late */
	bool audio_on;
	bool show_hud;     /* draw a status line on the bottom screen */
	bool fast_forward; /* session only, not persisted */
	char rom_path[512];
} gbabe_config;

extern gbabe_config g_cfg;
extern struct gb_s   gb;                    /* ~50 KiB, not on the stack */

/* Bounded string copy that always terminates; returns dst. */
static inline char *gbabe_copy(char *dst, size_t dst_len, const char *src)
{
	size_t n;

	if (dst_len == 0)
		return dst;
	n = strlen(src);
	if (n >= dst_len)
		n = dst_len - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
	return dst;
}

/* config.c */
void config_defaults(void);
void config_load(void);
void config_save(void);

/* Game Boy buttons. Bit values match the core's JOYPAD_* defines (and the
 * order the hardware shifts them out), so the mapping is a copy. */
enum gb_button
{
	GB_BTN_A      = 0x01,
	GB_BTN_B      = 0x02,
	GB_BTN_SELECT = 0x04,
	GB_BTN_START  = 0x08,
	GB_BTN_RIGHT  = 0x10,
	GB_BTN_LEFT   = 0x20,
	GB_BTN_UP     = 0x40,
	GB_BTN_DOWN   = 0x80,
};

/* Forward declarations the Walnut-CGB core expects from the front-end. */
struct gb_s;
uint8_t audio_read(uint16_t addr);
void    audio_write(uint16_t addr, uint8_t val);

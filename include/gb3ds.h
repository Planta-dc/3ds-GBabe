/**
 * gbabe - a Game Boy / Game Boy Color emulator for the Nintendo 3DS.
 *
 * Shared declarations for the platform layer.
 *
 * The emulation core is Gambatte 0.5.0 (third_party/gambatte, backend
 * source/emu_gambatte.cpp), behind include/emu.h. It used to be one of two -
 * the other was Walnut-CGB - and the second one was removed because on a
 * 268 MHz ARM11 the core is what decides whether a game runs at full speed.
 *
 * MIT licensed. See LICENSE.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define GBABE_VERSION       "0.2.0"
#define GBABE_APP_TITLE     "GBabe"
#define GBABE_ROOT          "sdmc:/3ds/gbabe"
#define GBABE_CONFIG_PATH   GBABE_ROOT "/config.cfg"
#define GBABE_SAVE_DIR      GBABE_ROOT "/saves"
#define GBABE_STATE_DIR     GBABE_ROOT "/states"
#define GBABE_LAST_ROM_PATH GBABE_ROOT "/last-rom"
#define GBABE_LOG_PATH      GBABE_ROOT "/debug.log"

/* ------------------------------------------------------------------------- */
/* Screen / scaling                                                          */
/* ------------------------------------------------------------------------- */

/* The 3DS top screen in landscape: 400x240. libctru reports the framebuffer as
 * 240x400 because the LCD is physically rotated. */
#define LCD_TOP_W       400
#define LCD_TOP_H       240
#define FB_STRIDE       240   /* framebuffer "width" as returned by libctru */

/* The Game Boy's frame rate is fixed and does not divide the panel's 60 Hz, so
 * the frame loop paces itself off this and not off vblank. (SYSCLOCK_ARM11
 * comes from libctru's 3ds.h; this is only ever expanded where that is
 * included.) */
#define GBABE_FRAME_HZ    59.7275
#define GBABE_FRAME_TICKS ((uint64_t)(SYSCLOCK_ARM11 / GBABE_FRAME_HZ))

/* Autosave cartridge RAM every 30 seconds of emulated time. The cast is not
 * decoration: SYSCLOCK_ARM11 is an integer constant in libctru, 30 x it is
 * 8.04e9, and that does not fit in 32 bits - the product wrapped to 3.75e9 and
 * the console wrote the save file every 14 seconds instead of every 30. */
#define GBABE_AUTOSAVE_SECONDS 30u
#define GBABE_AUTOSAVE_TICKS ((uint64_t)SYSCLOCK_ARM11 * GBABE_AUTOSAVE_SECONDS)

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
	float worst_ms; /* the slowest single frame in the last second           */
	float core_ms;  /* emulation: CPU + PPU + APU                           */
	float blit_ms;  /* scaled copy into the framebuffer                     */
	float post_ms;  /* once-a-second work done before that wait (free)      */
	float sync_ms;  /* waiting for the panel to take the finished frame     */
	float wait_ms;  /* paced off waiting for the next Game Boy frame        */
} gbabe_perf;

extern gbabe_perf g_perf;

/* ------------------------------------------------------------------------- */

typedef struct
{
	int  scale;        /* SCALE_* (see the enum video_scale below) */
	int  palette;      /* index into the palette table, 0 = auto */
	int  frameskip;    /* 0 = off (default), 1 = skip when running late */
	bool audio_on;
	bool show_hud;     /* draw a status line on the bottom screen */
	bool debug_log;    /* write sdmc:/3ds/gbabe/debug.log       */
	bool fast_forward; /* session only, not persisted */
	char rom_path[512];
} gbabe_config;

extern gbabe_config g_cfg;

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

/* Game Boy buttons. These are the bit values Gambatte's InputGetter expects. */
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


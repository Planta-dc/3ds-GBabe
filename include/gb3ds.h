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

#define GBABE_VERSION       "0.3.5"

/* Which build this is, stamped in by the Makefile (date + git revision).
 *
 * The version alone cannot tell two builds apart: one version number covers
 * however many fixes land while it is current, and once the .cia is on a card
 * there is no other way to know which one is installed. The debug log prints
 * it, so a log from the console identifies its own binary. Host tools, which
 * are built without the Makefile, get "dev". */
#ifndef GBABE_BUILD
#define GBABE_BUILD         "dev"
#endif
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
	float core_ms;  /* emu_run_frame()'s work: emu + audio + push + draw    */
	float emu_ms;   /* Gambatte's CPU + PPU + APU, nothing else             */
	float audio_ms; /* the 2 MHz -> 32 kHz decimator and the rate matcher   */
	float push_ms;  /* the FIFO copy and the csndPlaySound() behind it      */
	unsigned palette; /* CGB palette conversions per frame                  */
	float draw_ms;  /* scaled transpose into the framebuffer (64k writes)   */
	float blit_ms;  /* cache flush + buffer swap                            */
	float post_ms;  /* once-a-second work done before that wait (free)      */
	float sync_ms;  /* waiting for the panel to take the finished frame     */
	float wait_ms;  /* paced off waiting for the next Game Boy frame        */
} gbabe_perf;

extern gbabe_perf g_perf;

/* One frame's cost, in ARM11 ticks, split far enough to blame one part.
 *
 *   core  = emu + audio + draw + whatever emu_run_frame() spent elsewhere
 *   frame = core + blit + post (+ the idle wait, which `wait` reports)
 *
 * They are passed as a struct rather than as nine arguments because the
 * interesting question is always which of them is the big one, and a call with
 * nine unnamed uint64_ts cannot be read at the call site. */
typedef struct
{
	uint64_t core;   /* the work: emu + audio + push + draw                  */
	uint64_t emu;    /*   of which Gambatte                                 */
	uint64_t audio;  /*   of which the decimator                            */
	uint64_t push;   /*   of which handing the samples to CSND              */
	unsigned palette;/* CGB palette conversions (a Game Boy Color cost)     */
	uint64_t draw;   /*   of which the scaled transpose                     */
	uint64_t blit;   /* video_end_frame(): cache flush + swap               */
	uint64_t sync;   /* waiting for the panel to take the swap              */
	uint64_t post;   /* ui_idle_work(): once-a-second work, before the wait */
	uint64_t wait;   /* slept, waiting for the next frame to be due         */
	uint64_t frame;  /* the whole frame, start to start                     */
	unsigned late;   /* frames that started after their slot                */
	unsigned resyncs;/* backlogs thrown away                                */
} gbabe_frame_cost;

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


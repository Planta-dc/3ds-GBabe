/**
 * GBabe - Game Boy / Game Boy Color for the Nintendo 3DS.
 *
 * Main loop: services, input mapping, frame pacing.
 *
 * Frame pacing: the Game Boy runs at 59.7275 Hz, the 3DS LCD at 60 Hz, so we
 * cannot simply follow vblank - over a minute that drifts by roughly half a
 * second. Instead the emulator runs on its own tick deadline (the ARM11 tick
 * counter is exact) and the display is updated whenever a frame is ready. Sound
 * is buffered in audio.c, which absorbs the resulting jitter.
 *
 * MIT licensed.
 */
#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "emu.h"
#include "gb3ds.h"
#include "palette.h"
#include "ui.h"
#include "video.h"

#define GB_FRAME_HZ 59.7275
#define FRAME_TICKS ((u64)(SYSCLOCK_ARM11 / GB_FRAME_HZ))

/* Autosave cartridge RAM every 30 seconds of emulated time. */
#define AUTOSAVE_TICKS (SYSCLOCK_ARM11 * 30)

static u64 next_frame_tick;
static u64 autosave_tick;

/* ------------------------------------------------------------------------- */

static void platform_init(void)
{
	/* Old 3DS: this gives the app more of the system core's time budget;
	 * New 3DS: up to 3x the CPU clock, but the "speedup" bit is separate. */
	bool is_new = false;
	APT_CheckNew3DS(&is_new);
	if (is_new)
		osSetSpeedupEnable(true);
	APT_SetAppCpuTimeLimit(80);
	autosave_tick = svcGetSystemTick();

	video_init();
	ui_init();

	palette_init();
	config_load();

	palette_set(g_cfg.palette);
	video_set_scale(g_cfg.scale);

	if (!audio_init())
		g_cfg.audio_on = false;
	audio_set_enabled(g_cfg.audio_on);
}

static void platform_exit(void)
{
	emu_flush_save();
	config_save();
	audio_exit();
	video_exit();
}

/* ------------------------------------------------------------------------- */
/* Input                                                                     */
/* ------------------------------------------------------------------------- */

static void update_joypad(void)
{
	u32 held = hidKeysHeld();
	uint8_t pad = 0;

	if (held & KEY_A)
		pad |= GB_BTN_A;
	if (held & KEY_B)
		pad |= GB_BTN_B;
	if (held & KEY_START)
		pad |= GB_BTN_START;
	if (held & KEY_SELECT)
		pad |= GB_BTN_SELECT;
	if (held & (KEY_DUP | KEY_CPAD_UP))
		pad |= GB_BTN_UP;
	if (held & (KEY_DDOWN | KEY_CPAD_DOWN))
		pad |= GB_BTN_DOWN;
	if (held & (KEY_DLEFT | KEY_CPAD_LEFT))
		pad |= GB_BTN_LEFT;
	if (held & (KEY_DRIGHT | KEY_CPAD_RIGHT))
		pad |= GB_BTN_RIGHT;

	emu_set_joypad(pad);
}

/* ------------------------------------------------------------------------- */
/* Frame pacing                                                              */
/* ------------------------------------------------------------------------- */

static bool frame_due(void)
{
	u64 now = svcGetSystemTick();

	if (next_frame_tick == 0)
	{
		next_frame_tick = now;
		return true;
	}

	/* If we are early, sleep off most of the wait. svcSleepThread only has
	 * millisecond-ish granularity, so keep the last eighth of a frame as a
	 * short spin rather than overshooting the deadline. */
	if (now + FRAME_TICKS < next_frame_tick)
	{
		const u64 wake = next_frame_tick - FRAME_TICKS + FRAME_TICKS / 8;
		if (wake > now)
		{
			svcSleepThread(wake - now);
			now = svcGetSystemTick();
		}
	}

	if (now < next_frame_tick)
		return false;

	next_frame_tick += FRAME_TICKS;

	/* Fell more than four frames behind (menu, stalled SD card): resync
	 * instead of trying to catch up forever. */
	if (now - next_frame_tick > 4 * FRAME_TICKS)
		next_frame_tick = now + FRAME_TICKS;

	return true;
}

/* ------------------------------------------------------------------------- */

static void show_error(const char *msg)
{
	consoleClear();
	printf("\x1b[2;1H%s", msg);
	printf("\x1b[6;1HPress A to pick another ROM.");
	while (aptMainLoop())
	{
		hidScanInput();
		if (hidKeysDown() & (KEY_A | KEY_B))
			return;
		gspWaitForVBlank();
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	config_defaults();
	platform_init();

	const char *err = NULL;

	/* Load the last used ROM straight away, otherwise ask. */
	if (g_cfg.rom_path[0] != '\0')
		err = emu_load(g_cfg.rom_path);

	while (aptMainLoop())
	{
		if (!emu_loaded() || err != NULL)
		{
			char path[768];

			if (err != NULL)
			{
				show_error(err);
				err = NULL;
			}

			if (!ui_browse_rom(path, sizeof path))
				break;

			err = emu_load(path);
			if (err != NULL)
				continue;

			gbabe_copy(g_cfg.rom_path, sizeof g_cfg.rom_path, path);
			config_save();
			next_frame_tick = 0;
		}

		/* ---- play ---------------------------------------------------- */
		while (aptMainLoop() && emu_loaded())
		{
			hidScanInput();

			u32 down = hidKeysDown();

			if (down & KEY_X)
			{
				g_cfg.fast_forward = !g_cfg.fast_forward;
			}
			if (down & KEY_Y)
			{
				palette_set((palette_get() + 1) % palette_count());
				g_cfg.palette = palette_get();
				config_save();
			}
			if (down & KEY_TOUCH)
			{
				if (!ui_in_game_menu())
					goto done;
				next_frame_tick = 0;
			}
			if ((down & KEY_L) && (down & KEY_R))
			{
				if (!ui_in_game_menu())
					goto done;
				next_frame_tick = 0;
			}

			update_joypad();

			if (!frame_due())
			{
				svcSleepThread(200000); /* 0.2 ms, stay in the scheduler */
				continue;
			}

			/* Frame skip: present every other frame and skip drawing on
			 * the ones in between, so the two hardware buffers never
			 * disagree by more than one frame. */
			static unsigned frame_no;
			const bool draw = (g_cfg.frameskip == 0) || ((frame_no & 1) == 0);
			frame_no++;

			/* Turbo: one extra emulated frame per displayed frame. */
			if (g_cfg.fast_forward)
			{
				emu_set_rendering(false);
				emu_run_frame();
			}

			emu_set_rendering(draw);
			if (draw)
				video_begin_frame();

			emu_run_frame();

			if (draw)
				video_end_frame();

			ui_note_frame();

			const u64 now = svcGetSystemTick();
			if (now - autosave_tick > AUTOSAVE_TICKS)
			{
				emu_flush_save();
				autosave_tick = now;
			}
		}
	}

done:
	emu_unload();
	platform_exit();
	return 0;
}

/**
 * GBabe - Game Boy / Game Boy Color for the Nintendo 3DS.
 *
 * Main loop: services, input mapping, frame pacing.
 *
 * Frame pacing: the Game Boy runs at 59.7275 Hz, the 3DS LCD at 60 Hz, so we
 * cannot simply follow vblank - over a minute that drifts by roughly half a
 * second. Instead the emulator runs on its own tick deadline (the ARM11 tick
 * counter is exact) and the finished frame is handed to the panel and waited
 * for (video_end_frame()), which is what stops it tearing. Sound is buffered in
 * audio.c; its block deadlines are honoured from the same idle loop, so a block
 * is submitted when it is due rather than whenever a frame happens to end.
 *
 * MIT licensed.
 */
#include <3ds.h>

#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "emu.h"
#include "gb3ds.h"
#include "log.h"
#include "pace.h"
#include "palette.h"
#include "ui.h"
#include "video.h"

#define FRAME_TICKS ((u64)GBABE_FRAME_TICKS)

/* svcSleepThread() takes nanoseconds, the tick counter counts 1/268 MHz. */
#define TICKS_TO_NS(t) ((s64)((u64)(t) * 1000000000ull / SYSCLOCK_ARM11))

gbabe_perf g_perf;

/* Autosave cartridge RAM every 30 seconds of emulated time (see gb3ds.h for
 * why the product is not computed inline). */
#define AUTOSAVE_TICKS GBABE_AUTOSAVE_TICKS

static pace_state pace;
static u64 autosave_tick;
static u64 last_frame_end;

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

	/* Everything below is for the log: this is the information a bug report
	 * needs and nobody can read off the console's own menus. */
	log_init(GBABE_LOG_PATH, g_cfg.debug_log);
	log_line("GBabe %s build %s, %s, %s", GBABE_VERSION, GBABE_BUILD,
		 is_new ? "New 3DS/2DS" : "Old 3DS/2DS",
		 envIsHomebrew() ? "launched by a homebrew loader (3dsx)"
				 : "installed title (cia)");
	/* %u, not %.0f: SYSCLOCK_ARM11 is an integer constant in libctru, and
	 * handing an int to %f printed "tick rate 0 Hz" on hardware. */
	log_line("tick rate %u Hz, scale=%d palette=%d frameskip=%d",
		 (unsigned)SYSCLOCK_ARM11, g_cfg.scale, g_cfg.palette,
		 g_cfg.frameskip);

	if (!audio_init())
		g_cfg.audio_on = false;
	audio_set_enabled(g_cfg.audio_on);
	log_line("audio: %s, requested %s", audio_enabled() ? "on" : "off",
		 g_cfg.audio_on ? "on" : "off");
	log_flush();
}

static void platform_exit(void)
{
	log_line("exiting");
	log_shutdown();

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

/* The arithmetic lives in include/pace.h, where the host can test it (a frame
 * that is early, late, or far enough behind to need a new schedule), because
 * the bug that used to be in here was invisible from the outside: an unsigned
 * comparison made every single frame look like a resync. This function is only
 * the sleeping. */
static bool frame_due(void)
{
	u64 now = svcGetSystemTick();

	/* If we are early, sleep off most of the wait. The kernel's sleep
	 * granularity is coarser than a frame, so keep the last eighth of a
	 * frame as short polls rather than overshooting the deadline.
	 *
	 * (This used to pass ticks straight to svcSleepThread(), which wants
	 * nanoseconds - 3.7x too short, so the loop spent the rest of the frame
	 * polling and never really slept.) */
	const u64 wait = pace_wait(&pace, now);
	if (wait > 0)
	{
		const u64 margin = FRAME_TICKS / 8;
		if (wait > margin)
		{
			svcSleepThread(TICKS_TO_NS(wait - margin));
			now = svcGetSystemTick();
		}
	}

	return pace_due(&pace, now, FRAME_TICKS);
}

/* ------------------------------------------------------------------------- */

static void show_error(const char *msg)
{
	ui_message("Cannot open ROM", msg, "Press A to pick another ROM.");
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

			/* Nothing is playing while the browser is up: black the top
			 * screen instead of leaving the last game frame (or, on the
			 * first run, uninitialised framebuffer memory) there. */
			video_clear();

			if (!ui_browse_rom(path, sizeof path))
				break;

			err = emu_load(path);
			if (err != NULL)
				continue;

			gbabe_copy(g_cfg.rom_path, sizeof g_cfg.rom_path, path);
			config_save();
			pace_restart(&pace);
			last_frame_end = 0;

			const emu_info *ri = emu_get_info();
			log_line("rom: \"%s\" %s mbc%d %u rom banks %u ram banks",
				 ri->title, ri->cgb ? "CGB" : "DMG", ri->mbc,
				 ri->rom_banks, ri->ram_banks);
			log_line("rom path: %s", path);
			log_flush();
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
				emu_palette_changed();
				g_cfg.palette = palette_get();
				config_save();
			}
			if (down & KEY_TOUCH)
			{
				if (!ui_in_game_menu())
					goto done;
				pace_restart(&pace);
			}
			if ((down & KEY_L) && (down & KEY_R))
			{
				if (!ui_in_game_menu())
					goto done;
				pace_restart(&pace);
			}

			update_joypad();

			/* Fast-forward is not listenable. The loop emulates two
			 * frames per displayed one, so the sound arrives at twice
			 * the rate the channel eats, and the FIFO then does one of
			 * two things: overflows (samples dropped mid-stream, which
			 * is a tearing noise) or gets squeezed to the 20 % clamp (a
			 * chipmunk). Nothing plays while it is held, and the whole
			 * audio path - decimator included - is skipped, which is
			 * what makes it fast in the first place. */
			const bool want_audio = g_cfg.audio_on &&
						!g_cfg.fast_forward;
			if (want_audio != audio_enabled())
				audio_set_enabled(want_audio);

			/* Idle until the next emulated frame is due - or until a
			 * sound block has to be submitted, whichever comes first.
			 * Submitting blocks only at frame boundaries leaves a gap
			 * of up to 16.7 ms in the audio every 31 ms, which is
			 * audible as a 32 Hz stutter. */
			if (!frame_due())
			{
				u64 now = svcGetSystemTick();

				/* Everything worth waking for, and the swap
				 * poll that stands in for the vblank wait - the
				 * arithmetic is in pace.h, where the host tests
				 * it (a deadline in the past must not spin the
				 * loop, and a pending swap must not delay a
				 * frame). */
				const u64 wake = pace_wake(now, pace.next,
							   audio_next_deadline(),
							   video_swap_pending(),
							   FRAME_TICKS);

				if (wake > now)
					svcSleepThread(TICKS_TO_NS(wake - now));
				else
					svcSleepThread(200000); /* 0.2 ms */

				audio_pump();
				continue;
			}

			const u64 frame_start = svcGetSystemTick();

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
			emu_run_frame();

			/* The core reports bad opcodes and unmapped reads from
			 * inside a frame; the message is shown here, between
			 * frames, so the game keeps its timing model intact. */
			const char *core_err = emu_last_error();
			if (core_err != NULL)
				ui_message("Emulation error", core_err,
					   "The game may keep running.");

			const u64 after_core = svcGetSystemTick();

			/* Where that time went: Gambatte, the decimator, the scaled
			 * transpose. Counted inside the callees, so they are taken
			 * here too - an emulated frame with no drawing still decodes
			 * audio, and a skipped frame still runs the emulator. */
			u64 emu_ticks, audio_ticks, push_ticks, draw_ticks;
			const unsigned palette = emu_palette_take();

			emu_timing_take(&emu_ticks, &audio_ticks, &push_ticks);

			/* The picture goes to the panel *now*, after the emulation,
			 * and not before it. video_begin_frame() hands back the
			 * buffer the panel has finished with; asked at the top of
			 * the frame, 1 ms after the previous swap was posted, that
			 * answer is always "not yet", and the wait for it was a
			 * blocking vblank on every single frame - 4.3 ms, counted
			 * inside `core`, which is most of the reason this port ran
			 * at 56 fps when the emulator alone fits in 8.4 ms. */
			const u64 t_present = svcGetSystemTick();

			if (draw)
			{
				video_begin_frame();

				const uint16_t *px = emu_frame_buffer();
				if (px != NULL)
					video_draw_frame(px, 160);
			}

			draw_ticks = video_draw_take();
			const u64 t_drawn = svcGetSystemTick();

			/* What video_begin_frame() spent handing back a buffer the
			 * panel had not finished with. It is affordable now because
			 * the emulation has already run: the wait is the frame's
			 * slack, not an addition to it - the buffer the panel
			 * releases at the next vblank is the one this frame needs,
			 * and by then the frame is ready to use it. */
			const u64 sync = draw ? (t_drawn - t_present) - draw_ticks : 0;

			if (draw)
				video_end_frame();

			const u64 after_swap = svcGetSystemTick();

			/* ---- the window between the swap and the vblank ----------- */
			/* The frame is handed over; the panel takes it at the next
			 * vblank, and until then the loop has nothing to do. Work
			 * that is not per frame - the status readout, the log, the
			 * cartridge save - goes here, where the wait below absorbs it.
			 * After the wait there is about 0.1 ms of slack left and any
			 * of this pushes the next frame past its vblank: that was the
			 * 33 ms frame in every second of the console log. */
			if (draw)
			{
				ui_idle_work();
				log_flush();
				audio_log_stats();

				if (svcGetSystemTick() - autosave_tick > AUTOSAVE_TICKS)
				{
					emu_flush_save();
					autosave_tick = svcGetSystemTick();
				}
			}

			/* No vblank wait here either: the panel takes the swap
			 * while the loop emulates the next frame, and the wait
			 * for it happens in the next frame's present, where it is
			 * slack. Waiting here as well would only delay the frame
			 * that is already prepared. */
			const u64 after_post = svcGetSystemTick();

			if (draw)
			{
				/* What the emulator could not use because it was
				 * waiting for the next Game Boy frame to be due. */
				const u64 prev_end = last_frame_end;
				const u64 waited = (prev_end != 0 &&
						    frame_start > prev_end) ?
						   frame_start - prev_end : 0;
				unsigned late, resyncs;
				pace_take_counters(&pace, &late, &resyncs);

				const gbabe_frame_cost cost = {
					/* The work: Gambatte, the decimator, the
					 * transpose. It used to be timed from the
					 * start of the block to here, which was the
					 * same thing until the panel wait landed
					 * inside it. Now it is the sum, so the
					 * three parts add up to it by definition. */
					.core = emu_ticks + audio_ticks +
						push_ticks + draw_ticks,
					.emu = emu_ticks,
					.audio = audio_ticks,
					.push = push_ticks,
					.palette = palette,
					.draw = draw_ticks,
					.blit = after_swap - after_core,
					.sync = sync,
					.post = after_post - after_swap,
					.wait = waited,
					.frame = after_post - (prev_end ? prev_end : frame_start),
					.late = late,
					.resyncs = resyncs,
				};

				ui_note_frame(&cost);
				last_frame_end = after_post;
			}

			/* A frame that missed its deadline is what the player actually
			 * feels, and an average hides it. Logged at most once a second,
			 * with the split, so a stutter can be traced to one part. */
			static u64 last_slow_log;
			const u64 now = svcGetSystemTick();
			/* The work, not the wall period: the panel wait is not
			 * work. A frame that syncs to the vblank spends up to a
			 * whole panel period inside video_begin_frame() and is
			 * still perfectly on time, so it is subtracted here and
			 * reported as `sync`. What can make the loop miss a frame
			 * is emulation + transpose + the once-a-second work. */
			const double frame_ms =
				(double)(after_post - frame_start - sync) *
				1000.0 / SYSCLOCK_ARM11;
			if (draw && frame_ms > 16.7 &&
			    now - last_slow_log > SYSCLOCK_ARM11)
			{
				last_slow_log = now;

				/* The wait for the panel is not the blit. On
				 * hardware the panel's own frame is 16.67 ms and
				 * the Game Boy's is 16.74, so a frame that
				 * arrives just after a vblank waits a whole panel
				 * period - which reads as "blit 16.8" in this
				 * line while the copy itself is 0.1 ms. Split it:
				 * a 26 ms frame is core 10 + vblank 16, and the
				 * line should say so. */
				log_line("slow frame %.1f ms: core %.1f blit %.1f "
					 "post %.1f panel %.1f audio %u ms", frame_ms,
					 (double)(after_core - frame_start) * 1000.0 /
						 SYSCLOCK_ARM11,
					 (double)(after_swap - after_core) * 1000.0 /
						 SYSCLOCK_ARM11,
					 (double)(after_post - after_swap) * 1000.0 /
						 SYSCLOCK_ARM11,
					 (double)sync * 1000.0 / SYSCLOCK_ARM11,
					 audio_queue_ms());
			}
		}
	}

done:
	emu_unload();
	platform_exit();
	return 0;
}

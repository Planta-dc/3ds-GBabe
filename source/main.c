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
#include "hud.h"
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

/* Per-second totals for the four things the loop does after the emulation, in
 * ticks. Reported as one `once:` line a second: see the block that fills them. */
static u64 once_log, once_audio, once_save, once_since;
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
		/* The picture is one slot behind the emulator: a frame is
		 * emulated in one slot and presented at the top of the next,
		 * which is the only point in the loop where the panel has had a
		 * full period to release the buffer. `emu_frame_buffer()` still
		 * holds it until the next emulation overwrites it, so this costs
		 * one buffer, not two. */
		static bool frame_ready, draw_pending;
		static unsigned frame_skipped;

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

			/* ---- present the frame emulated in the previous slot --- */
			/* One Game Boy frame (16.74 ms) has passed since that swap
			 * was posted, which is longer than a panel period (16.72
			 * ms), so the panel has released the buffer by now: asking
			 * costs two registers and the answer is yes.
			 *
			 * This used to happen at the *end* of the frame instead,
			 * 10.6 ms after the swap, where the panel is still busy by
			 * construction - and there it waited, 5.6-6.3 ms of every
			 * frame, measured. That wait is the whole reason the
			 * console sat at 58.8 fps with only 9.6 ms of emulation in
			 * a 16.74 ms budget. When the answer here is no, the frame
			 * is dropped and the emulator keeps going: the loop must
			 * never be gated by the panel. */
			u64 draw_ticks = 0;
			const u64 t_present = svcGetSystemTick();

			if (frame_ready)
			{
				frame_ready = false;

				if (draw_pending)
				{
					if (video_begin_frame_try())
					{
						const uint16_t *px = emu_frame_buffer();

						if (px != NULL)
						{
							/* The status strip is drawn onto the frame
							 * itself, before it is presented, so it
							 * rides the transpose for free and can
							 * never cost a present (hud.h). */
							hud_draw((uint16_t *)px, 160);

							video_draw_frame(px, 160);
						}

						draw_ticks = video_draw_take();
						video_end_frame();
					}
					else
					{
						frame_skipped++;
					}
				}
			}

			const u64 after_swap = svcGetSystemTick();
			const u64 present = after_swap - t_present;

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

			frame_ready = true;
			draw_pending = draw;

			const u64 after_core = svcGetSystemTick();

			/* Where that time went: Gambatte, the decimator, the scaled
			 * transpose. Counted inside the callees, so they are taken
			 * here too - an emulated frame with no drawing still decodes
			 * audio, and a skipped frame still runs the emulator. */
			u64 emu_ticks, audio_ticks, push_ticks;
			const unsigned palette = emu_palette_take();

			emu_timing_take(&emu_ticks, &audio_ticks, &push_ticks);


			/* ---- the window between the swap and the vblank ----------- */
			/* The frame is handed over; the panel takes it at the next
			 * vblank, and until then the loop has nothing to do. Work
			 * that is not per frame - the log, the cartridge save -
			 * goes here, where the wait below absorbs it. After the
			 * wait there is about 0.1 ms of slack left and any of this
			 * pushes the next frame past its vblank: that was the 33 ms
			 * frame in every second of the console log. */
			if (draw)
			{
				/* Timed apart, and reported once a second as `once:`
				 * in the log. Every second the console's log showed
				 * a 33 ms frame whose `post` was 20-27 ms while the
				 * emulation in it was 6-8, and there were four
				 * candidates in this block - the status readout,
				 * the log's SD write, the audio bookkeeping and the
				 * cartridge autosave. Guessing which one is how
				 * this project has wasted time before, so the log
				 * says which one it is. The status readout is no
				 * longer one of them: it is drawn on the game frame
				 * itself now (hud.h), and costs a few hundredths of a
				 * millisecond instead of a present. */
				u64 t = svcGetSystemTick();

				log_flush_soft();

				const u64 t_log = svcGetSystemTick();

				once_log += t_log - t;
				t = t_log;

				audio_log_stats();

				/* The log write above is an SD card operation, and
				 * on a slow card it is milliseconds - a dead window
				 * for the sound in the middle of every frame that
				 * had something to write. One pump before and one
				 * after is enough to keep the deadline inside it. */
				audio_pump();

				const u64 t_audio = svcGetSystemTick();

				once_audio += t_audio - t;
				t = t_audio;

				if (t - autosave_tick > AUTOSAVE_TICKS)
				{
					emu_flush_save();
					autosave_tick = svcGetSystemTick();
				}

				once_save += svcGetSystemTick() - t;

				if (svcGetSystemTick() - once_since > SYSCLOCK_ARM11)
				{
					const double ms = 1000.0 / SYSCLOCK_ARM11;

					log_line("once: log %.1f audio %.1f save %.1f ms "
						 "(worst frame of the second: "
						 "see above)", (double)once_log * ms,
						 (double)once_audio * ms,
						 (double)once_save * ms);
					once_log = once_audio = once_save = 0;
					once_since = svcGetSystemTick();
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
				u64 slip;
				pace_take_counters(&pace, &late, &resyncs, &slip);

				/* The presents this window had to drop, as a
				 * window figure: frame_skipped is the running
				 * total the slow-frame line reports. Only draw
				 * frames can drop one, and only draw frames
				 * get here, so the difference is exact. */
				static unsigned skipped_seen;
				const unsigned dropped =
					frame_skipped - skipped_seen;
				skipped_seen = frame_skipped;

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
					/* The present block: the buffer handover,
					 * the transpose and the swap. It no longer
					 * contains a wait - video_begin_frame_try()
					 * never blocks - so `sync` is zero by
					 * construction and kept only so old logs
					 * still line up with new ones. */
					.blit = present,
					.sync = 0,
					/* From the end of the emulation: the present
					 * block now happens at the top of the slot,
					 * so measuring from it swallowed `core` and
					 * made `other` a constant -core. */
					.post = after_post - after_core,
					.wait = waited,
					.frame = after_post - (prev_end ? prev_end : frame_start),
					.late = late,
					.resyncs = resyncs,
					.slip = slip,
					.dropped = dropped,
				};

				ui_note_frame(&cost);
				last_frame_end = after_post;
			}

			/* A frame that missed its deadline is what the player actually
			 * feels, and an average hides it. Logged at most once a second,
			 * with the split, so a stutter can be traced to one part. */
			static u64 last_slow_log;
			const u64 now = svcGetSystemTick();
			/* The whole slot, from its start to the end of the
			 * once-a-second work: nothing in it waits for the panel
			 * any more, so there is no longer a term to subtract. */
			const double frame_ms =
				(double)(after_post - frame_start) *
				1000.0 / SYSCLOCK_ARM11;
			if (draw && frame_ms > 16.7 &&
			    now - last_slow_log > SYSCLOCK_ARM11)
			{
				last_slow_log = now;

				/* Nothing here waits for the panel any more, so
				 * a frame that overruns is work: the present
				 * block, the emulation, or the once-a-second
				 * work. If `present` itself is large, the panel
				 * was busy and video_begin_frame_try() should
				 * have said so - `skip` says how often it
				 * did. */
				log_line("slow frame %.1f ms: present %.1f core %.1f "
					 "post %.1f skip %u audio %u ms", frame_ms,
					 (double)present * 1000.0 / SYSCLOCK_ARM11,
					 (double)(after_core - after_swap) * 1000.0 /
						 SYSCLOCK_ARM11,
					 (double)(after_post - after_swap) * 1000.0 /
						 SYSCLOCK_ARM11,
					 frame_skipped, audio_queue_ms());
			}
		}

		/* The game has been unloaded - quit to the browser, or the menu
		 * reset it: either way this session is over, and its one-line
		 * summary belongs in the log before the next one starts. */
		ui_log_session();
	}

done:
	ui_log_session();
	emu_unload();
	platform_exit();
	return 0;
}

/**
 * Host benchmark for the emulator core and the blit.
 *
 *   make -C tools/host bench ROM=game.gb
 *
 * Prints the per-frame split the HUD shows, measured on the PC. Useful for
 * comparing compiler flags or core settings, because an ARM11 is a different
 * machine - numbers here only say which way something moved, never how many
 * fps a 3DS will do.
 *
 * NOT part of the 3DS build.
 */
#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "audio.h"
#include "emu.h"
#include "gb3ds.h"
#include "palette.h"
#include "video.h"

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: %s ROM [frames]\n", argv[0]);
		return 2;
	}

	const int frames = (argc > 2) ? atoi(argv[2]) : 600;

	config_defaults();
	video_init();
	video_set_scale(SCALE_FIT);
	palette_init();
	palette_set(g_cfg.palette);
	audio_init();
	audio_set_enabled(false); /* APU mixing is measured separately */

	const char *err = emu_load(argv[1]);
	if (err != NULL)
	{
		fprintf(stderr, "emu_load: %s\n", err);
		return 1;
	}

	/* warm up */
	for (int i = 0; i < 30; i++)
		emu_run_frame();

	double t0 = now_ms();
	for (int i = 0; i < frames; i++)
		emu_run_frame();
	double t1 = now_ms();

	/* The real blit: one whole frame per call. The vblank wait inside
	 * video_end_frame() is free here (the host stand-in presents
	 * immediately), so this measures the copy and nothing else. */
	static uint16_t px[144 * 160];
	for (int i = 0; i < frames; i++)
	{
		video_begin_frame();
		video_draw_frame(px, 160);
		video_end_frame();
	}
	double t2 = now_ms();

	/* audio on, to show what the APU costs */
	audio_set_enabled(true);
	for (int i = 0; i < 10; i++)
		emu_run_frame();
	double t3 = now_ms();
	for (int i = 0; i < frames; i++)
		emu_run_frame();
	double t4 = now_ms();

	const double core = (t1 - t0) / frames;
	const double blit = (t2 - t1) / frames;
	const double with_apu = (t4 - t3) / frames;

	printf("core          %6.3f ms/frame  (%6.0f fps)\n", core, 1000.0 / core);
	printf("blit          %6.3f ms/frame  (scale fit, 267x240)\n", blit);
	printf("core + APU    %6.3f ms/frame  (%6.0f fps)\n", with_apu,
	       1000.0 / with_apu);
	printf("frame total   %6.3f ms  -> %.0f %% of a 16.74 ms budget\n",
	       core + blit, (core + blit) / 16.74 * 100.0);

	return 0;
}

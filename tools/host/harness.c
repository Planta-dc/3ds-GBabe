/**
 * Headless harness: loads a ROM, runs N frames through the real front-end code
 * paths (emu.c -> video.c) and writes the top screen as a PPM.
 *
 *   make -C tools/host run ROM=/path/to/game.gb FRAMES=300 OUT=shot.ppm
 *
 * Used to sanity check the rendering path and to make sure the whole front-end
 * compiles and links. NOT part of the 3DS build.
 */
#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "emu.h"
#include "gb3ds.h"
#include "palette.h"
#include "video.h"

/* Provided by main.c, compiled with -Dmain=gbabe_main. */
int gbabe_main(int argc, char **argv);

static void write_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (f == NULL)
		return;

	fprintf(f, "P6\n400 240\n255\n");
	for (int i = 0; i < 400 * 240; i++)
	{
		u16 p = g_host_fb[i];
		unsigned char rgb[3] = {
			(unsigned char)(((p >> 11) & 0x1F) * 255 / 31),
			(unsigned char)(((p >> 5) & 0x3F) * 255 / 63),
			(unsigned char)((p & 0x1F) * 255 / 31),
		};
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: %s ROM [frames] [out.ppm] [scale]\n", argv[0]);
		return 2;
	}

	int frames = (argc > 2) ? atoi(argv[2]) : 300;
	const char *out = (argc > 3) ? argv[3] : "screen.ppm";
	int scale = (argc > 4) ? atoi(argv[4]) : SCALE_FIT;

	config_defaults();
	video_init();
	video_set_scale(scale);
	palette_init();
	palette_set(g_cfg.palette);

	/* CSND is not available on the host, so this reports "no audio" and the
	 * emulator carries on silently - the same path an Old 3DS takes when the
	 * sound service is busy. */
	audio_init();
	audio_set_enabled(g_cfg.audio_on);

	const char *err = emu_load(argv[1]);
	if (err != NULL)
	{
		fprintf(stderr, "emu_load: %s\n", err);
		return 1;
	}

	for (int i = 0; i < frames; i++)
	{
		video_begin_frame();
		emu_run_frame();
		video_end_frame();
	}

	write_ppm(out);

	const emu_info *info = emu_get_info();
	printf("title=%s %s mbc=%d rom_banks=%u ram_banks=%u hash=0x%02X\n",
	       info->title, info->cgb ? "CGB" : "DMG", info->mbc,
	       info->rom_banks, info->ram_banks, info->auto_hash);
	printf("wrote %s after %d frames\n", out, frames);

	emu_unload();
	audio_exit();

	/* Tearing: a frame that draws into the buffer the panel is still showing
	 * produces a tear line, and because the Game Boy's 59.7275 Hz does not
	 * divide 60 Hz it does not sit still - it slides down the screen. */
	printf("tears=%d (must be 0)\n", g_host_tears);
	if (g_host_tears != 0)
	{
		printf("torn frames: video_begin_frame() got the displayed buffer "
		       "while a swap was still pending\n");
		return 1;
	}
	return 0;
}

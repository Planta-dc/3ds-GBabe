/**
 * A four-function libretro frontend, to test libretro/gbabe_core.cpp without
 * RetroArch.
 *
 *   make -C libretro test
 *
 * It is deliberately dumb: it agrees to the pixel format, collects the
 * callbacks, drives retro_run() a fixed number of times and checks the things
 * a frontend would notice if they were wrong. That is the whole value - a core
 * that "looks fine in RetroArch" is not testable, and a core that renders the
 * wrong picture is exactly what this project has been bitten by before.
 *
 * What it checks, and why each one matters:
 *
 *   api version   a core built against a mismatched header is a mystery crash
 *                 in a frontend, and a one-line check here;
 *   pixel format  RGB565 has to be agreed *before* the first frame - a core that
 *                 forgets shows swapped channels;
 *   av_info       fps must be 4194304/70224 = 59.7275, not 60, and the sample
 *                 rate must be the 32768 Hz the decimator produces: a frontend
 *                 paces itself from these two numbers;
 *   frames        every retro_run() must produce exactly one video frame at
 *                 160x144, and about 547 stereo samples (32768 / 59.7275);
 *   audio         the batch callback must be called, with a plausible count;
 *   savestate     serialize / unserialize must round-trip: run, save, run,
 *                 load, run, and land on the same picture;
 *   memory        SAVE_RAM has to point somewhere the frontend can write, and
 *                 acid2 has no battery, so size 0 is the *right* answer here;
 *   picture       the last frame is written as a PPM so check-render.py can
 *                 compare it with the reference the 3DS path is held to.
 *
 * MIT licensed.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

/* ------------------------------------------------------------------------- */

static int failures;
static uint16_t frame[160 * 144];
static int frame_calls;
static unsigned frame_w, frame_h, frame_pitch;
static long audio_frames;
static int audio_calls;
static bool rgb565 = true;

static void check(const char *what, int ok)
{
	printf("%s: %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		failures++;
}

static bool env_cb(unsigned cmd, void *data)
{
	switch (cmd)
	{
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		rgb565 = (*(const enum retro_pixel_format *)data ==
			  RETRO_PIXEL_FORMAT_RGB565);
		return true;
	default:
		return false;
	}
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
	if (data != NULL)
	{
		/* Copy, don't keep the pointer: a frontend that kept it would be
		 * reading the core's buffer after retro_run() recycled it. */
		const uint8_t *src = (const uint8_t *)data;
		const unsigned row = w * 2;

		for (unsigned y = 0; y < h; y++)
			memcpy(&frame[y * w], src + y * pitch, row);
	}
	else
	{
		/* Duplicate frame: legal, and still one frame. */
	}

	frame_calls++;
	frame_w = w;
	frame_h = h;
	frame_pitch = (unsigned)pitch;
}

static void audio_cb(int16_t left, int16_t right)
{
	(void)left;
	(void)right;
	audio_frames++;
	audio_calls++;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
	(void)data;
	audio_frames += (long)frames;
	audio_calls++;
	return frames;
}

static void input_poll_cb(void) {}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index,
			      unsigned id)
{
	(void)port;
	(void)device;
	(void)index;
	(void)id;
	return 0;
}

/* ------------------------------------------------------------------------- */

static void dump_ppm(const char *path, unsigned w, unsigned h)
{
	FILE *f = fopen(path, "wb");
	if (f == NULL)
	{
		printf("cannot write %s\n", path);
		failures++;
		return;
	}

	fprintf(f, "P6\n%u %u\n255\n", w, h);

	for (unsigned i = 0; i < w * h; i++)
	{
		const uint16_t p = frame[i];
		unsigned char rgb[3];

		if (rgb565)
		{
			const unsigned r = (p >> 11) & 0x1F;
			const unsigned g = (p >> 5) & 0x3F;
			const unsigned b = p & 0x1F;

			rgb[0] = (unsigned char)((r << 3) | (r >> 2));
			rgb[1] = (unsigned char)((g << 2) | (g >> 4));
			rgb[2] = (unsigned char)((b << 3) | (b >> 2));
		}
		else
		{
			const unsigned r = (p >> 10) & 0x1F;
			const unsigned g = (p >> 5) & 0x1F;
			const unsigned b = p & 0x1F;

			rgb[0] = (unsigned char)((r << 3) | (r >> 2));
			rgb[1] = (unsigned char)((g << 3) | (g >> 2));
			rgb[2] = (unsigned char)((b << 3) | (b >> 2));
		}

		fwrite(rgb, 1, 3, f);
	}

	fclose(f);
}

static uint32_t hash_frame(void)
{
	uint32_t h = 2166136261u;

	for (unsigned i = 0; i < 160 * 144; i++)
	{
		h ^= frame[i];
		h *= 16777619u;
	}

	return h;
}

static void run_frames(int n)
{
	for (int i = 0; i < n; i++)
		(retro_run)();
}

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	const char *rom_path = (argc > 1) ? argv[1] : "../../test/dmg-acid2.gb";
	const char *out_path = (argc > 2) ? argv[2] : NULL;
	const int frames_to_run = (argc > 3) ? atoi(argv[3]) : 300;

	setvbuf(stdout, NULL, _IONBF, 0);

	check("api version is 1", retro_api_version() == RETRO_API_VERSION);

	retro_set_environment(env_cb);
	retro_set_video_refresh(video_cb);
	retro_set_audio_sample(audio_cb);
	retro_set_audio_sample_batch(audio_batch_cb);
	retro_set_input_poll(input_poll_cb);
	retro_set_input_state(input_state_cb);

	retro_init();

	struct retro_system_info info;
	memset(&info, 0, sizeof info);
	retro_get_system_info(&info);
	printf("core: %s %s (%s)\n", info.library_name, info.library_version,
	       info.valid_extensions);
	check("the core names itself",
	      info.library_name != NULL && strstr(info.library_name, "GBabe"));
	/* A core that requires a full path cannot be loaded from a frontend's
	 * ZIP or from the memory test below. */
	check("a ROM can be handed over in memory", info.need_fullpath == false);

	struct retro_game_info game;
	memset(&game, 0, sizeof game);
	game.path = rom_path;

	check("the ROM loads", retro_load_game(&game));

	/* Pixel format must be agreed during load - after that the frontend has
	 * allocated its frames. */
	check("RGB565 was negotiated", rgb565);

	struct retro_system_av_info av;
	memset(&av, 0, sizeof av);
	retro_get_system_av_info(&av);
	printf("av: %ux%u, %.4f fps, %.0f Hz\n", av.geometry.base_width,
	       av.geometry.base_height, av.timing.fps, av.timing.sample_rate);
	check("the geometry is the Game Boy's",
	      av.geometry.base_width == 160 && av.geometry.base_height == 144);
	/* 4194304 / 70224. A core that says 60 here makes the frontend run the
	 * Game Boy 0.45 % fast and drift against the panel - the whole subject
	 * of docs/PORT-NOTES.md §10-§14. */
	check("the frame rate is the Game Boy's 59.7275, not 60",
	      av.timing.fps > 59.72 && av.timing.fps < 59.73);
	check("the sample rate is the one the decimator produces",
	      (int)av.timing.sample_rate == 32768);

	run_frames(frames_to_run);

	printf("ran %d frames: %d video callbacks, %ld stereo samples in %d "
	       "batches\n", frames_to_run, frame_calls, audio_frames,
	       audio_calls);

	check("every frame produced exactly one video frame",
	      frame_calls == frames_to_run);
	check("the picture is 160x144", frame_w == 160 && frame_h == 144);
	check("the pitch matches the picture", frame_pitch == 160 * 2);

	/* 32768 / 59.7275 = 548.6 samples per frame. The decimator drops the
	 * remainder each frame, so this is a range, not an equality: what matters
	 * is that it is neither silent nor twice too much. */
	{
		const double per = (double)audio_frames / frames_to_run;
		printf("audio: %.1f samples per frame (548.6 expected)\n", per);
		check("the audio rate is right", per > 535.0 && per < 560.0);
		/* runFor() returns several times per frame, and each return with
		 * samples pushes a batch - so "a few per frame" is the right shape,
		 * and "one call per sample" (549 x frames) is the failure this is
		 * looking for: a core that ignores the batch callback and feeds
		 * samples one at a time stalls a real frontend's lock-free queue. */
		const double batches = (double)audio_calls / frames_to_run;
		printf("audio: %.2f batches per frame, %.1f samples each\n",
		       batches, (double)audio_frames / audio_calls);
		/* One batch per frame: samples are accumulated across the
		 * runFor() calls and pushed once. More than that is not a bug,
		 * but it is work the frontend then has to do per call, and on a
		 * 3DS that is a lock-free queue pop on the audio thread each
		 * time. */
		check("audio arrives as one batch per frame", batches <= 1.0);
	}

	/* Savestates: run, save, run, restore, run - the picture after restoring
	 * has to match the picture before, or the state is not complete. */
	{
		const size_t size = retro_serialize_size();
		printf("savestate: %zu bytes\n", (size_t)size);
		check("the savestate has a size", size > 0 && size < (1u << 22));

		uint8_t *state = (uint8_t *)malloc(size);
		check("the savestate can be written",
		      state != NULL && retro_serialize(state, size));

		run_frames(3);
		const uint32_t after = hash_frame();

		check("the savestate can be restored",
		      retro_unserialize(state, size));
		run_frames(3);
		printf("state round trip: %08x vs %08x\n", after, hash_frame());
		check("restoring a state lands on the same picture",
		      hash_frame() == after);

		free(state);
	}

	/* Memory maps: acid2 has no battery, and 0 bytes of SAVE_RAM is the
	 * correct answer - a core that invents a pointer there makes the frontend
	 * write a .srm nobody can load. */
	printf("memory: save %zu bytes, rtc %zu bytes\n",
	       retro_get_memory_size(RETRO_MEMORY_SAVE_RAM),
	       retro_get_memory_size(RETRO_MEMORY_RTC));
	check("a batteryless cart reports no save RAM",
	      retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0);

	if (out_path != NULL)
	{
		dump_ppm(out_path, 160, 144);
		printf("wrote %s\n", out_path);
	}

	retro_unload_game();
	retro_deinit();

	printf("core: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}

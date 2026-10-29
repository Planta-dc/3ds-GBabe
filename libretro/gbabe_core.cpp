/**
 * GBabe as a libretro core.
 *
 * This is the "base built on RetroArch" that docs/RETROARCH-3DS.md argues for:
 * the emulation side of GBabe (Gambatte 0.5.0, the same decimation to 32768 Hz,
 * the same runFor() loop that tools/host and the 3DS build use) behind the
 * libretro API, so it can run under a mature frontend - including the old
 * RetroArch 3DS, which ships a Gambatte build of its own. The point is a
 * control experiment for the frame-rate work: if Gambatte-under-RetroArch-3DS
 * runs the same ROM better than GBabe does, the front-end is the problem and
 * its drivers are worth copying; if it does not, the core is at its limit on a
 * 268 MHz ARM11 and no amount of presentation rework will help.
 *
 * What is deliberately *not* reused here:
 *
 *   - source/emu_gambatte.cpp. It is written for the 3DS front-end: it owns SD
 *     paths, .sav/.rtc sidecars, the CSND FIFO and the bottom-screen palette
 *     push. A core must not touch any of that - the frontend owns files and
 *     audio output, and the same binary has to run on a PC. The Gambatte glue
 *     below is therefore its own ~120 lines.
 *   - video.c, audio.c, ui*.c. Same reason, and they are the things under
 *     test.
 *
 * The duck-picture checks in tools/host are what keep the two paths honest:
 * libretro/test_frontend.c renders dmg-acid2 through *this* file and compares
 * it with test/dmg-acid2-reference.ppm, the same reference the 3DS path is
 * held to.
 *
 * Licence: this file is MIT, like the rest of GBabe's own code, but it links
 * Gambatte, which is GPL-2.0 - so the resulting core is GPL-2.0. See
 * third_party/gambatte/README.md. libretro.h is MIT (its own header says so)
 * and is vendored next to this file.
 *
 * MIT licensed (GBabe's own part).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "gambatte.h"
#include "libretro.h"

/* ------------------------------------------------------------------------- */
/* Input                                                                     */
/* ------------------------------------------------------------------------- */

/* Gambatte's InputGetter bits, the same values as GB_BTN_* in gb3ds.h. Kept
 * local because this file must not include the 3DS front-end's headers. */
enum
{
	BTN_A      = 0x01,
	BTN_B      = 0x02,
	BTN_SELECT = 0x04,
	BTN_START  = 0x08,
	BTN_RIGHT  = 0x10,
	BTN_LEFT   = 0x20,
	BTN_UP     = 0x40,
	BTN_DOWN   = 0x80
};

static unsigned pad_mask;

class Pad : public gambatte::InputGetter
{
public:
	unsigned operator()() { return pad_mask; }
};

/* The vendored Gambatte is built for this repository and reports its own
 * problems through log_line() (include/log.h), which belongs to the 3DS
 * front-end. A core is loaded into somebody else's process, so it must not
 * carry a dependency on that: this is the one symbol it needs, and it does
 * nothing. Silencing it is right, not lazy - a core that writes to stderr is a
 * core whose messages land in the frontend's terminal, on a device where there
 * is no terminal. */
extern "C" void log_line(const char *fmt, ...)
{
	(void)fmt;
}

/* ------------------------------------------------------------------------- */
/* Frontend callbacks                                                        */
/* ------------------------------------------------------------------------- */

static retro_environment_t        env_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_t       audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;

/* RGB565 unless the frontend insists otherwise (0RGB1555 is libretro's
 * default when a core does not ask). No conversion path for XRGB8888: asking
 * for 16-bit pixels is what every GB core does, and a frontend that refuses
 * would rather convert itself than have us second-guess it. */
static bool want_rgb565 = true;

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static gambatte::GB *core;
static Pad pad;

static std::vector<uint8_t> rom; /* the core does not take ownership */

static gambatte::video_pixel_t vbuf[144 * 160];
static uint16_t frame_out[144 * 160]; /* what actually goes to the frontend */

/* 35112 samples per frame at 2097152 Hz, in 2064-sample calls, exactly as in
 * source/emu_gambatte.cpp. */
enum { RUN_SAMPLES = 2064, SOUND_BUF = RUN_SAMPLES + 2064 };

static gambatte::uint_least32_t sound_buf[SOUND_BUF];
static int16_t audio_mid[520 * 2]; /* 4128 / 8 with slack */
static int16_t audio_out[72 * 2];  /* and / 8 again */

/* One frontend batch per frame. runFor() returns (and so yields samples) a
 * dozen-odd times per frame, and handing the frontend 31 samples at a time 17
 * times a frame is legal but needless: a frame is 549 stereo samples, so
 * accumulate and push once. */
static int16_t frame_audio[600 * 2];
static unsigned frame_audio_used;

static bool loaded;
static bool av_set;

/* ------------------------------------------------------------------------- */
/* Audio: 2097152 Hz -> 32768 Hz                                             */
/* ------------------------------------------------------------------------- */

/* Two stages of 8:1 box averaging.
 *
 * This is what the 3DS front-end used before source/decimator.c replaced its
 * second stage with a 64-tap windowed sinc (-74 dB of aliasing at 50 kHz
 * against -14 dB here). The core does not get that filter, and is not
 * going to: this file is deliberately independent of source/ and include/ (see
 * Makefile), because it is the thing the front-end is compared *against*. So a
 * comparison between the two paths is a comparison of frontends, and the two
 * do not produce the same samples - the front-end's stream is the cleaner of
 * the two. Nothing else in the core's output differs. */
struct Downsampler
{
	int32_t sum_l, sum_r;
	uint8_t count;

	void reset() { sum_l = sum_r = 0; count = 0; }

	unsigned run(const int16_t *in, unsigned n, int16_t *out)
	{
		unsigned written = 0;

		for (unsigned i = 0; i < n; i++)
		{
			sum_l += in[i * 2];
			sum_r += in[i * 2 + 1];

			if (++count == 8)
			{
				count = 0;
				out[written * 2]     = (int16_t)(sum_l >> 3);
				out[written * 2 + 1] = (int16_t)(sum_r >> 3);
				written++;
				sum_l = sum_r = 0;
			}
		}

		return written;
	}
};

static Downsampler stage1, stage2;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static void push_video(unsigned width, unsigned height)
{
	if (video_cb == NULL)
		return;

	/* Gambatte renders RGB565 (VIDEO_RGB565); a frontend that did not accept
	 * that format gets 0RGB1555 and a shifted 5-bit blue, which looks slightly
	 * dark on DMG greys but never wrong in shape. Nothing here converts whole
	 * frames otherwise. */
	if (want_rgb565)
	{
		video_cb(vbuf, width, height, width * (unsigned)sizeof(uint16_t));
		return;
	}

	const unsigned n = width * height;

	for (unsigned i = 0; i < n; i++)
	{
		const uint16_t p = vbuf[i];
		/* RGB565 (rrrrr gggggg bbbbb) -> 0RGB1555 (0 rrrrr ggggg bbbbb):
		 * R and B are already 5 bits, green loses its LSB. */
		const uint16_t r = (uint16_t)((p >> 11) & 0x1Fu);
		const uint16_t g = (uint16_t)((p >> 5) & 0x3Fu);
		const uint16_t b = (uint16_t)(p & 0x1Fu);

		frame_out[i] = (uint16_t)((r << 10) | ((g >> 1) << 5) | b);
	}

	video_cb(frame_out, width, height, width * (unsigned)sizeof(uint16_t));
}

static void push_audio(const int16_t *data, unsigned frames)
{
	if (frames == 0)
		return;

	if (audio_batch_cb != NULL)
	{
		audio_batch_cb(data, frames);
		return;
	}

	if (audio_cb != NULL)
		for (unsigned i = 0; i < frames; i++)
			audio_cb(data[i * 2], data[i * 2 + 1]);
}

/* ------------------------------------------------------------------------- */
/* libretro API                                                              */
/* ------------------------------------------------------------------------- */

unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

void retro_set_environment(retro_environment_t cb)        { env_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb)   { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)     { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
	audio_batch_cb = cb;
}
void retro_set_input_poll(retro_input_poll_t cb)         { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb)       { input_state_cb = cb; }

void retro_get_system_info(struct retro_system_info *info)
{
	memset(info, 0, sizeof *info);
	info->library_name     = "GBabe";
	info->library_version  = "0.3.0";
	info->valid_extensions = "gb|gbc";
	/* false: the frontend may hand us the ROM in memory, which is how the
	 * host test and RetroArch's zipped content both load it. */
	info->need_fullpath    = false;
	info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
	memset(info, 0, sizeof *info);
	info->geometry.base_width   = 160;
	info->geometry.base_height  = 144;
	info->geometry.max_width    = 160;
	info->geometry.max_height   = 144;
	info->geometry.aspect_ratio = 160.0f / 144.0f;
	/* The Game Boy's real rate: 4194304 Hz / 70224 cycles. Not 60: the
	 * difference is what the whole pacing discussion in docs/PORT-NOTES.md
	 * is about, and a frontend that honours it will run us correctly. */
	info->timing.fps         = 4194304.0 / 70224.0;
	info->timing.sample_rate = 32768.0;
}

void retro_init(void)
{
	pad_mask = 0;
	loaded   = false;
	av_set   = false;
	core     = NULL;
}

void retro_deinit(void)
{
	if (core != NULL)
	{
		delete core;
		core = NULL;
	}

	rom.clear();
	loaded = false;
}

unsigned retro_get_region(void)
{
	return RETRO_REGION_NTSC;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
	(void)port;
	(void)device;
}

void retro_reset(void)
{
	if (loaded && core != NULL)
	{
		core->reset();
		stage1.reset();
		stage2.reset();
	}
}

/* One emulated frame, split exactly like emu_run_frame(): runFor() returns when
 * a frame has been drawn, which is the only safe point to hand the picture to
 * the frontend. */
void retro_run(void)
{
	if (!loaded || core == NULL)
		return;

	if (input_poll_cb != NULL)
		input_poll_cb();

	unsigned mask = 0;

	if (input_state_cb != NULL)
	{
		static const struct { unsigned id, bit; } map[] = {
			{ RETRO_DEVICE_ID_JOYPAD_A,      BTN_A      },
			{ RETRO_DEVICE_ID_JOYPAD_B,      BTN_B      },
			{ RETRO_DEVICE_ID_JOYPAD_SELECT, BTN_SELECT },
			{ RETRO_DEVICE_ID_JOYPAD_START,  BTN_START  },
			{ RETRO_DEVICE_ID_JOYPAD_RIGHT,  BTN_RIGHT  },
			{ RETRO_DEVICE_ID_JOYPAD_LEFT,   BTN_LEFT   },
			{ RETRO_DEVICE_ID_JOYPAD_UP,     BTN_UP     },
			{ RETRO_DEVICE_ID_JOYPAD_DOWN,   BTN_DOWN   },
		};

		for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
			if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, map[i].id))
				mask |= map[i].bit;
	}

	pad_mask = mask;

	for (;;)
	{
		unsigned samples = RUN_SAMPLES;

		long ret = core->runFor(vbuf, 160, sound_buf, SOUND_BUF, samples);

		if (samples > SOUND_BUF)
			samples = SOUND_BUF; /* documented to overshoot by 2064 */

		if (samples > 0)
		{
			unsigned mid = stage1.run((const int16_t *)sound_buf,
						  samples, audio_mid);
			unsigned out = stage2.run(audio_mid, mid, audio_out);

			if (frame_audio_used + out <= 600)
			{
				memcpy(&frame_audio[frame_audio_used * 2], audio_out,
				       out * 2 * sizeof(int16_t));
				frame_audio_used += out;
			}
		}

		if (ret >= 0)
			break;
	}

	push_audio(frame_audio, frame_audio_used);
	frame_audio_used = 0;

	push_video(160, 144);
}

size_t retro_serialize_size(void)
{
	if (!loaded || core == NULL)
		return 0;

	return core->stateSize();
}

bool retro_serialize(void *data, size_t size)
{
	if (!loaded || core == NULL || data == NULL)
		return false;

	const size_t need = core->stateSize();

	if (size < need)
		return false;

	core->saveState(data);
	return true;
}

bool retro_unserialize(const void *data, size_t size)
{
	if (!loaded || core == NULL || data == NULL)
		return false;

	return core->loadState(data, size);
}

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
	(void)index;
	(void)enabled;
	(void)code;
}

static bool load_rom(const struct retro_game_info *game)
{
	if (game == NULL)
		return false;

	if (game->data != NULL && game->size > 0)
	{
		rom.assign((const uint8_t *)game->data,
			   (const uint8_t *)game->data + game->size);
	}
	else if (game->path != NULL)
	{
		FILE *f = fopen(game->path, "rb");
		if (f == NULL)
			return false;

		uint8_t chunk[16384];
		size_t got;

		rom.clear();
		while ((got = fread(chunk, 1, sizeof chunk, f)) > 0)
			rom.insert(rom.end(), chunk, chunk + got);

		fclose(f);
	}
	else
	{
		return false;
	}

	/* A Game Boy header is at 0x100..0x14F; anything shorter is not a ROM and
	 * Gambatte's loader would read past the end. */
	if (rom.size() < 0x150)
	{
		rom.clear();
		return false;
	}

	return true;
}

bool retro_load_game(const struct retro_game_info *game)
{
	if (!load_rom(game))
		return false;

	/* Pixel format has to be agreed before the first frame. Asking late is
	 * how a core ends up showing swapped channels. */
	if (!av_set)
	{
		enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;

		if (env_cb != NULL &&
		    env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
			want_rgb565 = true;
		else
			want_rgb565 = false; /* 0RGB1555 is libretro's default */

		av_set = true;
	}

	core = new gambatte::GB();
	core->setInputGetter(&pad);
	/* Same as the 3DS path: with colour correction on, the same ROM would not
	 * render the same colours the reference test ROM expects. */
	core->setColorCorrection(false);

	if (core->load(rom.data(), (unsigned)rom.size(), 0) != 0)
	{
		delete core;
		core = NULL;
		rom.clear();
		return false;
	}

	stage1.reset();
	stage2.reset();
	loaded = true;
	return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info,
			     size_t num)
{
	(void)type;
	(void)info;
	(void)num;
	return false;
}

void retro_unload_game(void)
{
	if (core != NULL)
	{
		delete core;
		core = NULL;
	}

	rom.clear();
	loaded = false;
}

void *retro_get_memory_data(unsigned id)
{
	if (!loaded || core == NULL)
		return NULL;

	if (id == RETRO_MEMORY_SAVE_RAM)
		return core->savedata_ptr();
	if (id == RETRO_MEMORY_RTC)
		return core->rtcdata_ptr();
	return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
	if (!loaded || core == NULL)
		return 0;

	if (id == RETRO_MEMORY_SAVE_RAM)
		return core->savedata_size();
	if (id == RETRO_MEMORY_RTC)
		return core->rtcdata_size();
	return 0;
}

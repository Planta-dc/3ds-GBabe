/**
 * Gambatte 0.5.0 backend for the 3DS front-end (the default core).
 *
 * Why Gambatte: on the PC it emulates the same ROM in 1.6x-3.4x less time per
 * frame than the Walnut-CGB port this project started from (the numbers and the
 * method are in docs/PORT-NOTES.md). An Old 3DS has 268 MHz and no L2 cache, so
 * the core is the first thing that decides whether a game runs at full speed.
 * Walnut is no longer built: it was removed once Gambatte had been measured on
 * the same ROM, so this is the only backend behind include/emu.h.
 *
 * The core is used through its own C++ API (gambatte.h) rather than through
 * the RetroArch libretro wrapper: that wrapper brings a file/VFS layer, a
 * core-options parser and a band-limited resampler that this front-end does
 * not need, since the video and audio paths here already exist.
 *
 * What this file owns, and the front-end does not:
 *
 *   video   Gambatte renders RGB565 straight into `vbuf` (46 KiB), one full
 *           frame at a time, and emu_frame_buffer() hands it to the front-end,
 *           so scaling and presentation stay in video.c and happen after the
 *           emulation - not before it, which is where the panel wait used to
 *           hide (docs/PORT-NOTES.md section 17).
 *   audio   Gambatte's APU produces 35112 stereo samples per frame at
 *           2097152 Hz. Two 8:1 box-average stages bring that to 32768 Hz
 *           (a second-order CIC decimator: nothing above 16 kHz folds back
 *           loudly, and it costs one add per sample per channel).
 *   saves   .sav next to the ROM, in the same place and format the Walnut
 *           build used, so save data from an older install keeps working, and
 *           <rom>.rtc for MBC3/HuC3 clocks.
 *
 * GPL-2.0: this file is part of a work that links Gambatte, so the whole
 * binary is distributed under GPL-2.0. See third_party/gambatte/README.md.
 */
#include "gb3ds.h"

#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio.h"
#include "emu.h"
#include "log.h"
#include "palette.h"

#include "gambatte.h"
#include "decimator.h"
#include "resamp.h"

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

namespace {

/* Gambatte's InputGetter is polled from inside the emulation. Its bit values
 * are the same as enum gb_button, so the joypad mask passes straight through. */
class Pad : public gambatte::InputGetter
{
public:
	Pad() : mask(0) {}
	unsigned operator()() { return mask; }
	unsigned mask;
};

Pad pad;
gambatte::GB *core;

uint8_t *rom;
size_t   rom_size;

/* Both boot ROMs are optional and not distributed; the file names are the ones
 * GBabe has always used. Gambatte asks for whichever one matches the cartridge. */
uint8_t  bootrom_dmg[0x100];
uint8_t  bootrom_cgb[0x900];
size_t   bootrom_dmg_size;
size_t   bootrom_cgb_size;

char save_path[512];  /* <rom>.sav, cart RAM                            */
char rtc_path[512];   /* <rom>.rtc, MBC3/HuC3 clock                     */

uint32_t savedata_hash; /* last written content, to skip unchanged writes */
bool     sram_dirty;

bool loaded;
bool rendering = true;
emu_info info;

/* Where the core draws. 160*144*2 = 46 KiB, deliberately static: it is touched
 * on every line of every frame. */
gambatte::video_pixel_t vbuf[144 * 160];

/* Grey-box for the error message the HUD can show. */
char error_text[64];
bool error_pending;

/* ------------------------------------------------------------------------- */
/* Audio: 2097152 Hz -> 32768 Hz                                             */
/* ------------------------------------------------------------------------- */

/* Gambatte's native audio rate is 35112 samples per frame, and 35112 / 59.7275
 * = 2097152 Hz exactly. That is 64x the rate the CSND channel is opened at, so
 * the decimation is 8 x 2 x 4: two box filters to get the rate down cheaply and
 * a 32-tap windowed sinc at 131072 Hz to do the anti-alias work where taps cost
 * least. See include/decimator.h for the shape and docs/PORT-NOTES.md 27/29 for
 * what the console measured each version at. */
decimator stage;

/* 35112 stereo samples per frame, in 2064-sample calls (the core's own
 * front-end uses the same step size). */
/* 35112 stereo samples per frame, in 2064-sample calls (the core's own
 * front-end uses the same step size). */
const unsigned RUN_SAMPLES  = 2064;
const unsigned SOUND_BUF    = RUN_SAMPLES + 2064; /* core may overshoot      */
gambatte::uint_least32_t sound_buf[SOUND_BUF];

/* One runFor() call produces at most 4128 samples, which is 64 output frames
 * at 32768 Hz; 72 leaves room for a block that overruns. */
int16_t audio_out[72 * 2];

/* And then the rate matcher. It stretches by up to 20 % when the console is
 * short of time, so 72 frames can become 90: 96 is the round number above
 * that, and resamp_run() is told the real capacity rather than trusting the
 * clamp to keep inside it. */
int16_t audio_play[96 * 2];
resamp   shaper;

/* ------------------------------------------------------------------------- */
/* Save files                                                                */
/* ------------------------------------------------------------------------- */

uint32_t hash_bytes(const uint8_t *data, size_t n)
{
	/* FNV-1a: good enough to answer "did the game write anything?", and far
	 * cheaper than an SD write every few seconds. */
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++)
	{
		h ^= data[i];
		h *= 16777619u;
	}
	return h;
}

void make_sidecar(char *dst, size_t dst_len, const char *rom_path,
		  const char *ext)
{
	gbabe_copy(dst, dst_len, rom_path);

	char *dot = strrchr(dst, '.');
	if (dot != NULL && dot != dst)
		*dot = '\0';

	size_t used = strlen(dst);
	if (used + strlen(ext) + 1 <= dst_len)
		strcat(dst, ext);
}

void load_savedata(void)
{
	void *ram = core->savedata_ptr();
	unsigned size = core->savedata_size();
	if (ram == NULL || size == 0)
		return;

	FILE *f = fopen(save_path, "rb");
	if (f == NULL)
		return;

	size_t got = fread(ram, 1, size, f);
	(void)got; /* a short file keeps the rest zeroed, like a new battery */
	fclose(f);
}

void load_rtc(void)
{
	void *rtc = core->rtcdata_ptr();
	unsigned size = core->rtcdata_size();
	if (rtc == NULL || size == 0)
		return;

	FILE *f = fopen(rtc_path, "rb");
	if (f == NULL)
		return;

	size_t got = fread(rtc, 1, size, f);
	(void)got;
	fclose(f);
}

/* ------------------------------------------------------------------------- */
/* Palette                                                                   */
/* ------------------------------------------------------------------------- */

void apply_palette(void)
{
	if (core == NULL || loaded == false)
		return;

	/* palette_dmg565() is 12 shades: OBJ0 0-3, OBJ1 0-3, BG 0-3. Gambatte
	 * wants them per palette index (BG = 0, sprites 1 and 2).
	 *
	 * The value goes in unchanged: with VIDEO_RGB565 the core stores what it
	 * is given straight into its DMG colour table, which is the same 565
	 * layout palette_dmg565() produces. (The parameter is called rgb32 in the
	 * API, which is only true of the 32-bit pixel build.) */
	const uint16_t *p = palette_dmg565();

	for (int c = 0; c < 4; c++)
	{
		core->setDmgPaletteColor(gambatte::BG_PALETTE,  (unsigned)c,
					 p[8 + c]);
		core->setDmgPaletteColor(gambatte::SP1_PALETTE, (unsigned)c,
					 p[0 + c]);
		core->setDmgPaletteColor(gambatte::SP2_PALETTE, (unsigned)c,
					 p[4 + c]);
	}
}

/* ------------------------------------------------------------------------- */
/* Boot ROM                                                                  */
/* ------------------------------------------------------------------------- */

/* Called by the core while loading a ROM. `isgbc` picks the size: the DMG boot
 * ROM is 256 bytes, the CGB one 2304 (0x900). Return false to use the core's
 * internal power-on state (no boot ROM). */
bool bootloader_getter(void *userdata, bool isgbc, uint8_t *data,
		       uint32_t buf_size)
{
	(void)userdata;

	const size_t want = isgbc ? 0x900 : 0x100;
	const uint8_t *src = isgbc ? bootrom_cgb : bootrom_dmg;
	const size_t have = isgbc ? bootrom_cgb_size : bootrom_dmg_size;

	if (have < want || buf_size < want)
		return false;

	memcpy(data, src, want);
	return true;
}

/* ------------------------------------------------------------------------- */
/* Header parsing, for the HUD                                               */
/* ------------------------------------------------------------------------- */

/* The same checksum the Walnut build used (gb_colour_hash()), so "Auto" picks
 * the same palette it always did for the same cartridge. */
uint8_t colour_hash(void)
{
	uint8_t x = 0;
	for (unsigned i = 0x134; i <= 0x143 && i < rom_size; i++)
		x += rom[i];
	return x;
}

void fill_info(void)
{
	memset(&info, 0, sizeof info);
	if (rom == NULL || rom_size < 0x150)
		return;

	/* Title at 0x134, up to 16 characters, NUL padded. CGB cartridges use
	 * the byte at 0x143 for the CGB flag, so the title is 15 characters
	 * long there; stopping at the first byte outside printable ASCII covers
	 * both. (Stopping only at control characters let 0x80-0xFF through, and
	 * a real cartridge - "Bugs Bunny in Crazy Castle 4" - put a 0xFF in the
	 * padding, which reached the log and the status readout as mojibake.) */
	unsigned n = 0;
	for (unsigned i = 0; i < sizeof info.title - 1; i++)
	{
		uint8_t c = rom[0x134 + i];
		if (c < 0x20 || c >= 0x7F)
			break;
		info.title[n++] = (char)c;
	}
	while (n > 0 && info.title[n - 1] == ' ')
		n--; /* titles are space padded */
	info.title[n] = '\0';

	info.cgb = (rom[0x143] & 0x80) != 0;
	if (core != NULL)
		info.cgb = core->isCgb();

	/* 16 KiB banks. 0x52 and up are unofficial; clamp instead of trusting
	 * the byte with a shift that could overflow. */
	unsigned rom_code = rom[0x148];
	info.rom_banks = (rom_code <= 8) ? (2u << rom_code) : 512u;

	static const uint8_t ram_banks[] = { 0, 1, 1, 4, 16, 8 };
	unsigned ram_code = rom[0x149];
	info.ram_banks = (ram_code < sizeof ram_banks) ? ram_banks[ram_code] : 0;

	/* The HUD prints this as a number; it is the MBC generation. */
	uint8_t type = rom[0x147];
	switch (type)
	{
	case 0x00: info.mbc = 0; break;                       /* plain */
	case 0x08:
	case 0x09: info.mbc = 0; break;                       /* +SRAM */
	case 0x01: case 0x02: case 0x03: info.mbc = 1; break;
	case 0x05: case 0x06: info.mbc = 2; break;
	case 0x0F: case 0x10: case 0x11: case 0x12:
	case 0x13: info.mbc = 3; break;
	case 0x19: case 0x1A: case 0x1B: case 0x1C:
	case 0x1D: case 0x1E: info.mbc = 5; break;
	case 0x20: info.mbc = 6; break;
	case 0x22: info.mbc = 7; break;
	case 0x0B: case 0x0C: case 0x0D: info.mbc = 8; break;  /* MMM01 */
	case 0xFC: info.mbc = 0xFC; break;                     /* Camerica */
	case 0xFD: info.mbc = 0xFD; break;                     /* Bandai */
	case 0xFE: info.mbc = 0xFE; break;                     /* HuC3 */
	case 0xFF: info.mbc = 0xFF; break;                     /* HuC1 */
	default:   info.mbc = type; break;
	}

	info.auto_hash = colour_hash();
}

/* Reads one file into `buf`; returns the number of bytes, or 0. */
size_t read_bootrom(const char *path, uint8_t *buf, size_t buf_size,
		    size_t min_size)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return 0;

	size_t n = fread(buf, 1, buf_size, f);
	fclose(f);

	if (n < min_size)
	{
		log_line("bootrom: %s is %u bytes, ignored (need %u)", path,
			 (unsigned)n, (unsigned)min_size);
		return 0;
	}

	log_line("bootrom: loaded %u bytes from %s", (unsigned)n, path);
	return n;
}

void try_load_bootrom(void)
{
	bootrom_dmg_size = read_bootrom(GBABE_ROOT "/bootrom/dmg_boot.bin",
					bootrom_dmg, sizeof bootrom_dmg, 0x100);
	bootrom_cgb_size = read_bootrom(GBABE_ROOT "/bootrom/cgb_boot.bin",
					bootrom_cgb, sizeof bootrom_cgb, 0x100);
}

} /* namespace */

/* ------------------------------------------------------------------------- */
/* include/emu.h                                                             */
/* ------------------------------------------------------------------------- */

extern "C" const char *emu_last_error(void)
{
	if (!error_pending)
		return NULL;

	error_pending = false;
	return error_text;
}

extern "C" void emu_unload(void)
{
	if (loaded)
		emu_flush_save();

	if (core != NULL)
	{
		delete core;
		core = NULL;
	}

	free(rom);
	rom = NULL;
	rom_size = 0;
	loaded = false;
	rendering = true;
	sram_dirty = false;
	error_pending = false;
	memset(&info, 0, sizeof info);

	/* A new game is new audio: the rate matcher must not carry a fractional
	 * position from the old one. */
	resamp_reset(&shaper);
}

extern "C" const char *emu_load(const char *path)
{
	emu_unload();

	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return "Cannot open ROM.";

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size < 0x150)
	{
		fclose(f);
		return "File is too small to be a ROM.";
	}

	rom_size = (size_t)size;
	rom = (uint8_t *)malloc(rom_size);
	if (rom == NULL)
	{
		fclose(f);
		return "Not enough memory for the ROM.";
	}

	if (fread(rom, 1, rom_size, f) != rom_size)
	{
		fclose(f);
		free(rom);
		rom = NULL;
		rom_size = 0;
		return "Short read while loading the ROM.";
	}
	fclose(f);

	make_sidecar(save_path, sizeof save_path, path, ".sav");
	make_sidecar(rtc_path, sizeof rtc_path, path, ".rtc");
	try_load_bootrom();

	/* The core object is an empty shell; the state it points at is a few
	 * hundred kilobytes and is allocated by load(). */
	core = new gambatte::GB();
	core->setInputGetter(&pad);
	if (bootrom_dmg_size > 0 || bootrom_cgb_size > 0)
		core->setBootloaderGetter(bootloader_getter);

	/* Colour correction is off. It is a CGB-only filter that shifts the
	 * whole palette; with it on, the same ROM would not render the same
	 * colours the reference test ROM expects. */
	core->setColorCorrection(false);

	if (core->load(rom, (unsigned)rom_size, 0) != 0)
	{
		log_line("core: gambatte refused the ROM (%u bytes)",
			 (unsigned)rom_size);
		delete core;
		core = NULL;
		free(rom);
		rom = NULL;
		rom_size = 0;
		return "The core cannot run this cartridge.";
	}

	load_savedata();
	load_rtc();
	sram_dirty = false;

	fill_info();
	palette_set_auto_hash(info.auto_hash);
	loaded = true;
	apply_palette();

	/* Logged once per load: the model, the MBC and the save size are the
	 * first things to look at when a game misbehaves. */
	log_line("core: gambatte 0.5.0, %s, %u rom banks, %u ram banks",
		 info.cgb ? "CGB" : "DMG", info.rom_banks, info.ram_banks);
	log_line("rom: \"%s\"", info.title);
	log_line("rom path: %s", path);
	return NULL;
}

extern "C" bool emu_loaded(void)
{
	return loaded;
}

extern "C" const emu_info *emu_get_info(void)
{
	return &info;
}

extern "C" void emu_set_joypad(uint8_t pressed)
{
	pad.mask = pressed;
}

extern "C" void emu_reset(void)
{
	if (!loaded)
		return;

	emu_flush_save();
	error_pending = false;
	core->reset();

	/* reset() re-runs the power-on sequence, which drops the DMG palette
	 * colours the front-end had pushed in. */
	apply_palette();
}

extern "C" void emu_set_rendering(bool on)
{
	rendering = on;
}

extern "C" void emu_palette_changed(void)
{
	apply_palette();
}

extern "C" void emu_flush_save(void)
{
	if (!loaded || core == NULL)
		return;

	void *ram = core->savedata_ptr();
	unsigned size = core->savedata_size();

	if (ram != NULL && size > 0)
	{
		uint32_t hash = hash_bytes((const uint8_t *)ram, size);
		if (hash != savedata_hash || sram_dirty)
		{
			FILE *f = fopen(save_path, "wb");
			if (f != NULL)
			{
				if (fwrite(ram, 1, size, f) == size)
				{
					savedata_hash = hash;
					sram_dirty = false;
				}
				fclose(f);
			}
		}
	}

	void *rtc = core->rtcdata_ptr();
	unsigned rtc_size = core->rtcdata_size();

	if (rtc != NULL && rtc_size > 0)
	{
		FILE *f = fopen(rtc_path, "wb");
		if (f != NULL)
		{
			fwrite(rtc, 1, rtc_size, f);
			fclose(f);
		}
	}
}

/* Ticks spent inside runFor(), in the decimator, and in handing the result to
 * CSND, since the last emu_timing_take(). All three happen inside
 * emu_run_frame(), so they are parts of what the log calls `core`; the point of
 * splitting them is that they are the things a slow frame can be blamed on, and
 * until they are counted separately none of them can be.
 *
 * The decimator and the push are measured apart because they have completely
 * different fixes: one is arithmetic over 140 KiB of samples, the other is a
 * service call to the DSP. `dec=` was 3.0 ms on the console and nobody could
 * say which half it was. */
static u64 emu_ticks_, audio_ticks_, push_ticks_;

/* CGB palette conversions in this frame. Gambatte converts a 15-bit colour to
 * the framebuffer format every time a CGB palette register is written, and with
 * colour correction on (the default) that is a gamma round trip per call. A
 * fade rewrites all 64 entries repeatedly, which is the only time it can cost
 * anything - and it is the one part of a Game Boy Color frame that a monochrome
 * game does not pay. Counted here, logged as `pal=`. */
extern "C" unsigned long gbabe_cgb_palette_count(void);
static unsigned long palette_mark_;

extern "C" void emu_run_frame(void)
{
	if (!loaded || core == NULL)
		return;

	/* runFor() returns the sample number at which a video frame was drawn,
	 * or -1 when it ran out of samples first. Ask for one frame's worth in
	 * steps and keep whatever audio came out on the way. */
	for (;;)
	{
		unsigned samples = RUN_SAMPLES;
		const u64 t_core = svcGetSystemTick();

		long ret = core->runFor(rendering ? vbuf : NULL, 160,
					sound_buf, SOUND_BUF, samples);
		const u64 t_done = svcGetSystemTick();

		emu_ticks_ += t_done - t_core;

		if (samples > SOUND_BUF)
			samples = SOUND_BUF; /* documented to overshoot by 2064 */

		/* The decimation and the resampling, then the hand-over to CSND
		 * as two separate figures. It only runs when the sound is
		 * actually wanted. */
		if (samples > 0 && audio_enabled())
		{
			const unsigned out = decimator_run(&stage, sound_buf,
							   samples, audio_out);
			unsigned n = 0;

			if (out > 0)
			{
				/* 548.6 samples a frame is only the right
				 * number if the frame rate is right, and it is
				 * not always: stretch or squeeze to follow the
				 * rate the console is actually producing, so the
				 * FIFO stays on target instead of draining into
				 * an underrun (see include/resamp.h). */
				resamp_set_step(&shaper, audio_rate_step());

				n = resamp_run(&shaper, audio_out, out, audio_play,
					       sizeof audio_play /
					       (2 * sizeof audio_play[0]));
			}

			const u64 t_dec = svcGetSystemTick();

			audio_ticks_ += t_dec - t_done;

			/* The FIFO copy and, every other frame, the
			 * csndPlaySound() service call: a different animal
			 * from the arithmetic, and logged apart. */
			if (n > 0)
				audio_push_frames(audio_play, n);

			push_ticks_ += svcGetSystemTick() - t_dec;
		}
		else
			audio_ticks_ += svcGetSystemTick() - t_done;

		if (ret >= 0)
			break;
	}

	/* The picture stays here. Putting it on the panel is the front-end's last
	 * act of the frame (see emu.h): it needs the back framebuffer, and asking
	 * for that 1 ms after the previous swap meant waiting for a vblank that
	 * had not arrived yet - on every frame. */
}

extern "C" const uint16_t *emu_frame_buffer(void)
{
	if (!loaded || !rendering)
		return NULL;

	return (const uint16_t *)vbuf;
}

extern "C" unsigned emu_palette_take(void)
{
	const unsigned long now = gbabe_cgb_palette_count();
	const unsigned long n = now - palette_mark_;

	palette_mark_ = now;
	return (unsigned)n;
}

extern "C" void emu_timing_take(uint64_t *emu_ticks, uint64_t *audio_ticks,
				uint64_t *push_ticks)
{
	if (emu_ticks != NULL)
		*emu_ticks = emu_ticks_;
	if (audio_ticks != NULL)
		*audio_ticks = audio_ticks_;
	if (push_ticks != NULL)
		*push_ticks = push_ticks_;

	emu_ticks_ = audio_ticks_ = push_ticks_ = 0;
}

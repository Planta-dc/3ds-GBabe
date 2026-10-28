/**
 * Sound: the emulated APU (minigb_apu) feeds the 3DS CSND service.
 *
 * Why CSND and not NDSP? NDSP on an Old 3DS needs the DSP firmware dumped to
 * sdmc:/3ds/dspfirm.cdc. CSND needs no such file, which matters a lot on the
 * console this port targets. The trade-off is that we have to feed the hardware
 * ourselves, so this file keeps a small software FIFO:
 *
 *   emulator thread: one frame (548 stereo samples) per emulated frame
 *        -> FIFO -> whenever a hardware block is idle, pop 1024 samples and
 *           queue them as a one-shot block
 *
 * The FIFO absorbs the jitter between the Game Boy's 59.7275 Hz frame clock and
 * the 3DS's 60 Hz vblank; when it runs empty we output silence rather than
 * blocking the emulator.
 *
 * MIT licensed.
 */
#include "audio.h"

#include <3ds.h>

#include <stdlib.h>
#include <string.h>

#include <3ds/allocator/linear.h>

/* minigb_apu needs to know the sample format at compile time. */
#ifndef MINIGB_APU_AUDIO_FORMAT_S16SYS
#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#endif
#include "minigb_apu.h"

/* CSND wants the sample rate as an integer and resamples on its own. */
#define OUT_RATE        AUDIO_SAMPLE_RATE /* 32768 Hz */
#define FIFO_SAMPLES    (AUDIO_SAMPLES * 6) /* ~100 ms of slack */
#define BLOCK_SAMPLES   1024             /* per hardware block, stereo */
#define BLOCK_BYTES     (BLOCK_SAMPLES * 2 * sizeof(s16))
#define CHANNEL         8                /* a CSND channel we own */

static struct minigb_apu_ctx apu;

static s16 *fifo;          /* ring buffer, FIFO_SAMPLES stereo frames */
static unsigned fifo_head, fifo_tail;

static s16 *blocks[2];     /* linear memory, one block each */
static int block_current;
static bool csnd_ok;
static bool enabled = true;

/* ------------------------------------------------------------------------- */

static void fifo_reset(void)
{
	fifo_head = fifo_tail = 0;
}

static unsigned fifo_used(void)
{
	return (fifo_head - fifo_tail + FIFO_SAMPLES) % FIFO_SAMPLES;
}

static void fifo_push(const s16 *src, unsigned frames)
{
	unsigned room = FIFO_SAMPLES - 1 - fifo_used();
	if (frames > room)
		frames = room; /* drop rather than overwrite the backlog */

	while (frames--)
	{
		fifo[fifo_head * 2 + 0] = *src++;
		fifo[fifo_head * 2 + 1] = *src++;
		fifo_head = (fifo_head + 1) % FIFO_SAMPLES;
	}
}

static unsigned fifo_pop(s16 *dst, unsigned frames)
{
	unsigned n = 0;

	while (n < frames && fifo_tail != fifo_head)
	{
		*dst++ = fifo[fifo_tail * 2 + 0];
		*dst++ = fifo[fifo_tail * 2 + 1];
		fifo_tail = (fifo_tail + 1) % FIFO_SAMPLES;
		n++;
	}
	return n;
}

/* Hand a full block to CSND, if the channel is free. */
static void pump(void)
{
	if (!csnd_ok || !enabled)
		return;

	u8 status = 0;
	CSND_UpdateInfo(true);
	if (R_SUCCEEDED(csndIsPlaying(CHANNEL, &status)) && status)
		return; /* still playing the previous block */

	if (fifo_used() < BLOCK_SAMPLES / 2)
		return; /* not enough samples; wait for the next frame */

	s16 *blk = blocks[block_current];
	unsigned got = fifo_pop(blk, BLOCK_SAMPLES);

	if (got < BLOCK_SAMPLES)
		memset(blk + got * 2, 0, (BLOCK_SAMPLES - got) * 2 * sizeof(s16));

	CSND_FlushDataCache(blk, BLOCK_BYTES);

	/* One-shot block, mono->stereo is already handled by minigb_apu (the
	 * APU renders stereo with per-channel L/R enables). */
	if (R_FAILED(csndPlaySound(CHANNEL, SOUND_ONE_SHOT | SOUND_FORMAT_16BIT,
				  OUT_RATE, 1.0f, 0.0f, blk, NULL,
				  BLOCK_BYTES)))
	{
		/* Channel refused (another app owns it?): go quiet for good. */
		csnd_ok = false;
		return;
	}

	block_current ^= 1;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

bool audio_init(void)
{
	/* The FIFO is only touched by the CPU, but the audio blocks are read by
	 * the DSP, so they must live in linear (physically mapped) memory and be
	 * cache-flushed after every write. */
	fifo = malloc(FIFO_SAMPLES * 2 * sizeof(s16));
	blocks[0] = linearAlloc(BLOCK_BYTES);
	blocks[1] = linearAlloc(BLOCK_BYTES);

	if (fifo == NULL || blocks[0] == NULL || blocks[1] == NULL)
	{
		audio_exit();
		return false;
	}
	memset(fifo, 0, FIFO_SAMPLES * 2 * sizeof(s16));
	memset(blocks[0], 0, BLOCK_BYTES);
	memset(blocks[1], 0, BLOCK_BYTES);

	block_current = 0;
	fifo_reset();
	minigb_apu_audio_init(&apu);

	/* Sound is a nice-to-have: if CSND will not start, keep emulating. */
	if (R_FAILED(csndInit()))
	{
		csnd_ok = false;
		return false;
	}

	csnd_ok = true;
	return true;
}

void audio_exit(void)
{
	if (csnd_ok)
	{
		CSND_SetPlayStateR(CHANNEL, 0);
		csndExecCmds(true);
		csndExit();
		csnd_ok = false;
	}

	free(fifo);
	if (blocks[0] != NULL)
		linearFree(blocks[0]);
	if (blocks[1] != NULL)
		linearFree(blocks[1]);
	fifo = NULL;
	blocks[0] = blocks[1] = NULL;
}

void audio_set_enabled(bool on)
{
	enabled = on;
	if (!on && csnd_ok)
	{
		CSND_SetPlayStateR(CHANNEL, 0);
		csndExecCmds(true);
		fifo_reset();
	}
}

bool audio_enabled(void)
{
	return enabled;
}

unsigned audio_queue_ms(void)
{
	return (fifo_used() * 1000u) / OUT_RATE;
}

void audio_frame(void)
{
	if (!enabled || fifo == NULL)
		return;

	/* One Game Boy frame worth of samples, stereo interleaved int16. */
	audio_sample_t samples[AUDIO_SAMPLES_TOTAL];
	minigb_apu_audio_callback(&apu, samples);

	fifo_push((const s16 *)samples, AUDIO_SAMPLES);
	pump();
}

/* ------------------------------------------------------------------------- */
/* Hooks required by the Walnut-CGB core                                     */
/* ------------------------------------------------------------------------- */

uint8_t audio_read(uint16_t addr)
{
	return minigb_apu_audio_read(&apu, addr);
}

void audio_write(uint16_t addr, uint8_t val)
{
	minigb_apu_audio_write(&apu, addr, val);
}

/**
 * Sound: the emulated APU feeds the 3DS CSND service.
 *
 * Why CSND and not NDSP? NDSP on an Old 3DS needs the DSP firmware dumped to
 * sdmc:/3ds/dspfirm.cdc. CSND needs no such file, which matters a lot on the
 * console this port targets. The trade-off is that feeding the hardware is our
 * job, and this file exists mostly to do that properly:
 *
 *   emulator: 548 stereo samples per emulated frame at 32768 Hz
 *        -> software FIFO (4096 frames, ~125 ms of slack)
 *        -> 1024-frame blocks (31.25 ms), submitted just before the previous
 *           block ends
 *
 * A CSND channel is mono. Its 32-bit volume word holds a left and a right
 * volume for the *same* sample, so a stereo stream needs two channels, one per
 * ear, started together. Feeding interleaved L,R data to one channel does not
 * play stereo: the interleaved samples are played as a mono stream, i.e. at
 * twice the pitch, in half the time, so every block also ends early. That is
 * what this file used to do, and it is why the sound was wrong.
 *
 * Slots alternate, so the next block is already playing when the current one
 * runs out. Without a second slot every block boundary is a silence gap of up
 * to one emulated frame - a 32 Hz stutter on top of the pitch problem.
 *
 * The submit time is a deadline, not "whenever a frame ends":
 * audio_next_deadline() tells the frame loop when the next block is due and the
 * loop sleeps to it. The frame loop is idle ~11 ms of every 16.7 ms, so there
 * is plenty of time to be precise. The margin is the measured csndPlaySound()
 * round trip, so the new block starts as the old one ends; being a little early
 * is deliberate, because a short overlap of the same audio is far less audible
 * than a gap.
 *
 * If the FIFO runs dry or we fall far behind, the stream is resynchronised
 * rather than played stale, and counted - those counters are the ones to read
 * when the sound stutters.
 *
 * MIT licensed.
 */
#include "audio.h"

#include <3ds.h>

#include <stdlib.h>
#include <string.h>

#include <3ds/allocator/linear.h>

#include "log.h"
#include "resamp.h"

/* CSND wants the sample rate as an integer and resamples on its own.
 * Gambatte delivers 32768 Hz, which is what the channels are opened at. */
#define OUT_RATE        GBABE_AUDIO_RATE
/* Power of two so the ring buffer wraps with a mask and can be copied in (at
 * most) two runs instead of a sample at a time. ~125 ms of slack. */
#define FIFO_SAMPLES    4096u
#define BLOCK_SAMPLES   1024u   /* frames per hardware block, per ear */
#define BLOCK_BYTES     (BLOCK_SAMPLES * sizeof(s16))

#define SLOTS           2       /* ping-pong: one playing, one being submitted */
#define EARS            2       /* left and right                             */
/* How many blocks have to be queued before the stream starts. This is the slack
 * that absorbs a hiccup: at one block, a single late frame is an underrun. It is
 * also latency, so it is kept small - 62 ms at BLOCK_SAMPLES = 1024. */
#define PREFILL_BLOCKS  2

/* A block lasts this long, in ARM11 ticks. */
#define BLOCK_TICKS     ((u64)((double)BLOCK_SAMPLES * SYSCLOCK_ARM11 / OUT_RATE))
/* How early a block is submitted, relative to the end of the one playing.
 *
 * This is a *phase*, not a rate: the cadence stays exactly one block per
 * BLOCK_TICKS, so it cannot make the stream consume the FIFO faster than the
 * emulator fills it. Being early means the two blocks overlap briefly - the
 * same audio twice for a millisecond - being late means a hole of silence, and
 * a hole every 31 ms is a buzz. So the margin is deliberately biased early and
 * grown to cover the worst wake-up seen recently, which is measured margin-free
 * from the interval between submissions. */
#define MARGIN_MIN      (SYSCLOCK_ARM11 / 10000) /* 0.1 ms */
#define MARGIN_MAX      (SYSCLOCK_ARM11 / 200)   /* 5 ms   */
#define MARGIN_BIAS     (SYSCLOCK_ARM11 / 2000)  /* 0.5 ms */

static s16 *fifo;          /* ring buffer, FIFO_SAMPLES stereo frames */
static unsigned fifo_head, fifo_tail;

/* One linear (DSP-visible) buffer per slot and ear, and the channel it plays
 * on. In mono mode only ear 0 is allocated and used. */
static s16 *blocks[SLOTS][EARS];
static int channels[SLOTS][EARS];
static bool stereo = true;
static bool csnd_ok;
static bool enabled = true;

/* The stream: the nominal start of the next block, and which slot it fills. */
static u64 next_start;
static bool scheduled;
static int slot_next;

/* Smoothed csndPlaySound() round trip, smoothed lateness of the frame loop, and
 * the sum of the two - the amount by which a block is submitted early. */
static u64 cmd_ticks;   /* measured csndPlaySound() round trip   */
static u64 late_max;    /* decaying worst hole, in ticks          */
static u64 margin;      /* cmd_ticks + late_max + MARGIN_BIAS     */

/* Previous submission, for the interval that shows whether this one was early
 * or late. */
static u64 last_submit;
static bool have_last;

/* Staging area for the frame that comes out of the FIFO, interleaved. */
static s16 staged[BLOCK_SAMPLES * 2];

/* Counters. Cumulative, so a report says what happened over the session. */
static unsigned stat_blocks;
static unsigned stat_pairs;
static unsigned stat_underruns;
static unsigned stat_resyncs;
static unsigned stat_dropped;
static unsigned stat_late_blocks;
static unsigned stat_stalled;      /* blocks over half a second late      */
static s64      stat_worst_late;   /* ticks, negative = early */

/* ------------------------------------------------------------------------- */
/* Queue                                                                     */
/* ------------------------------------------------------------------------- */

static void fifo_reset(void)
{
	fifo_head = fifo_tail = 0;
}

static unsigned fifo_used(void)
{
	return (fifo_head - fifo_tail) & (FIFO_SAMPLES - 1);
}

static void fifo_push(const s16 *src, unsigned frames)
{
	const unsigned room = FIFO_SAMPLES - 1 - fifo_used();
	if (frames > room)
	{
		/* Drop the newest samples rather than overwrite the backlog: the
		 * stream stays contiguous, which is what the ear notices. */
		stat_dropped += frames - room;
		frames = room;
	}

	unsigned first = FIFO_SAMPLES - fifo_head;
	if (first > frames)
		first = frames;

	memcpy(&fifo[fifo_head * 2], src, (size_t)first * 2 * sizeof(s16));
	if (frames > first)
		memcpy(fifo, src + (size_t)first * 2,
		       (size_t)(frames - first) * 2 * sizeof(s16));

	fifo_head = (fifo_head + frames) & (FIFO_SAMPLES - 1);
}

static unsigned fifo_pop(s16 *dst, unsigned frames)
{
	unsigned n = fifo_used();
	if (frames < n)
		n = frames;

	if (n == 0)
		return 0;

	unsigned first = FIFO_SAMPLES - fifo_tail;
	if (first > n)
		first = n;

	memcpy(dst, &fifo[fifo_tail * 2], (size_t)first * 2 * sizeof(s16));
	if (n > first)
		memcpy(dst + (size_t)first * 2, fifo,
		       (size_t)(n - first) * 2 * sizeof(s16));

	fifo_tail = (fifo_tail + n) & (FIFO_SAMPLES - 1);
	return n;
}

/* ------------------------------------------------------------------------- */
/* CSND                                                                      */
/* ------------------------------------------------------------------------- */

static void stop_all(void)
{
	if (!csnd_ok)
		return;

	for (int s = 0; s < SLOTS; s++)
		for (int e = 0; e < (stereo ? EARS : 1); e++)
			CSND_SetPlayStateR(channels[s][e], 0);
	csndExecCmds(true);
	scheduled = false;
}

/* Take the highest `count` channels the service has given us. High numbers are
 * the least likely to be in use by anything else. */
static bool pick_channels(int count, int *out)
{
	int found = 0;

	for (int ch = CSND_NUM_CHANNELS - 1; ch >= 0 && found < count; ch--)
		if (csndChannels & BIT(ch))
			out[found++] = ch;

	return found == count;
}

static u64 smooth(u64 old, u64 sample)
{
	return old ? (old * 7 + sample) / 8 : sample;
}

static void update_margin(void)
{
	margin = cmd_ticks + late_max + MARGIN_BIAS;
	if (margin < MARGIN_MIN)
		margin = MARGIN_MIN;
	if (margin > MARGIN_MAX)
		margin = MARGIN_MAX;
}

/* Pop one block from the FIFO, split it into per-ear mono buffers and start
 * both ears. The caller has already made sure BLOCK_SAMPLES are queued. */
static void start_block(void)
{
	const int slot = slot_next;
	const int ears = stereo ? EARS : 1;
	const unsigned got = fifo_pop(staged, BLOCK_SAMPLES);

	for (int e = 0; e < ears; e++)
	{
		s16 *dst = blocks[slot][e];

		if (stereo)
		{
			/* Deinterleave: one channel per ear. */
			for (unsigned i = 0; i < got; i++)
				dst[i] = staged[i * 2 + e];
		}
		else
		{
			/* One channel: average the ears, which is what a mono
			 * speaker would do with them anyway. */
			for (unsigned i = 0; i < got; i++)
				dst[i] = (s16)(((int)staged[i * 2] +
						(int)staged[i * 2 + 1]) / 2);
		}

		if (got < BLOCK_SAMPLES)
			memset(dst + got, 0, (BLOCK_SAMPLES - got) * sizeof(s16));
	}

	for (int e = 0; e < ears; e++)
	{
		/* The DSP reads this buffer: flush it before it is visible. */
		CSND_FlushDataCache(blocks[slot][e], BLOCK_BYTES);

		const u64 t0 = svcGetSystemTick();
		Result rc = csndPlaySound(channels[slot][e],
					  SOUND_ONE_SHOT | SOUND_FORMAT_16BIT,
					  OUT_RATE, 1.0f, 0.0f,
					  blocks[slot][e], NULL, BLOCK_BYTES);
		cmd_ticks = smooth(cmd_ticks, svcGetSystemTick() - t0);
		update_margin();

		if (R_FAILED(rc))
		{
			/* Channel refused (another app owns it?): go quiet for
			 * good rather than leaving a half-started stream. */
			log_line("audio: channel %d refused, running silent",
				 channels[slot][e]);
			csnd_ok = false;
			scheduled = false;
			return;
		}
	}

	slot_next ^= 1;
	stat_blocks++;
	if (stereo)
		stat_pairs++;
}

/* Frames that have to be queued before a block can go out: one block to play
 * with, or the whole prefill when the stream is not running. */
static unsigned need_frames(void)
{
	return scheduled ? BLOCK_SAMPLES : PREFILL_BLOCKS * BLOCK_SAMPLES;
}

void audio_pump(void)
{
	if (!csnd_ok || !enabled || fifo == NULL)
		return;

	const u64 now = svcGetSystemTick();

	if (scheduled)
	{
		const u64 due = (next_start > margin) ? next_start - margin : 1;
		if ((s64)(now - due) < 0)
			return; /* not time yet */
	}

	if (fifo_used() < need_frames())
	{
		if (scheduled)
		{
			/* The emulator did not fill the FIFO in time. Nothing can
			 * be done about the hole, so restart cleanly from the
			 * prefill level instead of limping. */
			stat_underruns++;
			scheduled = false;
		}
		return;
	}

	/* How far from the ideal one-block cadence this submission is. Positive
	 * means late, i.e. the previous block has already run out: a hole.
	 * Measured from the last submission, not from the deadline, so it does
	 * not feed back into the margin it is used to compute. */
	if (have_last)
	{
		const s64 drift = (s64)(now - last_submit) - (s64)BLOCK_TICKS;

		if (drift > 0)
		{
			stat_late_blocks++;

			/* A gap longer than half a second is not a late block,
			 * it is the application having been somewhere else (an
			 * SD write, a menu, a stalled kernel thread) - and the
			 * log used to carry "worst 56344.07 ms" for exactly
			 * that, a number that says nothing about scheduling.
			 * Reported separately and left out of the worst case. */
			if (drift > (s64)(SYSCLOCK_ARM11 / 2))
				stat_stalled++;
			else if ((u64)drift > (u64)stat_worst_late)
				stat_worst_late = drift;
			if ((u64)drift > late_max)
				late_max = (u64)drift;
		}
		else
		{
			/* No hole: forget the worst case slowly, so the margin
			 * settles just early enough to stay gapless. */
			late_max -= late_max / 64;
		}
		update_margin();
	}

	if (!scheduled)
		next_start = now; /* starts as soon as it is given to CSND */

	start_block();
	last_submit = now;
	have_last = true;

	if (!csnd_ok) /* start_block() clears this if the channel refused */
		return;

	scheduled = true;
	next_start += BLOCK_TICKS;
}

uint64_t audio_next_deadline(void)
{
	if (!csnd_ok || !enabled || fifo == NULL)
		return 0;
	if (fifo_used() < need_frames())
		return 0; /* nothing to submit yet: the next frame will bring more */

	if (!scheduled)
		return 1; /* start as soon as this is called */

	return (next_start > margin) ? next_start - margin : 1;
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
	if (fifo == NULL)
		return false;
	memset(fifo, 0, FIFO_SAMPLES * 2 * sizeof(s16));
	fifo_reset();

	csnd_ok = false;
	scheduled = false;
	slot_next = 0;
	cmd_ticks = late_max = margin = 0;
	last_submit = 0;
	have_last = false;

	/* Sound is a nice-to-have: if CSND will not start, keep emulating. */
	Result rc = csndInit();
	if (R_FAILED(rc))
	{
		log_line("audio: csndInit failed (0x%08lX), running silent",
			 (unsigned long)rc);
		free(fifo);
		fifo = NULL;
		return false;
	}

	/* Stereo needs two channels per slot. If the service will not give us
	 * four, fall back to one channel per slot and mix down to mono: wrong
	 * stereo image, right pitch and right timing. */
	int ch[SLOTS * EARS];
	stereo = pick_channels(SLOTS * EARS, ch);

	if (!stereo && !pick_channels(SLOTS, ch))
	{
		log_line("audio: only %d CSND channels available, running silent",
			 __builtin_popcount(csndChannels));
		csndExit();
		free(fifo);
		fifo = NULL;
		return false;
	}

	const int ears = stereo ? EARS : 1;
	for (int s = 0; s < SLOTS; s++)
	{
		for (int e = 0; e < ears; e++)
		{
			channels[s][e] = ch[s * ears + e];
			blocks[s][e] = linearAlloc(BLOCK_BYTES);
			if (blocks[s][e] == NULL)
			{
				log_line("audio: linearAlloc failed, running silent");
				audio_exit();
				return false;
			}
			memset(blocks[s][e], 0, BLOCK_BYTES);
		}
	}

	csnd_ok = true;
	next_start = svcGetSystemTick();
	(void)audio_rate_step(); /* clears the smoothed level for the new stream */

	log_line("audio: %u Hz, %s, %d ms blocks, channels %d/%d/%d/%d",
		 OUT_RATE, stereo ? "stereo" : "mono (not enough channels)",
		 (int)(BLOCK_SAMPLES * 1000u / OUT_RATE),
		 channels[0][0], channels[1][0],
		 stereo ? channels[0][1] : -1, stereo ? channels[1][1] : -1);
	return true;
}

void audio_exit(void)
{
	stop_all();

	if (csnd_ok)
	{
		csndExit();
		csnd_ok = false;
	}

	for (int s = 0; s < SLOTS; s++)
		for (int e = 0; e < EARS; e++)
		{
			if (blocks[s][e] != NULL)
				linearFree(blocks[s][e]);
			blocks[s][e] = NULL;
		}

	free(fifo);
	fifo = NULL;
}

void audio_set_enabled(bool on)
{
	enabled = on;
	if (!on)
	{
		stop_all();
		fifo_reset();
	}
}

bool audio_enabled(void)
{
	return enabled;
}

unsigned audio_queue_ms(void)
{
	if (fifo == NULL)
		return 0;
	return (fifo_used() * 1000u) / OUT_RATE;
}

/* See include/audio.h and include/resamp.h. The queue sawtooths by a whole
 * block - it drains for 31 ms and is then topped up with 1024 frames - so the
 * control law is given a slow average of the level, not the instant one.
 * 1/64 per call, and the emulator pushes 17 times a frame, is a time constant
 * of about four frames. */
int32_t audio_rate_step(void)
{
	static int32_t fill_ema = -1;

	if (!enabled || !csnd_ok || fifo == NULL)
	{
		fill_ema = -1;
		return RESAMP_UNITY;
	}

	const int32_t fill = (int32_t)fifo_used();

	fill_ema = (fill_ema < 0) ? fill : (fill_ema * 63 + fill) / 64;

	return resamp_control_step(fill_ema,
				   (int32_t)(BLOCK_SAMPLES * PREFILL_BLOCKS));
}

/* Hand the APU's output to the hardware. `frames` stereo frames at OUT_RATE,
 * interleaved int16. The backend calls this once per emulated frame; the FIFO
 * absorbs the difference between 59.7275 and 60 Hz. */
void audio_push_frames(const int16_t *stereo_frames, unsigned frames)
{
	if (!enabled || fifo == NULL || stereo_frames == NULL || frames == 0)
		return;

	fifo_push((const s16 *)stereo_frames, frames);
	audio_pump();
}

void audio_get_stats(audio_stats *out)
{
	if (out == NULL)
		return;

	out->blocks = stat_blocks;
	out->ear_pairs = stat_pairs;
	out->underruns = stat_underruns;
	out->resyncs = stat_resyncs;
	out->dropped = stat_dropped;
	out->late_blocks = stat_late_blocks;
	out->margin_us = (int)(margin * 1000000ull / (u64)SYSCLOCK_ARM11);
	out->worst_late_us = (stat_worst_late == 0) ? 0 :
		(int)(stat_worst_late * 1000000ll / (s64)SYSCLOCK_ARM11);
	out->queue_ms = audio_queue_ms();
}

/* Logged every few seconds rather than every second: it is diagnostic, and it
 * is the only way to see on a console whether the sound path is keeping up. */
void audio_log_stats(void)
{
	static u64 last_log;
	static unsigned last_blocks, last_underruns, last_resyncs, last_dropped;

	if (!csnd_ok)
		return;

	const u64 now = svcGetSystemTick();
	if (last_log != 0 && now - last_log < SYSCLOCK_ARM11 * 5)
		return;
	if (last_log != 0 && stat_blocks == last_blocks &&
	    stat_underruns == last_underruns && stat_resyncs == last_resyncs &&
	    stat_dropped == last_dropped)
		return; /* nothing happened */

	last_log = now;
	last_blocks = stat_blocks;
	last_underruns = stat_underruns;
	last_resyncs = stat_resyncs;
	last_dropped = stat_dropped;

	audio_stats st;
	audio_get_stats(&st);

	/* Once, not twice: the step carries the smoothed queue level, so asking
	 * it twice in one expression advances it twice. */
	const int32_t step = audio_rate_step();

	log_line("audio: %u blocks, late %u (worst %.2f ms), stalled %u, "
		 "underruns %u, resyncs %u, dropped %u, queue %u ms, "
		 "margin %.2f ms, rate %u.%03u",
		 st.blocks, st.late_blocks, st.worst_late_us / 1000.0,
		 stat_stalled, st.underruns, st.resyncs, st.dropped,
		 st.queue_ms, st.margin_us / 1000.0,
		 (unsigned)(step / 65536),
		 (unsigned)(((step % 65536) * 1000) / 65536));
}

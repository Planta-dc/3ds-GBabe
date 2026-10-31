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

#include "gb3ds.h"
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

/* The level the control law holds the queue at, in frames.
 *
 * Not PREFILL_BLOCKS * BLOCK_SAMPLES: that is the level at the instant the
 * stream starts, and it is the *peak* of the sawtooth, not its average. Each
 * submission takes a whole block out of the FIFO, which then refills over the
 * next 31 ms, so the level swings between (peak - BLOCK) and peak and its mean
 * sits half a block below the peak. Asking for the peak as a mean is a request
 * the scheduler can never satisfy: the trim stays on, pulling the rate about
 * 1 % low forever - a permanent flat pitch on a console running at exactly the
 * right speed. (Measured on the host: step 0.994 where it should be 1.000.) */
#define LEVEL_TARGET   ((int32_t)(PREFILL_BLOCKS * BLOCK_SAMPLES) - \
			(int32_t)(BLOCK_SAMPLES / 2))

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
/* The margin is not free, and the comment above is only half the story: CSND
 * has no scheduled start, so submitting a block early does not queue it, it
 * *plays it* - the two blocks are mixed for `margin` worth of audio. That is a
 * comb filter at the block rate, and at the 5 ms this used to grow to it was
 * 16 % of every block doubled, 32 times a second. The console log shows
 * the margin pinned at exactly that ceiling for a whole session: every seam,
 * every time.
 *
 * So the ceiling is 2 ms - 6 % of a block, against 16 % before. Being later
 * than the margin is a hole instead, which is what the poll-and-pump in main.c's
 * present exists to prevent: the block is submitted from inside the panel wait
 * as well as from the emulator, so the deadline is hit within a poll (0.2 ms)
 * rather than within a frame. 1 ms was tried and is too tight: at 95 % speed the
 * stream cannot stay ahead and the rate control starts hunting (measured wobble
 * went from 0.03 % to 3.9 % rms), so the margin is not only an overlap budget -
 * it is the slack that lets a submission land after a poll without becoming a
 * hole. */
#define MARGIN_MIN      (SYSCLOCK_ARM11 / 10000) /* 0.1 ms */
#define MARGIN_MAX      (SYSCLOCK_ARM11 / 500)   /* 2 ms   */
#define MARGIN_BIAS     (SYSCLOCK_ARM11 / 4000)  /* 0.25 ms */

/* Lateness beyond this is not lateness, it is the console having been somewhere
 * else, and it is not allowed to raise the margin. 4 ms is twice the ceiling, so
 * anything that would pin the margin is discarded rather than tracked: a hitch
 * that big leaves a hole whatever the margin is, and paying for it with a 2 ms
 * overlap on every seam for the next minute is the wrong trade. */
#define MARGIN_TRACK_MAX (SYSCLOCK_ARM11 / 250)  /* 4 ms */

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

/* Production-rate measurement: how many frames the emulator is actually ready
 * to hand over per wall-clock second. 32768 is the answer on a console running
 * at 59.7275 fps; anything else is a console that cannot quite keep up, and the
 * resampler has to follow the game rather than the nominal rate. Counted in
 * input frames (the caller's frames are stretched by `last_step`, so they are
 * converted back before being counted), and smoothed over a couple of windows
 * with the same 7/8 filter as the service call timing. */
#define RATE_WINDOW (SYSCLOCK_ARM11 / 4) /* 0.25 s */
/* Longer than any frame the loop intends to take: a gap this big is a stall,
 * not a frame rate. See measure_rate(). */
#define RATE_STALL  (SYSCLOCK_ARM11 / 10) /* 100 ms */
static u64      rate_start;
static u64      rate_last;   /* previous call: one per emulated frame     */
static unsigned rate_frames;
static int64_t  rate_frac;   /* 16.16 leftover from the conversion below */
static int32_t  rate_q16 = RESAMP_UNITY;
static int32_t  last_step = RESAMP_UNITY;
static bool     stream_dropped;

/* Staging area for the frame that comes out of the FIFO, interleaved. */
static s16 staged[BLOCK_SAMPLES * 2];

/* Counters. Cumulative, so a report says what happened over the session. */
static unsigned stat_blocks;
static unsigned stat_pairs;
static unsigned stat_underruns;
static unsigned stat_resyncs;
static unsigned stat_dropped;
static unsigned stat_late_blocks;
static unsigned stat_holes;        /* blocks that started after their turn */
static u64      stat_hole_ticks;   /* the silence they left, in total      */
static u64      stat_worst_hole;   /* the worst of it                      */
static unsigned stat_overlaps;     /* blocks that started before it        */
static u64      stat_overlap_ticks;/* the doubling that cost              */
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

	/* Flush all buffers before they are visible to the DSP. */
	for (int e = 0; e < ears; e++)
		CSND_FlushDataCache(blocks[slot][e], BLOCK_BYTES);

	/* Start both ears together to avoid stereo image skew.
	 * csndPlaySound() calls csndExecCmds(true) for each channel, which
	 * means the second ear starts one IPC round-trip after the first.
	 * Instead, set up both channels' registers manually and call
	 * csndExecCmds(true) once, so both ears start simultaneously. */
	const u64 t0 = svcGetSystemTick();
	bool any_fail = false;

	for (int e = 0; e < ears; e++)
	{
		/* Pan: left channel to left speaker (-1.0), right to right (+1.0).
		 * A CSND channel is mono but has separate left/right volumes, so
		 * pan steers it to the correct ear. 0.0 for both would mix both
		 * channels into both speakers. */
		const float pan = stereo ? (e == 0 ? -1.0f : 1.0f) : 0.0f;

		/* Replicate csndPlaySound() body for SOUND_ONE_SHOT | SOUND_FORMAT_16BIT */
		const u32 phys_addr = osConvertVirtToPhys(blocks[slot][e]);

		/* Set channel registers: channel, flags, buffer addresses, size */
		CSND_SetChnRegs(channels[slot][e],
				SOUND_ONE_SHOT | SOUND_FORMAT_16BIT,
				phys_addr, 0, BLOCK_BYTES);

		/* Set volume with pan: channel, left/right volumes, capture volumes */
		CSND_SetVol(channels[slot][e], CSND_VOL(1.0f, pan), 0);

		/* Start the channel */
		CSND_SetPlayState(channels[slot][e], 1);

		/* Check if the channel was accepted (it might be in use) */
		if (!(csndChannels & BIT(channels[slot][e])))
		{
			log_line("audio: channel %d refused, running silent",
				 channels[slot][e]);
			any_fail = true;
		}
	}

	/* Submit all commands at once so both ears start together */
	csndExecCmds(true);

	cmd_ticks = smooth(cmd_ticks, svcGetSystemTick() - t0);
	update_margin();

	if (any_fail)
	{
		csnd_ok = false;
		scheduled = false;
		return;
	}

	slot_next ^= 1;
	stat_blocks++;
	if (stereo)
		stat_pairs++;

	/* A stream that was dropped by the underrun path and is starting again
	 * is a resync, not a continuation: that is what the counter is for, and
	 * until now nothing ever incremented it. */
	if (stream_dropped)
	{
		stat_resyncs++;
		stream_dropped = false;
	}
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
			stream_dropped = true;
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
			/* Only scheduling jitter goes into the margin. A frame
			 * loop that was away for 30 ms - an SD write, the menu,
			 * a stalled kernel thread - is not something a margin
			 * can cover (the ceiling is MARGIN_MAX, 2 ms), so
			 * feeding it is a pure loss: it pins the margin at the
			 * ceiling and every seam in the session then plays 2 ms
			 * of audio twice. The console log showed exactly that:
			 * `margin 2.00` in every audio report of a session
			 * but the first, because every second had one 33 ms
			 * frame. */
			if ((u64)drift < (u64)MARGIN_TRACK_MAX &&
			    (u64)drift > late_max)
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
	else
	{
		/* The seam, which is what the ear hears. `next_start` is when
		 * this block should begin, i.e. when the one before it ends;
		 * CSND has no scheduled start, so submitting before that
		 * moment plays the two blocks on top of each other and
		 * submitting after it leaves silence. Both are single figures
		 * here rather than something to be inferred from `late`,
		 * which is the *difference* between consecutive overshoots and
		 * cancels itself out: a block 8 ms late followed by one 8 ms
		 * early is one hole, not two late blocks.
		 *
		 * A centisecond either way is a stopwatch artefact (a stalled
		 * kernel thread, the app being suspended), not a seam. */
		const s64 seam = (s64)now - (s64)next_start;
		if (seam > 0)
		{
			if ((u64)seam < SYSCLOCK_ARM11 / 100)
			{
				stat_holes++;
				stat_hole_ticks += (u64)seam;
				if ((u64)seam > stat_worst_hole)
					stat_worst_hole = (u64)seam;
			}
		}
		else if ((u64)-seam < SYSCLOCK_ARM11 / 100)
		{
			stat_overlaps++;
			stat_overlap_ticks += (u64)-seam;
		}
	}

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
	stat_blocks = stat_pairs = stat_underruns = 0;
	stat_resyncs = stat_dropped = stat_late_blocks = 0;
	stat_holes = stat_overlaps = 0;
	stat_hole_ticks = stat_overlap_ticks = stat_worst_hole = 0;
	stat_stalled = 0;
	stat_worst_late = 0;

	rate_start = 0;
	rate_last = 0;
	rate_frames = 0;
	rate_frac = 0;
	rate_q16 = RESAMP_UNITY;
	last_step = RESAMP_UNITY;
	stream_dropped = false;

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
	 * four, or if mono mode is requested for performance, fall back to one
	 * channel per slot and mix down to mono: wrong stereo image, right pitch
	 * and right timing. */
	int ch[SLOTS * EARS];
	
	/* Check if mono is forced in config for performance */
	bool want_stereo = !g_cfg.audio_mono;
	stereo = want_stereo && pick_channels(SLOTS * EARS, ch);

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

/* Frames per second the emulator is producing, in 16.16 - the feedforward term
 * of the control law. One window is 0.25 s, which is 15 emulated frames and
 * about eight sound blocks: long enough that the per-frame push pattern (548
 * and 549 samples alternately) is averaged out, short enough that a game which
 * starts to struggle is corrected within a fraction of a second. */
static void measure_rate(u64 now)
{
	const u64 since = (rate_last != 0) ? now - rate_last : 0;

	rate_last = now;

	/* A frame that took longer than this was not a frame: the console was
	 * in the menu, writing the log to the card, or the thread was stalled.
	 * That time must not go into the measurement, because the measurement
	 * is "how fast does this console produce samples", and a window that
	 * contains a 100 ms stall answers 0.6 - which the control law then
	 * applies as a fifth-of-an-octave pitch drop for the next half second.
	 * The console log's `rate 0.800` entries are exactly that: all six of
	 * them are in the reports that follow a stall or a resync. Throw the
	 * window away and start again instead. */
	if (since > RATE_STALL)
	{
		rate_start = now;
		rate_frames = 0;
		return;
	}

	if (rate_start == 0)
	{
		rate_start = now;
		rate_frames = 0;
		return;
	}

	const u64 span = now - rate_start;
	if (span < RATE_WINDOW)
		return;

	/* A window holds at most a second of samples, so the 16.16 product fits
	 * in 64 bits with room to spare. */
	const int64_t hz_q16 = ((int64_t)rate_frames << 16) *
			       (int64_t)SYSCLOCK_ARM11 / (int64_t)span;

	/* step = produced rate / OUT_RATE, in 16.16 - the units resamp_run()
	 * wants (input frames per output frame). */
	/* One window of 0.25 s holds ~8200 frames, so the measurement itself
	 * is good to a few parts in ten thousand: the smoothing only has to
	 * take the edge off the frame pattern, not average out noise. Half-and-
	 * half is a time constant of about 0.35 s, which is quick enough that a
	 * game dropping from full speed to 57 fps is corrected before the
	 * player notices, and slow enough not to chase anything. */
	rate_q16 = (rate_q16 + (int32_t)(hz_q16 / (int64_t)OUT_RATE)) / 2;
	rate_start = now;
	rate_frames = 0;
}

/* See include/audio.h and include/resamp.h. Two different signals, on purpose:
 *
 *   feedforward  the measured production rate, so a console at 57 fps runs the
 *                resampler at 0.954 straight away instead of hunting for it;
 *   feedback     a heavily smoothed queue level, 1/256 per call, which at 17
 *                calls a frame is a time constant of ~15 frames - long enough
 *                that the block sawtooth is 2 % of what it is in the level.
 *
 * The old version fed the *level* to the law with a constant of 1/64 per call
 * and got the sawtooth straight through: the closed loop had as much gain at the
 * block rate as the rate error it was chasing, so it sat on its clamps and
 * wobbled the pitch by up to 5 % on a console that was running perfectly. */
/* Smoothed queue level, in frames. File scope so the log can print it. */
static int32_t fill_ema = -1;

static int32_t fill_ema_level(void)
{
	return (fill_ema < 0) ? 0 : fill_ema;
}

int32_t audio_rate_step(void)
{
	if (!enabled || !csnd_ok || fifo == NULL)
	{
		fill_ema = -1;
		rate_start = 0;
		rate_frames = 0;
		rate_frac = 0;
		rate_q16 = RESAMP_UNITY;
		last_step = RESAMP_UNITY;
		return RESAMP_UNITY;
	}

	measure_rate(svcGetSystemTick());

	const int32_t fill = (int32_t)fifo_used();

	/* While the stream is stopped - after an underrun, before the prefill
	 * is back - the queue is empty because nothing is playing, not because
	 * the emulator is slow. Letting the 1/256 filter crawl up from zero
	 * made the control law see a starving queue for a second after every
	 * resync and answer with the 20 % squeeze: `rate 0.800` in the console
	 * log, a second of audio a fifth too fast, fourteen times a session.
	 * Follow the queue exactly while stopped instead, so the law is right
	 * the moment the stream starts again. */
	if (!scheduled)
		fill_ema = fill;
	else
		fill_ema = (fill_ema < 0) ? fill : fill_ema + (fill - fill_ema) / 256;

	last_step = resamp_control_step(fill_ema, LEVEL_TARGET, rate_q16);
	return last_step;
}

/* The step currently in force, for the log: reading it must not advance any of
 * the filters above. */
int32_t audio_rate_current(void)
{
	return last_step;
}

/* Hand the APU's output to the hardware. `frames` stereo frames at OUT_RATE,
 * interleaved int16. The backend calls this once per emulated frame; the FIFO
 * absorbs the difference between 59.7275 and 60 Hz. */
void audio_push_frames(const int16_t *stereo_frames, unsigned frames)
{
	if (!enabled || fifo == NULL || stereo_frames == NULL || frames == 0)
		return;

	/* The rate that matters is the emulator's, not the FIFO's: these frames
	 * have already been stretched by `last_step`, so undo that to count what
	 * the decimator produced. The remainder is carried, deliberately: a chunk
	 * is ~32 frames, so truncating each conversion throws away up to a whole
	 * frame per chunk, and thirty of those are 3 % - the loop then measures
	 * itself as slow, corrects itself as slow, and settles at a permanent
	 * pitch error. (Measured on the host before this was fixed: a console at
	 * exactly 59.7275 fps held a step of 0.987 and a 2 % wobble.) */
	rate_frac += (int64_t)frames * last_step;
	rate_frames += (unsigned)(rate_frac >> 16);
	rate_frac &= 0xFFFF;

	fifo_push((const s16 *)stereo_frames, frames);
	/* Note: audio_pump() is now called only from the idle loop in main.c,
	 * not from here. This keeps the CSND IPC out of the frame path.
	 * See docs/PORT-NOTES.md section 20. */
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
	out->holes = stat_holes;
	out->hole_ms = (int)(stat_hole_ticks * 1000ull / (u64)SYSCLOCK_ARM11);
	out->worst_hole_ms =
		(int)(stat_worst_hole * 1000ull / (u64)SYSCLOCK_ARM11);
	out->overlap_us = (int)(stat_overlap_ticks /
				(stat_overlaps ? stat_overlaps : 1) *
				1000000ull / (u64)SYSCLOCK_ARM11);
	out->margin_us = (int)(margin * 1000000ull / (u64)SYSCLOCK_ARM11);
	/* The margin has three parts and only one of them is a choice: the
	 * round trip of csndPlaySound() (cmd_us), the worst recent scheduling
	 * lateness (track_us) and the 0.25 ms bias. The console log showed the
	 * total pinned at the ceiling for a whole session, which could be read
	 * as "the margin is too greedy" - it was not, or not only. This line
	 * says which part it is. */
	out->cmd_us = (int)(cmd_ticks * 1000000ull / (u64)SYSCLOCK_ARM11);
	out->track_us = (int)(late_max * 1000000ull / (u64)SYSCLOCK_ARM11);
	out->worst_late_us = (stat_worst_late == 0) ? 0 :
		(int)(stat_worst_late * 1000000ll / (s64)SYSCLOCK_ARM11);
	out->queue_ms = audio_queue_ms();
	out->rate_meas_q16 = rate_q16;
	out->fill_frames = (int)fill_ema_level();
	out->fill_target = LEVEL_TARGET;
}

/* Logged every few seconds rather than every second: it is diagnostic, and it
 * is the only way to see on a console whether the sound path is keeping up. */
void audio_log_stats(void)
{
	static u64 last_log;
	static unsigned last_blocks, last_underruns, last_resyncs, last_dropped;
	static unsigned last_holes;

	if (!csnd_ok)
		return;

	const u64 now = svcGetSystemTick();
	if (last_log != 0 && now - last_log < SYSCLOCK_ARM11 * 5)
		return;
	if (last_log != 0 && stat_blocks == last_blocks &&
	    stat_underruns == last_underruns && stat_resyncs == last_resyncs &&
	    stat_dropped == last_dropped && stat_holes == last_holes)
		return; /* nothing happened */

	last_log = now;
	last_blocks = stat_blocks;
	last_underruns = stat_underruns;
	last_resyncs = stat_resyncs;
	last_dropped = stat_dropped;
	last_holes = stat_holes;

	audio_stats st;
	audio_get_stats(&st);

	/* Read, do not ask: audio_rate_step() advances the rate window and the
	 * level filter, and a log line must not be part of the control loop. */
	const int32_t step = audio_rate_current();

	/* `produced` is the number to read first: it is the emulator's own
	 * production rate, measured, against the 32768 Hz the channel is opened
	 * at. 1.000 means the console is keeping up exactly and nothing is being
	 * resampled; 0.954 means a console running at 57 fps, and the sound being
	 * stretched to match. The two go together with `rate`: `rate` should
	 * equal `produced`, and either one away from 1.000 on a full-speed game
	 * is a measurement to disbelieve rather than a fault. */
	log_line("audio: %u blocks, holes %u (worst %.2f ms), overlap %.2f ms, "
		 "late %u (worst %.2f ms), stalled %u, "
		 "underruns %u, resyncs %u, dropped %u, queue %u ms, "
		 "level %d/%d, produced %.3f, margin %.2f ms "
		 "(cmd %.2f + late %.2f), rate %u.%03u",
		 st.blocks, st.holes, st.worst_hole_ms / 1000.0,
		 st.overlap_us / 1000.0,
		 st.late_blocks, st.worst_late_us / 1000.0,
		 stat_stalled, st.underruns, st.resyncs, st.dropped,
		 st.queue_ms, st.fill_frames, st.fill_target,
		 (double)st.rate_meas_q16 / RESAMP_UNITY,
		 st.margin_us / 1000.0, st.cmd_us / 1000.0, st.track_us / 1000.0,
		 (unsigned)(step / 65536),
		 (unsigned)(((step % 65536) * 1000) / 65536));
}

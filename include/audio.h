/** Audio output (core APU -> CSND). MIT. */
#pragma once

#include "gb3ds.h"

#include <stdbool.h>
#include <stdint.h>

/* Included from C++ as well (the Gambatte backend is C++). */
#ifdef __cplusplus
extern "C" {
#endif

/* Returns false when the sound service could not be started; the emulator
 * keeps running silently in that case. */
bool audio_init(void);
void audio_exit(void);

/* Output rate of everything below this line: 32768 Hz, stereo, s16. */
#define GBABE_AUDIO_RATE 32768u

/* Hand the core's audio to the hardware: `frames` stereo frames at
 * GBABE_AUDIO_RATE, interleaved. Safe once audio_init() has run. */
void audio_push_frames(const int16_t *stereo, unsigned frames);

/* Submit the next block if it is due.
 *
 * A CSND channel plays one buffer and then stops, so the blocks have to be
 * handed over slightly before the previous one ends; doing that only once per
 * emulated frame is not precise enough and leaves a gap. The frame loop calls
 * audio_next_deadline() to find out whether it is worth waking up before the
 * next frame is due, and calls this when it does. */
void audio_pump(void);

/* When the next block should be submitted, in ARM11 ticks (svcGetSystemTick
 * units), or 0 when there is nothing to do before the next frame arrives.
 * A value in the past means "now". */
uint64_t audio_next_deadline(void);

/* What the block scheduler has been doing. Read by the once-a-second log line
 * and by tools/host/audiotest.c, which is the only place these numbers can be
 * checked without a console. Times are microseconds, or -1 when not measured. */
typedef struct
{
	unsigned blocks;      /* blocks submitted since audio_init()          */
	unsigned ear_pairs;   /* blocks submitted as a stereo pair            */
	unsigned underruns;   /* deadlines that arrived with nothing queued   */
	unsigned resyncs;     /* stream dropped and restarted                 */
	unsigned dropped;     /* samples discarded because the FIFO was full  */
	unsigned late_blocks; /* blocks whose deadline had already passed     */
	int margin_us;        /* submit margin currently in use               */
	int worst_late_us;    /* worst lateness seen since audio_init()       */
	unsigned queue_ms;    /* audio buffered ahead of the hardware         */
	int rate_meas_q16;    /* measured production rate, 16.16 (65536 = the
	                       * 32768 Hz the channel is opened at)           */
	int fill_frames;      /* smoothed queue level, in frames              */
	int fill_target;      /* the level it is being held at, in frames     */
} audio_stats;

void audio_get_stats(audio_stats *out);

/* Log the same numbers. Called once a second; it decides itself how often to
 * write a line. */
void audio_log_stats(void);

void audio_set_enabled(bool on);
bool audio_enabled(void);

/* Number of frames the sound FIFO is behind, for the HUD. */
unsigned audio_queue_ms(void);

/* How fast the decimator's output should be fed to the FIFO, in 16.16
 * (RESAMP_UNITY is 1:1). Below 1:1 when the console is producing fewer samples
 * per second than CSND is eating (the audio is stretched, and its pitch falls
 * with the game - see include/resamp.h), above it when the console is running
 * ahead, and exactly 1:1 on a console that keeps up.
 *
 * Advances the measurement; call it once per chunk, as emu_gambatte.cpp does,
 * not from a log line. */
int32_t audio_rate_step(void);

/* The step currently in force, without advancing anything. For the log. */
int32_t audio_rate_current(void);

#ifdef __cplusplus
}
#endif

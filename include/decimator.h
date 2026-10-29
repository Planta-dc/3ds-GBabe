/** The 2 MHz -> 32768 Hz audio decimator, as one pass. MIT. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Gambatte's PSG produces one 32-bit word per sample at 2097152 Hz, holding two
 * 16-bit signed channels (left in the low half, right in the high half on a
 * little-endian machine - see PSG::fillBuffer() in the core). A frame is 35112
 * of them, and 35112 / 2097152 = 16.74 ms, so a frame of audio is a frame.
 *
 * The output has to be 32768 Hz, i.e. one sample per 64 inputs, and the port
 * has always got there in two stages of 8. The second stage is not decoration:
 * the frequency response of a length-8 box filter is only -3.9 dB at the
 * output's Nyquist frequency, so one stage would fold 16-262 kHz straight down
 * into the audible band; two cascaded stages double that in dB, which is the
 * difference between a square wave and a hiss.
 *
 * What is decoration is the buffer between them. The old code wrote 4389
 * intermediate samples per frame into a 17.5 KiB array and read them straight
 * back - a quarter of the frame's audio memory traffic, spent on nothing. This
 * does both stages in one pass over the core's own buffer, in 8-sample steps
 * with no per-sample branch, and is bit-for-bit identical to the two-stage
 * version it replaces (tools/host/dectest.c compares them over 200 frames).
 *
 * 35112 samples per frame is 140 KiB read; at 268 MHz with no L2 that read is
 * the floor on what this can cost, and it is the reason the figure is worth
 * having in the log at all.
 */
typedef struct
{
	int32_t sum_l, sum_r;  /* stage 1: 8 input samples -> one sample */
	int32_t acc_l, acc_r;  /* stage 2: 8 stage-1 samples -> one frame sample */
	uint8_t count1;        /* input samples into the current stage-1 group, 0-7 */
	uint8_t count2;        /* stage-1 samples into the current output sample, 0-7 */
} decimator;

void decimator_reset(decimator *d);

/* Feed `frames` interleaved samples from the core; write at most
 * frames/64 + 1 interleaved stereo int16 samples to `out` and return the count.
 * State carries across calls, so the caller may hand over any block size. */
unsigned decimator_run(decimator *d, const uint32_t *in, unsigned frames,
		       int16_t *out);

#ifdef __cplusplus
}
#endif

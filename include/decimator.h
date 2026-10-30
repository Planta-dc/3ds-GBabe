/** The 2 MHz -> 32768 Hz audio decimator: two box filters, then a 32-tap
 * windowed sinc. MIT. */
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
 * The output has to be 32768 Hz: one sample per 64 inputs.
 *
 * ## Why it is shaped this way
 *
 * The first version of this was two box filters of 8 in a row, which is three
 * adds per input sample and almost no arithmetic at all - and it aliases: the
 * converted stream contains tones that were not in the original, folded into
 * the audible band at up to -6 dB. The second version put a 64-tap windowed
 * sinc in the second stage, which fixes the aliasing and cost 4.2 ms a frame on
 * the console - a quarter of the whole frame budget for the sound, which took
 * the Game Boy Color build from 59.70 to 56.40 fps. docs/PORT-NOTES.md 27, 29.
 *
 * The cost of an FIR here is (taps x outputs), and outputs is fixed at 548 per
 * frame. So the only way to make it cheaper at a given sharpness is to run it
 * at a lower rate: a tap covers more time when the samples are further apart,
 * so half the rate needs half the taps for the same transition band. Hence two
 * box stages - 8, then 2 - before the filter, which puts it at 131072 Hz with
 * 32 taps instead of 262144 Hz with 64. Twice as sharp for half the work.
 *
 * Measured on the host (tools/host/dectest.c) by putting one loud tone in and
 * looking for it where it would land if the conversion aliased it - the figure
 * is the alias, re a full-scale tone:
 *
 *     tone in    lands at      this filter    the box chain
 *     20 kHz     12768 Hz        -27.3 dB        -6.2 dB
 *     50 kHz     15536 Hz        -85.3 dB       -13.6 dB
 *     200 kHz     3392 Hz        -91.7 dB       -35.4 dB
 *     300 kHz     5088 Hz        -96.4 dB       -35.5 dB
 *     700 kHz    11872 Hz        -90.4 dB       -35.7 dB
 *
 * The 20 kHz row is the worst of them and it is the transition band, not the
 * stopband: the corner is 13.5 kHz and 32 taps at this rate take 22 kHz to get
 * from passband to -80 dB, so 20 kHz is only part way down. That table is
 * printed by the test - it is the record, not this comment.
 *
 * ## How the samples move
 *
 * The history is a doubled ring of 2 x DECIMATOR_TAPS: every stage-2 sample is
 * written at `pos` and at `pos + TAPS`, so the last TAPS samples are always the
 * contiguous run starting at `pos + 1` and nothing is ever shifted. (It used to
 * shift the array down by eight with two memmove()s per output - see
 * docs/PORT-NOTES.md 27, where the console measured what that cost.)
 *
 * Stage 3 reads that run as packed 32-bit words - two int16 samples - and the
 * taps are stored packed the same way, so one `smlabb`/`smlatt` does two
 * multiply-accumulates. That is not a micro-optimisation: this console is
 * instruction-bound, not maths-bound, and the straightforward version of the
 * same filter needs about three instructions per multiply and 6.6 for the
 * version that came before it. The alignment that makes the packed reads legal
 * is a property of the design: `pos` advances by DECIMATE_FIR (an even number)
 * per output, so every window starts at an even index, and `hist_*` are
 * declared 4-byte aligned.
 */
/* The FIR runs at the stage-2 rate, 131072 Hz, and decimates by four. Its
 * length is a compromise this console forced (docs/PORT-NOTES.md 29). */
#define DECIMATOR_TAPS   32
#define DECIMATE_BOX1     8   /* inputs per stage-1 sample    */
#define DECIMATE_BOX2     2   /* stage-1 samples per stage-2  */
#define DECIMATE_FIR      4   /* stage-2 samples per output   */

typedef struct
{
	int32_t sum_l, sum_r;  /* stage 1: 8 input samples -> one sample    */
	int32_t sum2_l, sum2_r;/* stage 2: 2 stage-1 samples -> one sample  */
	uint8_t count2;        /* stage-1 samples into the stage-2 group, 0-1 */
	uint8_t count1;        /* input samples into the current stage-1 group, 0-7 */
	uint8_t pending;       /* stage-1 samples since the last output, 0-7 */
	uint8_t pos;           /* where the next stage-1 sample goes, 0..63 */
	/* The last DECIMATOR_TAPS stage-1 samples, held twice over so that the
	 * window a convolution reads is always contiguous - see "the doubled
	 * ring" below. Every sample is written at pos and at pos + TAPS. */
	/* Aligned to 4 bytes because the FIR reads them two samples at a time,
	 * as packed 32-bit words (see source/decimator.c). `aligned()` rather
	 * than `_Alignas` because this header is included from C++ as well, and
	 * C++ spells it differently. */
	int16_t hist_l[2 * DECIMATOR_TAPS] __attribute__((aligned(4)));
	int16_t hist_r[2 * DECIMATOR_TAPS] __attribute__((aligned(4)));
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

/** Top screen rendering. MIT. */
#pragma once

#include "gb3ds.h"

/* Included from C++ as well (the Gambatte backend is C++). */
#ifdef __cplusplus
extern "C" {
#endif

void video_init(void);
void video_exit(void);

/* Select the scaling mode (enum video_scale). Cheap, safe to call per frame. */
void video_set_scale(int mode);

/* Grab the back framebuffer for this frame.
 *
 * Call this as late as possible - after the emulator has produced the frame and
 * immediately before video_draw_frame() - not at the top of the frame. The
 * buffer it hands back is the one the panel has finished with, and "finished
 * with" only becomes true at the vblank that takes the previous swap, a whole
 * panel period after that swap was posted. Called at the top of the frame, 1 ms
 * after the swap, the answer is always "not yet": every frame then blocked on a
 * vblank on its way into the emulation, 4.3 ms measured and counted inside
 * `core`, which is most of the reason this port ran at 56 fps when the emulator
 * alone fits in 8.4 ms (docs/PORT-NOTES.md section 17).
 *
 * Called at the end of the frame the wait is free instead: a frame that has to
 * wait is a frame that arrived early, with its picture already rendered and
 * nothing left to do but present it. A frame that overran finds the swap long
 * taken and does not wait at all. Either way the picture is drawn into a buffer
 * the panel has released, so nothing is ever presented torn.
 */
void video_begin_frame(void);

/* The same, without ever waiting: false means the panel has not released the
 * last buffer yet, so this frame's picture is dropped rather than delayed.
 *
 * The frame loop uses this one. Blocking here measured 5.6-6.3 ms a frame on
 * the console - a third of the budget - and gated the emulator on the panel:
 * 58.8 fps measured with the emulation itself at 9.6 ms. Asking one Game Boy
 * frame after the swap is posted instead of 10.6 ms after it makes the answer
 * "yes" because a full panel period has passed by then. */
bool video_begin_frame_try(void);

/* Draw a whole 160x144 RGB565 frame, `pitch` pixels per line.
 *
 * The top LCD is a rotated panel, so the picture is stored column by column.
 * The frame is scaled a band of destination rows at a time: every write is
 * still a contiguous run of halfwords down a column, and the band is small
 * enough that the handful of source rows it needs stay in L1 while all dst_w
 * columns of the band are written. See the comment there for why the banding
 * is what makes the gather affordable. */
void video_draw_frame(const uint16_t *frame565, int pitch);

/* Ticks spent inside video_draw_frame() since the last call, and reset it.
 *
 * Worth its own figure: the scaled transpose is the only part of the frame
 * whose cost is not obvious from the code - it is 267x240 = 64,080 writes for
 * a 23,040-pixel picture in the default scale, gathered from the source with a
 * stride, on a CPU with a 16 KiB L1 and no L2. */
uint64_t video_draw_take(void);

#ifdef GBABE_HOST_TESTS
/* Host builds only: choose the height of the destination band. video_draw_frame
 * walks the picture in bands of VIDEO_BAND rows; the host test walks that number
 * past every awkward remainder to prove the banding cannot change the picture. */
void video_test_set_band(int rows);
#endif

/* Flush the back buffer and hand it to the panel.
 *
 * The buffer is not safe to write again until the panel has taken the swap.
 * libctru's buffers are a two-deep swap chain: gfxGetFramebuffer() returns the
 * buffer that is *not* displayed, and gfxScreenSwapBuffers() flips the caller's
 * idea of which one that is immediately - while the panel only switches at the
 * next vblank. Draw into the new "back" buffer straight away and, for the
 * 10-15 ms until that vblank arrives, you are writing into the buffer that is
 * still being scanned out. A tear line appears wherever the scanout happens to
 * be, and because the Game Boy's 59.7275 Hz does not divide 60 Hz it *drifts*
 * down the screen.
 *
 * That is why the present is the *last* thing a frame does (see
 * video_begin_frame()) and why the picture is emulated into the backend's own
 * buffer first: the panel needs the time, and the emulator does not need the
 * panel.
 *
 * Everything the loop does once a frame but not per frame - the status redraw,
 * the log flush, the cartridge save - belongs after this call, where the
 * panel's own vblank absorbs it. */
void video_end_frame(void);

/* Has the panel taken the swap made by video_end_frame()? Non-blocking.
 *
 * video_begin_frame() is the caller that cares, and it asks this before
 * waiting: after the emulation the answer is usually "yes, long ago". The frame
 * loop also polls it in the idle time at the top of a frame, which is where the
 * wait costs nothing at all. */
bool video_swap_pending(void);

/* Blocking wait for the panel, for the callers outside the frame loop (menus)
 * and as the fallback in video_begin_frame(). Returns the ticks spent (0 if
 * nothing was pending). */
uint64_t video_present_sync(void);

/* Forget which framebuffers already have a black letterbox.
 *
 * The letterbox is cleared once per buffer instead of once per frame, because
 * it cannot change while the scale does not. Anything else that writes to the
 * top screen has to say so - the scale mode does it itself, and so does every
 * path that fills the screen with video_clear(). */
void video_letterbox_unknown(void);

/* ARM11 ticks spent waiting for the panel in the last video_present_sync(). */
uint64_t video_present_wait(void);

/* Present-path diagnostics since the previous call, for the log:
 *
 *   free_waits    frames that reached video_begin_frame() after the panel had
 *                 already taken the previous swap, so getting the buffer back
 *                 cost nothing
 *   late_applies  swaps the panel did not take at the next vblank, so a second
 *                 one was waited for
 *
 * `free_waits` is the healthy direction: it says the frame arrived at the
 * present with time to spare. `late_applies` should be 0; if it is not, the
 * swap is being posted too late in the panel's period to make the next vblank,
 * which costs a whole extra period - and that is a pacing problem, not a
 * rendering one. */
void video_sync_take(unsigned *free_waits, unsigned *late_applies);

/* Fill the top screen with black (used by menus). */
void video_clear(void);

#ifdef __cplusplus
}
#endif

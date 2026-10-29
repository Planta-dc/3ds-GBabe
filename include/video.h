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

/* Grab the back framebuffer for this frame. */
void video_begin_frame(void);

/* Draw a whole 160x144 RGB565 frame, `pitch` pixels per line.
 *
 * The top LCD is a rotated panel, so the picture is stored column by column.
 * The frame is scaled down a destination column at a time, which makes every
 * write a contiguous run of dst_h halfwords instead of one halfword every 480
 * bytes - the difference between touching each cache line once and touching it
 * 240 times per frame. The source is read column by column too, and stays in
 * cache while it is being read. */
void video_draw_frame(const uint16_t *frame565, int pitch);

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
 * So the swap and the wait for it are two calls: video_present_sync() does the
 * waiting. Everything the loop does once a frame but not per frame - the status
 * redraw, the log flush, the cartridge save - belongs between them, where the
 * wait absorbs it instead of delaying the next frame.
 *
 * If the frame's own work already overran a frame period, video_present_sync()
 * returns 0 and the frame is presented at once. Waiting would take until the
 * *following* vblank and halve the frame rate at exactly the moment the console
 * is struggling - one torn frame is the lesser evil. */
void video_end_frame(void);

/* Has the panel taken the swap made by video_end_frame()? Non-blocking.
 *
 * The frame loop polls this in its idle time before the next frame instead of
 * blocking on the vblank straight after the swap: a blocking wait there pushed
 * every frame past its slot (the Game Boy's frame is 16.74 ms and the panel's
 * is 16.67 ms), which cost ~2 fps and made 95 % of frames start late. The wait
 * comes out of the idle budget now. */
bool video_swap_pending(void);

/* Blocking wait for the panel, for the callers outside the frame loop (menus)
 * and as the safety net in video_begin_frame(). Returns the ticks spent (0 if
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
 *   skipped       frames presented without waiting, because their own work
 *                 already overran a frame period (they may tear)
 *   late_applies  swaps the panel did not take at the next vblank, so a second
 *                 one was waited for
 *
 * Both should be 0 on a console that is keeping up. A non-zero skipped count
 * means the machine is too slow for the scale mode in use, and that is the only
 * way tearing can still appear. */
void video_sync_take(unsigned *skipped, unsigned *late_applies);

/* Fill the top screen with black (used by menus). */
void video_clear(void);

#ifdef __cplusplus
}
#endif

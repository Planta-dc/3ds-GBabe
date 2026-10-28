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

/* Draw one Game Boy scanline (160 RGB565 pixels) into the back framebuffer,
 * applying the current scale mode. line is 0..143, top to bottom. */
void video_draw_line(const uint16_t *line565, int line);

/* Draw a whole 160x144 RGB565 frame at once, `pitch` pixels per line.
 *
 * The top LCD is a rotated panel, so the picture is stored column by column:
 * with one scanline at a time every pixel written is a separate cache line
 * (240 halfwords apart). With the whole frame in hand the writes go down a
 * column instead, which is one contiguous run of dst_h halfwords, and the
 * source is read column by column - also sequential, because the frame is
 * small enough to stay in cache.
 *
 * Callers that only get one line at a time (the Walnut-CGB hook) still use
 * video_draw_line(). */
void video_draw_frame(const uint16_t *frame565, int pitch);

/* Flush and present the back buffer. */
void video_end_frame(void);

/* Fill the top screen with black (used by menus). */
void video_clear(void);

#ifdef __cplusplus
}
#endif

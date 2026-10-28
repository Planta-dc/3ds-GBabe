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

/* Flush and present the back buffer. */
void video_end_frame(void);

/* Fill the top screen with black (used by menus). */
void video_clear(void);

#ifdef __cplusplus
}
#endif

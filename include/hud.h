/**
 * The status strip, drawn in place on the game frame.
 *
 * The old status readout was a separate 320x240 overlay on the bottom screen:
 * drawing it cost 4.2 ms and presenting it - transposing 76,800 pixels into
 * the sideways panel layout and flushing 153 KiB of framebuffer - cost 13.3 ms,
 * together one whole 16.7 ms frame (docs/PORT-NOTES.md §13, §24, §27). Spreading
 * it over eight bands of two milliseconds still cost nine presents a second,
 * because a frame budget with no slack loses a present to any extra
 * millisecond.
 *
 * This strip instead lives in the 160x144 RGB565 framebuffer the core just
 * rendered, in place, before video_draw_frame() scales it onto the panel. It
 * rides the banded transpose that is already happening: the marginal cost is
 * the few thousand pixel writes in hud_draw(), a few hundredths of a
 * millisecond, and there is no second present, no second cache flush and no
 * vblank-time work to fit in. It costs what a few rows of the game image cost,
 * because it is drawn on the game image.
 *
 * MIT licensed.
 */
#pragma once

#include <stdint.h>

/* Draw the strip into the 160x144 RGB565 Game Boy framebuffer (`pitch`
 * halfwords per row), in place, before the frame is presented. Does nothing
 * when the game is not loaded or the strip is switched off. Cheap enough to
 * call every presented frame. */
void hud_draw(uint16_t *fb565, int pitch);

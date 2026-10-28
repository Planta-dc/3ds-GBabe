/**
 * Diagnostic log, for when the console is somewhere you cannot attach a
 * debugger to.
 *
 * Writes a plain text file (sdmc:/3ds/gbabe/debug.log) with the things that
 * are impossible to ask a user for: the frame-time split once a second, which
 * frames went over budget, what the emulator core and the audio service
 * reported, and every settings change. When something is slow, that file is
 * the difference between a guess and a measurement.
 *
 * Rules that keep it free:
 *
 *   - lines are buffered in RAM and written at most every few seconds, so the
 *     log never does an SD write inside a frame;
 *   - events (errors, settings changes, start-up) flush immediately, so a crash
 *     does not lose them;
 *   - after LOG_MAX_BYTES the file stops growing and says so in its last line;
 *   - if the file cannot be opened, logging is simply off.
 *
 * MIT licensed.
 */
#pragma once

#include <stdbool.h>

/* Included from C++ as well (the Gambatte backend is C++). */
#ifdef __cplusplus
extern "C" {
#endif

/* Start (or restart) logging. `enabled` false just records the setting; the
 * file is not opened until it is switched on. Returns whether the file is
 * being written. */
bool log_init(const char *path, bool enabled);

/* Switch it on or off at runtime (the settings tab does this). */
void log_set_enabled(bool on);
bool log_enabled(void);

/* One formatted line. `urgent` lines are flushed right away. */
void log_line(const char *fmt, ...);

/* Write what is buffered and close the file (call before exiting). */
void log_flush(void);
void log_shutdown(void);

#ifdef __cplusplus
}
#endif

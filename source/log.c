/**
 * Diagnostic log. See log.h.
 *
 * MIT licensed.
 */
#include "log.h"

#include <3ds.h>

#include "gb3ds.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Plenty for the ~40 lines a second this writes, small enough to stay in the
 * L1 for its whole life. */
#define LOG_BUF 2048

/* Stop growing here: the first minutes are the interesting ones, and an
 * unbounded log on a 2 GB card is an SD card filling up silently. */
#define LOG_MAX_BYTES (384 * 1024)

static FILE *file;
static bool wanted;
static char buf[LOG_BUF];
static size_t used;
static size_t written;
static u64 start_tick;
static u64 last_write;
static bool truncated;

/* Seconds since the log started. */
static double since_start(void)
{
	if (start_tick == 0)
		start_tick = svcGetSystemTick();

	return (double)(svcGetSystemTick() - start_tick) / SYSCLOCK_ARM11;
}

static void write_out(void)
{
	if (file == NULL || used == 0)
		return;

	fwrite(buf, 1, used, file);
	fflush(file);
	written += used;
	used = 0;
	last_write = svcGetSystemTick();
}

static bool open_file(const char *path)
{
	if (path == NULL)
		return false;

	file = fopen(path, "w");
	if (file == NULL)
		return false;

	/* The absolute time is only useful if the console's clock is set; the
	 * relative stamps are what the numbers below are read against. */
	/* The build stamp, not just the version: a log has to identify the
	 * binary that wrote it, and one version number covers a lot of builds. */
	fprintf(file, "# GBabe " GBABE_VERSION " build " GBABE_BUILD
		       " debug log\n");
	fprintf(file, "# os time: %llu\n", (unsigned long long)osGetTime());
	written = 0;
	truncated = false;
	return true;
}

bool log_init(const char *path, bool enabled)
{
	wanted = enabled;
	start_tick = svcGetSystemTick();
	used = 0;

	if (!enabled)
		return false;

	return open_file(path);
}

void log_set_enabled(bool on)
{
	wanted = on;

	if (!on)
	{
		log_flush();
		if (file != NULL)
		{
			fclose(file);
			file = NULL;
		}
	}
	/* Switching it back on needs the path, which only log_init() and the
	 * caller know: main.c re-inits when the setting changes. */
}

bool log_enabled(void)
{
	return wanted && file != NULL;
}

void log_line(const char *fmt, ...)
{
	char text[256];
	char entry[288];
	va_list ap;
	int n;

	if (file == NULL || !wanted || truncated)
		return;

	va_start(ap, fmt);
	vsnprintf(text, sizeof text, fmt, ap);
	va_end(ap);

	/* One whole line at a time, so a timestamp always starts a line. */
	n = snprintf(entry, sizeof entry, "[%9.3f] %s\n", since_start(), text);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof entry)
		n = (int)sizeof entry - 1;

	if (used + (size_t)n > sizeof buf)
		write_out();

	memcpy(buf + used, entry, (size_t)n);
	used += (size_t)n;

	if (written + used > LOG_MAX_BYTES)
	{
		/* Say why the file stops here, rather than just ending. */
		write_out();
		truncated = true;
		fprintf(file, "[%9.3f] stopped: %u KiB limit reached\n",
			since_start(), (unsigned)(LOG_MAX_BYTES / 1024));
		fflush(file);
	}
}

void log_flush(void)
{
	write_out();
}

/* For a caller in a frame loop: write only when it is worth an SD card write.
 *
 * log_flush() is an fwrite and an fflush, and on a slow card that is 10-20 ms -
 * longer than a frame. The console log showed a 33 ms frame in every single
 * second of a session, and this was one of the candidates. The buffer is 2 KiB
 * and a second of lines is ~500 bytes, so writing it every second spends a
 * hitch to move a quarter of a buffer: wait until it is three quarters full
 * (four seconds of lines) or has been waiting five seconds, and the writes go
 * down four-fold at no cost but how much a crash could lose. */
void log_flush_soft(void)
{
	const u64 now = svcGetSystemTick();

	if (used == 0 || file == NULL)
		return;

	if (used * 4 < sizeof buf * 3 && now - last_write < SYSCLOCK_ARM11 * 5)
		return;

	last_write = now;
	write_out();
}

void log_shutdown(void)
{
	log_flush();

	if (file != NULL)
	{
		fprintf(file, "[  end   ] log closed\n");
		fflush(file);
		fclose(file);
		file = NULL;
	}
}

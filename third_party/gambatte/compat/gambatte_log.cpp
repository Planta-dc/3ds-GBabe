/**
 * Gambatte's log calls -> GBabe's debug log.
 *
 * The core only says a handful of things ("MBC1 ROM loaded", Sachen warnings),
 * all of them once per ROM load, so this is cheap enough to keep in the build
 * unconditionally.
 */
#include "gambatte_log.h"

#include <stdarg.h>
#include <stdio.h>

#include "log.h"

void gambatte_log(int level, const char *format, ...)
{
	char text[192];
	va_list ap;

	(void)level;
	va_start(ap, format);
	vsnprintf(text, sizeof text, format, ap);
	va_end(ap);

	/* The core's messages end in a newline; the log adds its own. */
	size_t n = 0;
	while (text[n] != '\0' && text[n] != '\n')
		n++;
	text[n] = '\0';

	if (text[0] != '\0')
		log_line("core: %s", text);
}

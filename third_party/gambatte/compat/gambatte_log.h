/**
 * The vendored core's log calls, redirected to GBabe's own log.
 *
 * Gambatte's sources call gambatte_log(RETRO_LOG_INFO, ...) with the libretro
 * severity names. GBabe does not build the libretro front-end, so this header
 * supplies those names and a function that writes to debug.log instead
 * (source/log.c). Keeping the names means the vendored .cpp files can be used
 * exactly as upstream ships them.
 */
#ifndef GAMBATTE_LOG_H
#define GAMBATTE_LOG_H

enum
{
	RETRO_LOG_DEBUG = 0,
	RETRO_LOG_INFO  = 1,
	RETRO_LOG_WARN  = 2,
	RETRO_LOG_ERROR = 3
};

#ifdef __cplusplus
extern "C" {
#endif

void gambatte_log(int level, const char *format, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 2, 3)))
#endif
	;

#ifdef __cplusplus
}
#endif

#endif

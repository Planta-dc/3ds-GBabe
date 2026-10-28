/**
 * Checks the debug log's format, because that file is the only view of a
 * console nobody can attach a debugger to.
 *
 *   make -C tools/host logtest
 *
 * NOT part of the 3DS build.
 */
#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

static int failures;

static void check(const char *what, int ok)
{
	printf("%s: %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		failures++;
}

static char *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return NULL;

	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);

	char *buf = malloc((size_t)n + 1);
	size_t got = fread(buf, 1, (size_t)n, f);
	buf[got] = '\0';
	fclose(f);
	if (len != NULL)
		*len = got;
	return buf;
}

int main(void)
{
	const char *path = "logtest.log";
	size_t len = 0;

	remove(path);

	/* off by default: nothing may be created */
	check("disabled log creates no file",
	      !log_init(path, false) && slurp(path, NULL) == NULL);
	check("log_enabled() is false", !log_enabled());

	/* on: lines carry a timestamp and end with a newline */
	check("log_init() opens the file", log_init(path, true));
	log_line("GBabe %s test", "0.1.0");
	log_line("perf fps=%0.1f core=%0.1f", 59.7, 9.8);
	log_line(""); /* empty lines are legal */
	log_line("a very long line: %s", "0123456789012345678901234567890123456789"
					  "0123456789012345678901234567890123456789"
					  "0123456789012345678901234567890123456789"
					  "0123456789012345678901234567890123456789"
					  "0123456789012345678901234567890123456789");
	log_flush();
	log_shutdown();

	char *text = slurp(path, &len);
	check("the file was written", text != NULL && len > 0);

	/* Lines starting with '#' are the file header (version, clock); every
	 * other line has to carry a "[seconds]" stamp. */
	int lines = 0, stamped = 0, header = 0;
	for (char *p = text; p != NULL && *p; )
	{
		char *nl = strchr(p, '\n');
		if (nl == NULL)
			break;

		if (*p == '#')
			header++;
		else
		{
			lines++;
			/* "[    0.000] text" */
			if (*p == '[' && strchr(p, ']') != NULL &&
			    strchr(p, ']') < nl)
				stamped++;
		}
		p = nl + 1;
	}

	printf("log has %d lines + %d header lines\n", lines, header);
	check("the header is present", header == 2);
	check("every entry is timestamped", lines > 0 && stamped == lines);
	check("startup line is present", strstr(text, "GBabe 0.1.0 test") != NULL);
	check("values are formatted", strstr(text, "fps=59.7 core=9.8") != NULL);
	check("long lines are truncated, not split",
	      strstr(text, "0123456789") != NULL);
	check("shutdown marks the end", strstr(text, "log closed") != NULL);

	/* an over-long single line must not become several lines */
	int longest = 0, cur = 0;
	for (char *p = text; *p; p++)
	{
		cur = (*p == '\n') ? 0 : cur + 1;
		if (cur > longest)
			longest = cur;
	}
	printf("longest line: %d bytes\n", longest);
	check("a line never exceeds 288 bytes", longest <= 288);

	free(text);

	/* switching off closes it; turning it back on needs a new init */
	log_init(path, true);
	log_line("first");
	log_set_enabled(false);
	check("disabled logging writes nothing", !log_enabled());
	log_line("after disable");
	log_init(path, true);
	log_line("second");
	log_flush();
	log_shutdown();

	text = slurp(path, &len);
	check("re-opening truncates the old file",
	      text != NULL && strstr(text, "after disable") == NULL &&
		      strstr(text, "second") != NULL);
	free(text);

	remove(path);
	printf(failures ? "log: FAIL\n" : "log: ok\n");
	return failures ? 1 : 0;
}

/**
 * Settings and last-ROM bookkeeping.
 *
 * A deliberately boring "key=value" text file, so a user can fix a bad setting
 * with any text editor on the SD card.
 *
 * MIT licensed.
 */
#include "gb3ds.h"

#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <sys/types.h>

#include "palette.h"

gbabe_config g_cfg;

void config_defaults(void)
{
	memset(&g_cfg, 0, sizeof g_cfg);
	g_cfg.scale = SCALE_FIT;
	g_cfg.palette = 0;      /* auto */
	g_cfg.frameskip = 0;    /* off */
	g_cfg.audio_on = true;
	g_cfg.show_hud = true;
	g_cfg.debug_log = true; /* on by default: it is the only way to see why a
				  build is slow on someone else's console */
	g_cfg.fast_forward = false;
	g_cfg.rom_path[0] = '\0';
}

static void config_dir(void)
{
	mkdir(GBABE_ROOT, 0777);
	mkdir(GBABE_SAVE_DIR, 0777);
	mkdir(GBABE_STATE_DIR, 0777);
}

void config_load(void)
{
	config_dir();

	FILE *f = fopen(GBABE_CONFIG_PATH, "r");
	if (f == NULL)
		return;

	char line[256];
	while (fgets(line, sizeof line, f) != NULL)
	{
		char key[64];
		char val[192];
		if (sscanf(line, "%63[^=]=%191[^\n]", key, val) != 2)
			continue;

		if (!strcmp(key, "scale"))
			g_cfg.scale = atoi(val);
		else if (!strcmp(key, "palette"))
			g_cfg.palette = atoi(val);
		else if (!strcmp(key, "frameskip"))
			g_cfg.frameskip = atoi(val);
		else if (!strcmp(key, "audio"))
			g_cfg.audio_on = atoi(val) != 0;
		else if (!strcmp(key, "hud"))
			g_cfg.show_hud = atoi(val) != 0;
		else if (!strcmp(key, "log"))
			g_cfg.debug_log = atoi(val) != 0;
		else if (!strcmp(key, "rom"))
		{
			strncpy(g_cfg.rom_path, val, sizeof g_cfg.rom_path - 1);
			g_cfg.rom_path[sizeof g_cfg.rom_path - 1] = '\0';
		}
	}
	fclose(f);

	if (g_cfg.scale < 0 || g_cfg.scale >= SCALE_COUNT)
		g_cfg.scale = SCALE_FIT;
	if (g_cfg.palette < 0 || g_cfg.palette >= palette_count())
		g_cfg.palette = 0;
}

void config_save(void)
{
	config_dir();

	FILE *f = fopen(GBABE_CONFIG_PATH, "w");
	if (f == NULL)
		return;

	fprintf(f, "scale=%d\n", g_cfg.scale);
	fprintf(f, "palette=%d\n", g_cfg.palette);
	fprintf(f, "frameskip=%d\n", g_cfg.frameskip);
	fprintf(f, "audio=%d\n", g_cfg.audio_on ? 1 : 0);
	fprintf(f, "hud=%d\n", g_cfg.show_hud ? 1 : 0);
	fprintf(f, "log=%d\n", g_cfg.debug_log ? 1 : 0);
	fprintf(f, "rom=%s\n", g_cfg.rom_path);
	fclose(f);
}

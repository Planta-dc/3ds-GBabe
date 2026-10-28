/**
 * What the bottom screen looks like: ROM browser, pause menu, status readout,
 * messages.
 *
 * Pure drawing, no state beyond layout constants: the callers in ui.c own the
 * selection, the scroll offset and the settings. That keeps every screen
 * testable on the host (tools/uipreview renders them straight to PNG).
 *
 * The layout language - dark panels, an accent colour, value pills, a footer of
 * key hints - follows bheppe's gpsp-3d (GPLv2). Only the look is shared; this
 * file is written from scratch for GBabe.
 *
 * MIT licensed.
 */
#pragma once

#include <stdbool.h>

#include "ui_gfx.h"

#define UI_HEADER_H 30

/* ---- file browser ------------------------------------------------------- */

#define UI_LIST_Y     60
#define UI_LIST_PITCH 21
#define UI_LIST_H     19
#define UI_LIST_ROWS  7

/* `right` is the version tag in the header. The path may be long; its middle is
 * elided so the tail stays readable. */
void scr_browser(const char *right, const char *path, const char *const *names,
		 const bool *isdir, int n, int sel, int top, bool ok);

/* Which row of the list is at screen y? Returns an index (absolute, i.e.
 * top + row) or -1. */
int scr_browser_hit(int y, int top, int n);

/* ---- pause menu --------------------------------------------------------- */

typedef enum
{
	UI_ROW_ACTION, /* Enter / Reset / Quit */
	UI_ROW_CHOICE, /* < value > */
	UI_ROW_TOGGLE, /* on / off pill */
} ui_row_kind;

typedef struct
{
	const char *label;
	const char *value; /* shown on the right for CHOICE/TOGGLE */
	ui_row_kind kind;
} ui_row;

typedef struct
{
	const char *title;  /* "GBabe 0.1.0" */
	const char *status; /* right-hand side, e.g. "paused" */
	const char *const *tabs;
	int n_tabs;
	int tab;
	const ui_row *rows;
	int n_rows;
	int sel;
	const char *note; /* grey line under the rows, may be NULL */
} ui_menu;

void scr_menu(const ui_menu *m);

/* -1 when the point is not on any tab, otherwise the tab index. */
int scr_menu_hit_tab(int x, int y, const char *const *tabs, int n_tabs);
/* Row index under y, or -1. */
int scr_menu_hit_row(int y, int n_rows);

/* ---- in-game status ----------------------------------------------------- */

#define UI_HUD_STATS 6

typedef struct
{
	const char *title;
	const char *sub;
	float fps;
	float frame_ms;
	float core_ms; /* core + blit, used for the frame-time bar */
	float blit_ms;
	float wait_ms;
	const char *stat_label[UI_HUD_STATS];
	const char *stat_value[UI_HUD_STATS];
	int n_stats;
} ui_hud;

void scr_hud(const ui_hud *h);

/* ---- messages ----------------------------------------------------------- */

void scr_message(const char *title, const char *l1, const char *l2,
		 const char *hint);

/* Flat background only. */
void scr_blank(void);

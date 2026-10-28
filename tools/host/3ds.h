/**
 * Minimal stand-in for libctru, used only by tools/host to compile and run the
 * GBabe sources on a PC. Prototypes are copied from the real libctru headers
 * so a successful link here means the front-end only uses APIs that exist.
 *
 * This file is NOT part of the 3DS build.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef s32 Result;

#define R_SUCCEEDED(r) ((r) >= 0)
#define R_FAILED(r) ((r) < 0)

#define BIT(n) (1u << (n))

typedef enum
{
	GFX_TOP = 0,
	GFX_BOTTOM = 1,
} gfxScreen_t;

typedef enum
{
	GFX_LEFT = 0,
	GFX_RIGHT = 1,
} gfx3dSide_t;

typedef enum
{
	GSP_BGR8_OES = 1,
	GSP_RGB565_OES = 2,
	GSP_RGB5_A1_OES = 3,
	GSP_RGBA4_OES = 4,
} GSPGPU_FramebufferFormat;

typedef enum
{
	KEY_A = BIT(0),
	KEY_B = BIT(1),
	KEY_SELECT = BIT(2),
	KEY_START = BIT(3),
	KEY_DRIGHT = BIT(4),
	KEY_DLEFT = BIT(5),
	KEY_DUP = BIT(6),
	KEY_DDOWN = BIT(7),
	KEY_R = BIT(8),
	KEY_L = BIT(9),
	KEY_X = BIT(10),
	KEY_Y = BIT(11),
	KEY_TOUCH = BIT(12),
	KEY_CPAD_RIGHT = BIT(28),
	KEY_CPAD_LEFT = BIT(29),
	KEY_CPAD_UP = BIT(30),
	KEY_CPAD_DOWN = BIT(31),
} Key;

typedef struct
{
	u16 px, py;
} touchPosition;

typedef struct
{
	const char *name;
} PrintConsole;

/* CSND */
typedef enum
{
	CSND_ENCODING_PCM8 = 0,
	CSND_ENCODING_PCM16 = 1,
} CSND_Encoding;

#define CSND_LOOPMODE_MANUAL 0
#define CSND_LOOPMODE_NORMAL 1
#define CSND_LOOPMODE_ONESHOT 2
#define CSND_LOOPMODE_NORELOAD 3

#define SOUND_ENABLE BIT(12)
#define SOUND_FORMAT(v) ((v) << 12)
#define SOUND_LOOPMODE(v) ((v) << 10)
#define SOUND_ONE_SHOT SOUND_LOOPMODE(CSND_LOOPMODE_ONESHOT)
#define SOUND_REPEAT SOUND_LOOPMODE(CSND_LOOPMODE_NORMAL)
#define SOUND_FORMAT_16BIT SOUND_FORMAT(CSND_ENCODING_PCM16)

#define SYSCLOCK_ARM11 268123480.0
#define SYSCLOCK_SOC 16756991.0

/* Host-only output buffer so the harness can dump what would be on screen. */
extern u16 g_host_fb[400 * 240];
/* Bottom screen, un-rotated to 320x240 landscape, after the last present. */
extern u16 g_host_bottom_fb[320 * 240];

/* Fills every framebuffer libctru would hand out with `value`, like the
 * uninitialised linearAlloc() memory a real 3DS starts with, so a test can tell
 * "the code cleared this" apart from "the code never wrote here". */
void g_host_poison_framebuffers(u16 value);
extern int g_host_frames;
extern int g_host_max_frames;
extern bool g_host_quit;

u64   svcGetSystemTick(void);
void  svcSleepThread(s64 ns);
u64   osGetTime(void);
void  osSetSpeedupEnable(bool enable);
u32   osConvertVirtToPhys(const void *vaddr);

bool  aptMainLoop(void);
Result APT_SetAppCpuTimeLimit(u32 percent);
Result APT_CheckNew3DS(bool *out);

void  gfxInitDefault(void);
void  gfxExit(void);
void  gfxSet3D(bool enable);
void  gfxSetScreenFormat(gfxScreen_t screen, GSPGPU_FramebufferFormat format);
void  gfxSetDoubleBuffering(gfxScreen_t screen, bool enable);
u8   *gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16 *width, u16 *height);
void  gfxFlushBuffers(void);
void  gfxSwapBuffers(void);
void  gfxScreenSwapBuffers(gfxScreen_t scr, bool hasStereo);
void  gspWaitForVBlank(void);
Result GSPGPU_FlushDataCache(const void *adr, u32 size);

PrintConsole *consoleInit(gfxScreen_t screen, PrintConsole *console);
void  consoleClear(void);

void  hidScanInput(void);
u32   hidKeysDown(void);
u32   hidKeysHeld(void);
u32   hidKeysDownRepeat(void);
void  hidTouchRead(touchPosition *pos);

Result csndInit(void);
void   csndExit(void);
Result CSND_UpdateInfo(bool waitDone);
Result csndIsPlaying(u32 channel, u8 *status);
Result CSND_FlushDataCache(const void *adr, u32 size);
Result csndExecCmds(bool waitDone);
void   CSND_SetPlayStateR(u32 channel, u32 value);
Result csndPlaySound(int chn, u32 flags, u32 sampleRate, float vol, float pan,
		     void *data0, void *data1, u32 size);

void *linearAlloc(size_t size);
void  linearFree(void *mem);

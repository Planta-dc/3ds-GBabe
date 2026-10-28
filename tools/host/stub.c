/**
 * Host implementation of the libctru subset in 3ds.h.
 *
 * gfxGetFramebuffer() hands out two real 240x400 RGB565 buffers and
 * gfxSwapBuffers() alternates them, so the harness exercises the exact pixel
 * path video.c uses on hardware (including the 90 degree rotation).
 *
 * This file is NOT part of the 3DS build.
 */
#include <3ds.h>

#include <stdlib.h>
#include <time.h>

u16 g_host_fb[400 * 240];
u16 g_host_bottom_fb[320 * 240];
int g_host_frames;
int g_host_max_frames = 300;
bool g_host_quit;

static u16 fb_backing[2][240 * 400];
static u16 fb_bottom[2][240 * 320];
static int fb_index;
static int fb_bottom_index;
static int fb_top_format = GSP_BGR8_OES;

u64 svcGetSystemTick(void)
{
	static u64 base;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	u64 ns = (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
	if (base == 0)
		base = ns;
	/* Convert to ARM11 ticks, like the real thing. */
	return (u64)((ns - base) * (SYSCLOCK_ARM11 / 1e9));
}

void svcSleepThread(s64 ns)
{
	struct timespec ts = { .tv_sec = ns / 1000000000,
			       .tv_nsec = ns % 1000000000 };
	nanosleep(&ts, NULL);
}

u64 osGetTime(void)
{
	return (u64)time(NULL) * 1000ull;
}

void osSetSpeedupEnable(bool enable) { (void)enable; }
u32 osConvertVirtToPhys(const void *vaddr) { return (u32)(uintptr_t)vaddr; }

bool aptMainLoop(void)
{
	if (g_host_quit)
		return false;
	if (g_host_max_frames >= 0 && g_host_frames >= g_host_max_frames)
		return false;
	return true;
}

Result APT_SetAppCpuTimeLimit(u32 percent) { (void)percent; return 0; }
Result APT_CheckNew3DS(bool *out) { *out = false; return 0; }

void gfxInitDefault(void) { fb_index = 0; }
void gfxExit(void) {}
void gfxSet3D(bool enable) { (void)enable; }
void gfxSetScreenFormat(gfxScreen_t screen, GSPGPU_FramebufferFormat format)
{
	if (screen == GFX_TOP)
		fb_top_format = (int)format;
	(void)fb_top_format;
}
void gfxSetDoubleBuffering(gfxScreen_t screen, bool enable)
{
	(void)screen;
	(void)enable;
}

u8 *gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16 *width, u16 *height)
{
	(void)side;
	if (width != NULL)
		*width = 240;
	if (height != NULL)
		*height = (screen == GFX_TOP) ? 400 : 320;
	return (screen == GFX_TOP) ? (u8 *)fb_backing[fb_index]
				   : (u8 *)fb_bottom[fb_bottom_index];
}

Result GSPGPU_FlushDataCache(const void *adr, u32 size)
{
	(void)adr;
	(void)size;
	return 0;
}

void gfxScreenSwapBuffers(gfxScreen_t scr, bool hasStereo)
{
	(void)hasStereo;
	if (scr == GFX_TOP)
	{
		const u16 *src = fb_backing[fb_index];
		for (int y = 0; y < 240; y++)
			for (int x = 0; x < 400; x++)
				g_host_fb[y * 400 + x] =
					src[x * 240 + (239 - y)];
		fb_index ^= 1;
		return;
	}

	/* Undo the rotation: the panel is 240 wide x 320 tall in memory, so
	 * landscape (x, y) sits at x * 240 + (239 - y) - same constant as the top
	 * screen, because both panels are 240 pixels on their short side. */
	const u16 *src = fb_bottom[fb_bottom_index];
	for (int y = 0; y < 240; y++)
		for (int x = 0; x < 320; x++)
			g_host_bottom_fb[y * 320 + x] = src[x * 240 + (239 - y)];

	fb_bottom_index ^= 1;
}

void gfxFlushBuffers(void) {}

void gfxSwapBuffers(void)
{
	/* libctru's gfxSwapBuffers() swaps *both* screens - which is why video.c
	 * calls gfxScreenSwapBuffers(GFX_TOP, true) alone, and why this stand-in
	 * does the same thing rather than only the top. */
	gfxScreenSwapBuffers(GFX_TOP, true);
	gfxScreenSwapBuffers(GFX_BOTTOM, true);
}

void gspWaitForVBlank(void) {}

PrintConsole *consoleInit(gfxScreen_t screen, PrintConsole *console)
{
	(void)screen;
	return console;
}
void consoleClear(void) {}

void hidScanInput(void) {}
u32 hidKeysDown(void) { return 0; }
u32 hidKeysHeld(void) { return 0; }
u32 hidKeysDownRepeat(void) { return 0; }
void hidTouchRead(touchPosition *pos) { pos->px = pos->py = 0; }

Result csndInit(void) { return -1; } /* no audio on the host */
void csndExit(void) {}
Result CSND_UpdateInfo(bool waitDone) { (void)waitDone; return -1; }
Result csndIsPlaying(u32 channel, u8 *status) { (void)channel; *status = 0; return -1; }
Result CSND_FlushDataCache(const void *adr, u32 size) { (void)adr; (void)size; return 0; }
Result csndExecCmds(bool waitDone) { (void)waitDone; return 0; }
void CSND_SetPlayStateR(u32 channel, u32 value) { (void)channel; (void)value; }
Result csndPlaySound(int chn, u32 flags, u32 sampleRate, float vol, float pan,
		     void *data0, void *data1, u32 size)
{
	(void)chn; (void)flags; (void)sampleRate; (void)vol; (void)pan;
	(void)data0; (void)data1; (void)size;
	return -1;
}

void *linearAlloc(size_t size) { return malloc(size); }
void linearFree(void *mem) { free(mem); }

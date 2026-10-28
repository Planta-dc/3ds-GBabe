/**
 * Host implementation of the libctru subset in 3ds.h.
 *
 * gfxGetFramebuffer() hands out two real 240x400 RGB565 buffers and
 * gfxSwapBuffers() alternates them, so the harness exercises the exact pixel
 * path video.c uses on hardware (including the 90 degree rotation).
 *
 * Double buffering is modelled the way libctru does it, because the difference
 * matters: gfxGetFramebuffer() returns the *back* buffer (cur ^ isDoubleBuf) and
 * gfxScreenSwapBuffers() flips the presented one. A screen that is swapped
 * without being drawn therefore shows whatever the other buffer holds - which
 * is how "the UI flickers, and only every other frame is right" bugs happen - so
 * a test here can catch that. gfxSwapBuffers() swaps *both* screens, as on the
 * real thing.
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

static u16 fb_backing[2][240 * 400]; /* top, 240x400 in panel layout */
static u16 fb_bottom[2][240 * 320];  /* bottom, ditto */
static int fb_cur[2];                /* presented buffer, per screen */
static int fb_double[2] = { 1, 1 };  /* gfxInitDefault() enables it */
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

void g_host_poison_framebuffers(u16 value)
{
	for (int b = 0; b < 2; b++)
	{
		for (int i = 0; i < 240 * 400; i++)
			fb_backing[b][i] = value;
		for (int i = 0; i < 240 * 320; i++)
			fb_bottom[b][i] = value;
	}

	/* The stand-in presents immediately, unlike a real swap chain. */
	for (int i = 0; i < 400 * 240; i++)
		g_host_fb[i] = value;
	for (int i = 0; i < 320 * 240; i++)
		g_host_bottom_fb[i] = value;
}

void gfxInitDefault(void)
{
	/* Like libctru: both screens enabled for double buffering, buffer 0
	 * presented, and the other buffer left as it was (uninitialised). */
	fb_double[0] = fb_double[1] = 1;
	fb_cur[0] = fb_cur[1] = 0;
}
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
	fb_double[(int)screen] = enable ? 1 : 0;
}

u8 *gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16 *width, u16 *height)
{
	(void)side;
	if (width != NULL)
		*width = 240;
	if (height != NULL)
		*height = (screen == GFX_TOP) ? 400 : 320;
	/* The back buffer: what you draw into before swapping. */
	const int id = fb_cur[(int)screen] ^ fb_double[(int)screen];
	return (screen == GFX_TOP) ? (u8 *)fb_backing[id]
				   : (u8 *)fb_bottom[id];
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

	/* Toggle first: the buffer that becomes current is the one presented,
	 * which is what the caller just drew into. */
	fb_cur[(int)scr] ^= fb_double[(int)scr];

	/* Undo the panel rotation so the host sees a landscape image: the panel
	 * is 240 wide in memory, so landscape (x, y) sits at
	 * x * 240 + (239 - y) - the same constant for both screens, because both
	 * panels are 240 pixels on their short side. */
	if (scr == GFX_TOP)
	{
		const u16 *src = fb_backing[fb_cur[0]];
		for (int y = 0; y < 240; y++)
			for (int x = 0; x < 400; x++)
				g_host_fb[y * 400 + x] =
					src[x * 240 + (239 - y)];
		return;
	}

	const u16 *src = fb_bottom[fb_cur[1]];
	for (int y = 0; y < 240; y++)
		for (int x = 0; x < 320; x++)
			g_host_bottom_fb[y * 320 + x] = src[x * 240 + (239 - y)];
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

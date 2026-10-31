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

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

u16 g_host_fb[400 * 240];
u16 g_host_bottom_fb[320 * 240];
int g_host_frames;
int g_host_tears;

/* libctru answers this from the homebrew launcher's environment block. The host
 * build stands in for an installed title, which is the case that has a version
 * string to check. */
bool envIsHomebrew(void) { return false; }
int g_host_max_frames = 300;
bool g_host_quit;

static u16 fb_backing[2][240 * 400]; /* top, 240x400 in panel layout */
static u16 fb_bottom[2][240 * 320];  /* bottom, ditto */
static int fb_cur[2];                /* presented buffer, per screen */
static int present_pending[2];       /* a swap the panel has not taken yet */
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

	/* A swap that the panel has not taken yet means the buffer this call is
	 * about to hand out is the one on screen right now. Drawing into it is
	 * exactly the tear that video_end_frame() waits to avoid. */
	if (screen == GFX_TOP && present_pending[0])
		g_host_tears++;
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
	present_pending[(int)scr] = 1;

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

/* The panel takes a swap at the next vblank. Modelled because video.c waits for
 * it: a swap that is still pending means the buffer the caller is about to draw
 * into is still the one on screen. */
bool gspIsPresentPending(unsigned screen)
{
	return screen < 2 && present_pending[screen] != 0;
}

void gspWaitForVBlank(void)
{
	present_pending[0] = present_pending[1] = 0;
}

/* The panel takes a pending swap. Same thing gspWaitForVBlank() does, without
 * the wait - the real one happens at a vblank, and the point of
 * video_swap_pending() is that the caller polls instead of waiting. */
void g_host_present_take(void)
{
	gspWaitForVBlank();
}

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

/* ------------------------------------------------------------------------- */
/* Simulated CSND                                                           */
/* ------------------------------------------------------------------------- */

/* Off by default: a render check has no use for real-time audio. With
 * GBABE_HOST_AUDIO=1 the service pretends to play the blocks it is given, in
 * real time, which is enough to measure whether the block scheduler keeps the
 * stream continuous (see audiotest.c). */
u32 csndChannels;
host_csnd_block g_host_csnd_blocks[HOST_CSND_MAX_BLOCKS];
unsigned g_host_csnd_count;

static int csnd_on;           /* the fake service is running       */
static u64 block_end[CSND_NUM_CHANNELS]; /* when each channel falls silent */
static u32 block_rate[CSND_NUM_CHANNELS];

void g_host_csnd_reset(void)
{
	g_host_csnd_count = 0;
	for (int i = 0; i < CSND_NUM_CHANNELS; i++)
		block_end[i] = 0;
}

Result csndInit(void)
{
	const char *on = getenv("GBABE_HOST_AUDIO");

	if (on == NULL || (on[0] != '1' && on[0] != '2'))
		return -1; /* no audio on the host, as before */

	csnd_on = 1;
	/* "1" hands out four channels (stereo, the normal case); "2" only two, so
	 * the mono fallback in audio.c is exercised too. */
	csndChannels = (on[0] == '2') ? (BIT(30) | BIT(29))
				      : (BIT(30) | BIT(29) | BIT(28) | BIT(27));
	g_host_csnd_reset();
	return 0;
}

void csndExit(void) { csnd_on = 0; }

Result CSND_UpdateInfo(bool waitDone) { (void)waitDone; return 0; }

Result csndIsPlaying(u32 channel, u8 *status)
{
	*status = (channel < CSND_NUM_CHANNELS &&
		   svcGetSystemTick() < block_end[channel]) ? 1 : 0;
	return 0;
}

Result CSND_FlushDataCache(const void *adr, u32 size) { (void)adr; (void)size; return 0; }
Result csndExecCmds(bool waitDone) { (void)waitDone; return 0; }

void CSND_SetPlayStateR(u32 channel, u32 value)
{
	if (channel < CSND_NUM_CHANNELS && value == 0)
		block_end[channel] = svcGetSystemTick();
}

/* Stub implementations for manual CSND setup (used by audio.c start_block) */
void CSND_SetChnRegs(u32 channel, u32 addr0, u32 addr1, u32 rate, u32 flags)
{
	(void)addr0; (void)addr1; (void)flags;
	if (channel < CSND_NUM_CHANNELS && rate != 0)
		block_rate[channel] = rate;
}

u32 CSND_VOL(float left, float right)
{
	/* In the real libctru, this packs volume/pan into a u32 register value.
	 * For the host stub, we don't need the actual value. */
	(void)left; (void)right;
	return 0;
}

void CSND_SetVol(u32 channel, u32 vol)
{
	(void)channel; (void)vol;
	/* Stub - real implementation sets the volume register */
}

void CSND_SetPlayState(u32 channel, u32 value)
{
	if (channel < CSND_NUM_CHANNELS)
	{
		if (value == 0)
		{
			/* Stop playback */
			block_end[channel] = svcGetSystemTick();
		}
		else
		{
			/* Start playback - track this as a block submission.
			 * BLOCK_SAMPLES is 1024, so the block duration is based on the
			 * channel's rate (set by CSND_SetChnRegs). */
			const u64 now = svcGetSystemTick();
			const u32 frames = 1024; /* BLOCK_SAMPLES */
			block_end[channel] = now + (u64)((double)frames * SYSCLOCK_ARM11 / block_rate[channel]);

			if (g_host_csnd_count < HOST_CSND_MAX_BLOCKS)
			{
				g_host_csnd_blocks[g_host_csnd_count].start = now;
				g_host_csnd_blocks[g_host_csnd_count].ticks = block_end[channel] - now;
				g_host_csnd_blocks[g_host_csnd_count].channel = channel;
				g_host_csnd_blocks[g_host_csnd_count].frames = frames;
				g_host_csnd_count++;
			}
		}
	}
}

Result csndPlaySound(int chn, u32 flags, u32 sampleRate, float vol, float pan,
		     void *data0, void *data1, u32 size)
{
	(void)flags; (void)vol; (void)pan; (void)data0; (void)data1;

	if (!csnd_on || chn < 0 || chn >= CSND_NUM_CHANNELS || sampleRate == 0)
		return -1;

	const u64 now = svcGetSystemTick();
	const u32 frames = size / sizeof(s16);
	block_rate[chn] = sampleRate;
	block_end[chn] = now + (u64)((double)frames * SYSCLOCK_ARM11 / sampleRate);

	if (g_host_csnd_count < HOST_CSND_MAX_BLOCKS)
	{
		g_host_csnd_blocks[g_host_csnd_count].start = now;
		g_host_csnd_blocks[g_host_csnd_count].ticks = block_end[chn] - now;
		g_host_csnd_blocks[g_host_csnd_count].channel = chn;
		g_host_csnd_blocks[g_host_csnd_count].frames = frames;
		g_host_csnd_count++;
	}
	return 0;
}

/* Silence between blocks, and how far apart the two ears of a pair started.
 * Both are what "the sound stutters" looks like from here. */
int g_host_csnd_report(void)
{
	if (g_host_csnd_count == 0)
	{
		printf("csnd: no blocks were ever submitted\n");
		return 1;
	}

	/* Union of every block's playing interval: any hole in it is audio the
	 * hardware did not play because nothing had been submitted. */
	u64 covered_until = g_host_csnd_blocks[0].start;
	u64 worst_gap = 0;
	u64 total_gap = 0;
	unsigned gaps = 0;
	unsigned long_gaps = 0;

	for (unsigned i = 0; i < g_host_csnd_count; i++)
	{
		const u64 s = g_host_csnd_blocks[i].start;
		const u64 e = s + g_host_csnd_blocks[i].ticks;

		if (s > covered_until)
		{
			const u64 gap = s - covered_until;
			total_gap += gap;
			if (gap > worst_gap)
				worst_gap = gap;
			if (gap > (u64)(SYSCLOCK_ARM11 * 0.0005))
				long_gaps++;
			gaps++;
		}
		if (e > covered_until)
			covered_until = e;
	}

	/* Ears: consecutive submissions within a millisecond of each other are
	 * the two channels of one block, and should start together. In the mono
	 * fallback there are no pairs, which is fine - but then only one channel
	 * may be in use. */
	u64 worst_skew = 0;
	unsigned pairs = 0;

	unsigned channels_used = 0;
	for (unsigned i = 0; i < g_host_csnd_count; i++)
	{
		unsigned mask = 0;
		for (unsigned j = 0; j < i; j++)
			if (g_host_csnd_blocks[j].channel ==
			    g_host_csnd_blocks[i].channel)
				mask = 1;
		if (!mask)
			channels_used++;
	}

	for (unsigned i = 1; i < g_host_csnd_count; i++)
	{
		const s64 d = (s64)g_host_csnd_blocks[i].start -
			      (s64)g_host_csnd_blocks[i - 1].start;

		if (d >= 0 && d < (s64)(SYSCLOCK_ARM11 / 1000))
		{
			if ((u64)d > worst_skew)
				worst_skew = (u64)d;
			pairs++;
		}
	}

	const double ms = 1000.0 / SYSCLOCK_ARM11;
	const double span = (double)(covered_until - g_host_csnd_blocks[0].start);

	printf("csnd: %u blocks submitted on %u channels, %u ear pairs, "
	       "%.0f ms of audio\n", g_host_csnd_count, channels_used, pairs,
	       span * ms);
	printf("csnd: %u holes, %.2f ms total, worst %.2f ms; worst ear skew "
	       "%.2f ms\n", gaps, (double)total_gap * ms, (double)worst_gap * ms,
	       (double)worst_skew * ms);

	/* A hole is audible when it is long enough to be heard as a break in
	 * the waveform. 0.5 ms is 16 samples at 32768 Hz, and the total is
	 * capped at 1% of the run: the scheduler is expected to hover right at
	 * the block boundary, so sub-millisecond holes are a sign that it is
	 * working, not that it is broken.
	 *
	 * Two long holes are forgiven. This test runs in real time on a shared
	 * machine, and a host that deschedules the process for a few
	 * milliseconds produces one - it says nothing about the code under
	 * test. The bug this guards is nothing like that: submitting per frame
	 * gave 54 holes and 121 ms of silence in a 2 s run, which trips both
	 * limits several times over. */
	const u64 hole_limit = (u64)(SYSCLOCK_ARM11 * 0.0005);
	const unsigned outlier_allowance = 2;

	int bad = 0;
	if (long_gaps > outlier_allowance || total_gap * 100 > span)
	{
		printf("csnd: FAIL - the stream was interrupted (%u holes over "
		       "%.2f ms, %u allowed; %u%% of the run silent, 1%% allowed); "
		       "blocks are not being submitted before the previous one "
		       "ends\n", long_gaps, (double)hole_limit * ms,
		       outlier_allowance, (unsigned)(total_gap * 100 / span));
		bad = 1;
	}
	else if (long_gaps > 0)
	{
		printf("csnd: %u hole(s) over %.2f ms, ignored as host "
		       "scheduling (up to %u allowed)\n", long_gaps,
		       (double)hole_limit * ms, outlier_allowance);
	}
	if (worst_skew > (u64)(SYSCLOCK_ARM11 / 500))
	{
		printf("csnd: FAIL - the two ears of a block start more than 2 ms "
		       "apart\n");
		bad = 1;
	}
	if (channels_used >= 4 && pairs * 2 < g_host_csnd_count - 2)
	{
		printf("csnd: FAIL - %u of %u blocks were not submitted as a "
		       "stereo pair\n", g_host_csnd_count - pairs * 2,
		       g_host_csnd_count);
		bad = 1;
	}
	return bad;
}

void *linearAlloc(size_t size) { return malloc(size); }
void linearFree(void *mem) { free(mem); }

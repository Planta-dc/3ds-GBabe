# GBEMU43DS: what a fast 3DS Game Boy emulator actually does

Read on request, after it was pointed out that it runs at full speed on the same
console and is, by its own README's admission, written with AI assistance. That
last part is the interesting bit: nothing in the list below is clever. It is all
in the *shape* of the program.

**Licence: GPL-3.0.** GBabe's front-end is MIT and its core is GPL-2.0, and
GPL-3.0 code cannot be linked into that. So nothing here is copied - the code
below was read for technique and everything adopted from it was written from
scratch against this tree's own conventions. File and line references are for
orientation. (Same rule this project already applies to RetroArch and Gearboy.)

## What it does, in its own code

### 1. It never scales a pixel on the CPU

```
ppu.fb_draw   = g_gb_linear_buf[0];     // 160x144 RGBA8, stride 256
ppu.fb_stride = 256;
...
GSPGPU_FlushDataCache(buf, 256 * SCREEN_H * sizeof(u32));
GX_DisplayTransfer(buf, GX_BUFFER_DIM(256, SCREEN_H),
                   g_gb_tex.data, GX_BUFFER_DIM(256, SCREEN_H),
                   ... GX_TRANSFER_OUT_TILED(1) ...);
```
`platform/3ds/source/main.cpp:1266-1300`

The CPU writes the Game Boy's 160x144 into a **linear** buffer. The GPU's own
DMA does the linear-to-tiled conversion (`GX_DisplayTransfer`), and the picture
reaches the screen by drawing one textured quad with `GPU_NEAREST`
(`C3D_DrawImageAt`, same file, ~2790). Scaling, rotation and letterboxing are
one textured quad's job.

Per frame that is 92 KiB of stores in a straight line, and no read-back at all.
GBabe writes 267x240 = 128 KiB down a rotated column layout, gathering the
source with a stride: measured 1.0 ms.

### 2. Its PPU decodes a tile row with two table lookups

```
g_tile_lo[b], g_tile_hi[b]     // 256-entry tables, one byte of tile data -> 8 pixels
u64 v = g_tile_lo[byte1] | g_tile_hi[byte2];
memcpy(out, &v, 8);            // 8 palette indices, no per-pixel work
```
`src/gb_ppu.cpp:129-148`

Then the line is composited with one 32-bit store per pixel against a 4-entry
palette LUT (`bg_lut[ci]`), `src/gb_ppu.cpp:284-299`. Two loads and one shift
per *tile row*, not per pixel.

### 3. Its audio is mixed at the output rate, only when a sample is due

```
static const int SAMPLE_RATE = 11025;
static const int SAMPLE_BASE = CPU_CLOCK / SAMPLE_RATE;   // 380 cycles
static const int SAMPLE_REM  = CPU_CLOCK % SAMPLE_RATE;   // remainder carried
...
if (sample_err >= SAMPLE_RATE) { sample_err -= SAMPLE_RATE; sample_countdown++; }
```
`src/gb_apu.h:6-9`, `src/gb_apu.cpp:416`

Four channels summed, mixed to **mono**, one-pole DC blocker, one `s16` per
11,025th of a second - about 185 samples a frame, each a handful of adds. This
is the single biggest structural difference from GBabe, whose core mixes
deliberately at 2,097,152 Hz for accuracy and leaves 35,112 samples a frame for
the front-end to decimate.

### 4. Its saves happen on another thread

`platform/3ds/source/main.cpp:262-292`: a worker thread with three job slots,
`saver_worker()` + `LightEvent`, so a write to the SD card never blocks a frame.

GBabe's 0.3.1 log has nine frames of **240-790 ms** - that is `log_flush()` and
the 30-second autosave doing exactly that.

### 5. Its pacing is a deadline, a sleep and a short spin

`platform/3ds/source/main.cpp:195-216`: calibrate the tick rate against
`osGetTime()` at startup, then per frame `deadline += frame_ticks`, sleep until
just short of it, spin the last fraction of a millisecond. (GBabe sleeps to a
margin and lets the idle loop poll; the spin is not worth copying, but the
calibration is cheap and it is a second source of truth for the clock.)

## The trade it makes

Its whole emulator core is ~1,976 lines - `gb_cpu.cpp`, `gb_memory.cpp`,
`gb_ppu.cpp`, `gb_apu.cpp`. Gambatte is ~20,000. That is where the 8.4 ms lives
and where the accuracy lives: this port exists because Gambatte 0.5.0 is the
emulator the user considers the best one, and swapping cores to buy 4 ms that
0.3.2 no longer needs would be trading the reason for the project against the
budget.

The parts worth having were never the core. They are the three front-end
decisions above, and all three were independently arrived at here - just later,
and after being told.

## What is being taken

| # | what | where it lands | expected |
|---|---|---|---|
| 1 | one-pass, word-at-a-time decimator | **this commit** | `dec=` 3.0 -> ~1.5 ms |
| 2 | present through the GPU: 160x144 linear, `GX_DisplayTransfer`, textured quad | next | `draw=` 1.0 -> ~0.05 ms, and a much simpler video.c |
| 3 | SD writes on a worker thread | next | the 240-790 ms freezes |
| 4 | tick-rate calibration at startup | optional | a second opinion on `SYSCLOCK_ARM11` |

Not taken: 11 kHz mono (an audible quality cut, and 32 kHz stereo is not what
is costing this port), the busy-wait in `frame_pace()`, and the core itself.

Item 2 is the one with real work in it: it needs citro3d in the build, it
replaces the whole `video_draw_frame()` transpose - and therefore `drawtest`'s
subject - and it puts the picture outside what the host test suite can check
without a GPU. It should be done with the console in reach.

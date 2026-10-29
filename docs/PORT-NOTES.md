# Porting a Game Boy core to the Nintendo 3DS — notes

The original question was "can Walnut-CGB be ported to the Old 3DS?". The
answer was yes, and that is what this port started as (§1 onwards). It then
turned out to be too slow to play, so the core was replaced by Gambatte 0.5.0
while the rest of the port stayed. Walnut was kept for a while as a second
installable, to compare the two on the same console, and then removed once the
comparison had been made: **GBabe is Gambatte-only now** (§7).

This document records why, what was measured, what went wrong on the console,
and what is still missing.

---

## 0. The core is Gambatte 0.5.0

`make` builds the vendored core in `third_party/gambatte/` (GPL-2.0, backend
`source/emu_gambatte.cpp`). There is no second backend anymore; `CORE=` is gone
from the build, and `gbabe-walnut.cia` no longer exists.

Why the change was made, in one line: on the same PC, the same ROM and the same
flags, Walnut needed 1.6x-3.4x more time per emulated frame, and on a 268 MHz
ARM11 the core is the thing that decides whether a game runs at full speed. What
Walnut was still useful for by then — being a second, independent implementation
to compare against — is covered by the reference-image test in §0
("Correctness, not only speed"), which is a better oracle than a second
emulator because it does not share either one's bugs.

Everything below the core is unchanged and was always core-agnostic: video
scaling, the audio FIFO and CSND scheduler, input, `.sav` files, UI, debug log.

### What the switch was worth, measured

`make -C tools/host bench ROM=<rom> FRAMES=300`, same PC, same flags, same ROM,
**ms per emulated frame** (lower is better). "core" is `emu_run_frame()` with
the sound off; "+ APU" is the same with the sound on:

| ROM | Walnut core | Gambatte core | Walnut + APU | Gambatte + APU |
|---|---|---|---|---|
| `dmg-acid2` (DMG, light) | 0.059 | **0.037** | 0.063 | 0.064 |
| `bench.gb` (synthetic DMG) | 0.108 | **0.056** | 0.108 | 0.085 |
| blargg `cpu_instrs` (CGB) | 0.188 | **0.058** | 0.210 | 0.090 |
| blargg `mem_timing` (CGB) | 0.133 | **0.052** | 0.119 | 0.082 |

The emulation itself is 1.6x-3.2x cheaper. With the sound on the win is
1.0x-2.3x, because Gambatte's APU hands the front-end 35112 stereo samples per
frame at 2097152 Hz and something has to decimate that to the 32768 Hz the CSND
channel runs at. `source/emu_gambatte.cpp` does it with two 8:1 box-average
stages (one add per sample, no history buffer), which costs ~0.03 ms per frame
on this PC — on a light ROM that is more than the emulation itself. The
decimation is skipped entirely while the sound is off. If a console log shows
`audio=` close to the frame time, that function is the first thing to look at.

**None of these are 3DS numbers.** An ARM11 at 268 MHz with no L2 cache has a
very different memory system from an x86-64 with 32 MiB of cache, so the table
says which way the cost moved, not how many frames a console will do. The
measurement that matters is the `core=` figure in `sdmc:/3ds/gbabe/debug.log` on
the console.

### Correctness, not only speed

The DMG rendering test ROM `dmg-acid2` is rendered through the real front-end
and compared against the reference image its author ships.

That comparison is a build target rather than something someone ran once:

    make -C tools/host acid2

`tools/check-render.py` compares the two frames **by shade rank** — the
distinct colours of each image are sorted by luminance and replaced by their
index — so selecting a different DMG palette cannot change the result. Only a
pixel with the wrong layer or the wrong shade fails it. The window is fixed
because the ROM has to be rendered at `SCALE_ORIG`, the one scale that puts the
frame 1:1 on the panel.


Pixels differing from the reference, of 23 040, and the number of distinct
colours in the render (a DMG frame has four):

| Build | Differing pixels | Colours | |
|---|---|---|---|
| Gambatte | **0** | 4 | pixel-identical |
| Walnut-CGB, before the fix | 19 291 | 3 | two shades collapsed into one |
| Walnut-CGB, after the fix | **0** | 4 | pixel-identical |

(The Walnut rows are history: that backend was removed once Gambatte had been
measured and verified. CI now runs the check once, for the one core there is.)

The Walnut bug the check caught: its scanline byte carries the shade in bits
0-1 and the layer (OBJ0 / OBJ1 / BG) in bits 4-5, but the DMG path built the
palette index as `((p & 18) >> 1) | (p & 3)`. That reads bit 1 — half of the
shade — as a layer bit and drops bit 5, so shades 2 and 3 landed on the same
colour and the BG row of the palette was never selected at all: a 4-shade
picture came out with 3. The index is now `((p & 0x30) >> 2) | (p & 3)`,
layer * 4 + shade, and the two cores agree pixel for pixel.

The CGB path is unchanged: the same CGB ROM (blargg `cpu_instrs`) renders
identically in both builds, 0 of 96 000 pixels different.

### What the backend has to do

| Concern | How |
|---|---|
| Video | `runFor()` draws a whole 160x144 RGB565 frame into a static buffer (`-DVIDEO_RGB565` on both sides, or the pointer types disagree); the frame then goes to `video_draw_frame()`, so scaling stays in `video.c`. Frame skip passes a NULL buffer and skips the blit. |
| Audio | `runFor()` returns up to 4128 stereo samples per call; the two decimation stages bring them to 32768 Hz and `audio_push_frames()` puts them in the same FIFO the Walnut build uses. |
| Input | `gambatte::InputGetter`; its bit values match `enum gb_button` bit for bit, so the joypad mask is passed through. |
| Saves | `savedata_ptr()`/`savedata_size()` (cart RAM) written to the same `<rom>.sav` the Walnut build uses, so the two builds share saves; `<rom>.rtc` holds the 48-byte MBC3/HuC3 clock. Writes are skipped when a cheap hash says the content did not change. |
| Palette | The "Auto"/manual DMG palettes are pushed per palette index with `setDmgPaletteColor()`, which takes a value in the core's own pixel format (565 here, despite the parameter being called `rgb32`). CGB games ignore it, as on hardware. |
| Boot ROM | `setBootloaderGetter()` reads the same optional `sdmc:/3ds/gbabe/bootrom/{dmg,cgb}_boot.bin` files. |
| Errors | The core has no error callback; `emu_last_error()` returns NULL and the log carries the core's own load messages. |

### Licence consequence

Gambatte is GPL-2.0. The GBabe binary that contains it is therefore distributed
under GPL-2.0 (the project's own sources stay MIT, which is compatible with
that). `third_party/gambatte/COPYING` has the licence text and
`third_party/gambatte/README.md` records the exact upstream commit and the three
local patches.

### What is still unverified

* Frame times on the console: everything above is x86-64.
* The ARM11 build of the C++ core: devkitARM compiles it (CI does that on every
  push), but nobody has measured it here — there is no 3DS in this environment.
* `.sav`/`.rtc` round trips on hardware, for the same reason: the code paths
  run on the host, but no test ROM with battery-backed RAM was available.
* The audio decimator's quality by ear: it is a second-order CIC at 64:1, which
  is the cheap end of "acceptable", not a band-limited resampler.

---

## 1. Why Walnut-CGB suited the 3DS (historical: the first core, since removed)

Things checked directly against Walnut-CGB 1.2.4 (`walnut_cgb.h`, 9 920 lines,
MIT):

| Property | Consequence for the port |
|---|---|
| Single-header, plain C11, no dependencies beyond `stdint`/`string`/`time` | drops into the devkitARM build with nothing to configure |
| Front-end provides five callbacks (`rom_read`, `rom_read_16`, `rom_read_32`, `cart_ram_read`, `cart_ram_write`) plus an error handler | ROM and save RAM are `malloc`'d buffers, so the callbacks are one-liners |
| Rendering through one `lcd_draw_line(gb, pixels[160], line)` hook | we write the 3DS framebuffer directly from that hook; no intermediate 240x400 buffer |
| `gb->cgb.fixPalette[]` already holds native **RGB565** (`WALNUT_GB_RGB565_BIGENDIAN 0`) | the CGB path is a straight LUT copy — and the 3DS top screen can be switched to `GSP_RGB565_OES`, so no per-pixel conversion at all |
| DMG pixels carry shade **and** layer bits (`(pixels[x] & 18) >> 1 | (pixels[x] & 3)`) | free 3x4 "Game Boy Color" colouring of monochrome games |
| `gb->direct.joypad` is a plain byte the front-end may write | input mapping is a bitmask copy |
| `gb_run_frame_dualfetch()` runs one frame and returns | cooperative single-threaded main loop, no threading needed |
| Total emulator state is `struct gb_s` = **50 048 bytes** (DMG+CGB WRAM/VRAM/OAM included) | trivial for the 3DS heap |
| Audio is an optional external APU via `audio_read`/`audio_write` | we use the bundled `minigb_apu`, no new port work |
| Cart RAM is the front-end's buffer (`gb_get_save_size_s()` says how big) | `.sav` files are a `fread`/`fwrite` |

The 3DS side has no unusual requirements either: libctru gives CPU-only access to
the framebuffer (`gfxGetFramebuffer`), HID input, and two sound services. No
citro3d/PICA200 work is needed for a 160x144 picture.

### What the 3DS asks of a front-end

* **Framebuffer is rotated.** The top LCD is a 240x400 panel mounted sideways,
  so landscape pixel `(x, y)` with `y` from the top of the panel lives at
  `(x * 240 + (239 - y)) * 2` bytes in RGB565 — see `source/video.c`.
* **60 Hz display vs 59.7275 Hz Game Boy.** Following vblank directly drifts
  ~0.45 % (≈ 16 s per hour of audio desync). `source/main.c` therefore paces on
  the ARM11 tick counter at the true Game Boy rate, and audio is decoupled
  through a FIFO.
* **No DSP firmware.** NDSP on an Old 3DS requires `sdmc:/3ds/dspfirm.cdc`,
  dumped from the console. CSND does not, so this port uses CSND with its own
  block pump (`source/audio.c`). If CSND refuses to start, the emulator keeps
  running silently instead of failing.

---

## 2. What was measured

`tools/host` compiles `emu.c`, `video.c`, `palette.c`, `audio.c` and `config.c`
against a stub of libctru (same prototypes as the real headers) and runs them on
a PC. Useful numbers from that harness and from a standalone benchmark:

| Measurement | Result |
|---|---|
| `gb_init()` + callbacks + `lcd_draw_line` + `gb_run_frame_dualfetch()` | works unmodified; `dmg-acid2` renders pixel-perfect through GBabe's blit (both in `SCALE_FIT` and `SCALE_WIDE`) |
| Core speed, x86-64, `-O2`, no audio | ~12 700 fps (≈ 210x real time) |
| Scanline blit cost, 5/3 scale (267x240) | ~1.3 % of a core frame on x86 — i.e. dwarfed by emulation |
| `struct gb_s` | 50 048 bytes |
| RGB565 LUT approach | zero per-pixel maths in the CGB path |

The x86 numbers only bound the front-end overhead; they say nothing about ARM11
throughput, which is the open question on real hardware (see §4).

---

## 3. Design decisions worth knowing

* **RGB565 + rotated direct blit.** One pass, no extra buffer, no GPU. Bandwidth
  at the default scale is ~128 KiB per frame; the CPU cost is a 160-entry LUT
  lookup and a store per output pixel.
* **One blit per frame, down a column.** The panel is rotated, so writing a
  scanline touches one halfword per 480-byte destination column: 160 separate
  cache lines, and — worse — 160 partial writes into the same cache line by the
  next scanline. `video_draw_frame()` therefore takes the whole 160x144 frame
  and walks *down* a destination column, which is one contiguous run of `dst_h`
  halfwords per column and 267 runs per frame at `SCALE_FIT`. The nearest
  neighbour mapping is unchanged (`map_x[]`/`map_y[]`), so the picture is the
  same one the per-line path produced — the two were compared byte for byte at
  all three scales before the per-line path was deleted.
* **The letterbox is cleared once per buffer, not once per frame.** The bars
  beside the picture cannot change while the scale does not, and clearing them
  was ~63 KiB of `memset` per frame at `SCALE_FIT` - inside the figure the log
  calls `core`, i.e. charged to the emulator. `video.c` remembers which buffers
  it has already blacked at the current geometry; anything else that writes to
  the top screen calls `video_letterbox_unknown()`, and the stress test fails if
  that notification is dropped.
* **Present on the vblank, and wait for it.** `video_end_frame()` flushes the
  back buffer, asks for the swap, and then waits for the panel to take it.
  Without the wait the next frame starts drawing into the buffer that is still
  being scanned out, and the tear line *moves*: 59.7275 Hz against a 60 Hz panel
  means the phase drifts, so the tear slides down the screen instead of sitting
  still. Measured on the host, before the fix every one of 60 consecutive frames
  drew into the displayed buffer; after it, none. See §7.
* **One screen each, and only one.** The game writes the top framebuffer and
  the UI writes the bottom one; each screen is flushed and swapped on its own.
  It has to be that way: `gfxFlushBuffers()` and `gfxSwapBuffers()` act on
  *both* screens, so calling them per frame flushes 153 KiB of unchanged
  bottom-screen buffer and flips the bottom buffer out from under the UI (with
  double buffering, the screen then shows the other buffer - a frame that is
  one old, or never drawn). `video_end_frame()` and `ui_present()` therefore
  call `GSPGPU_FlushDataCache()` plus `gfxScreenSwapBuffers()` for their own
  screen, and nothing else. `tools/host`'s stub models double buffering the way
  libctru does, so `make -C tools/uipreview asan` fails if either side starts
  swapping both.
* **The letterbox is cleared every frame.** `video_draw_frame()` writes only the
  scaled picture rectangle, and libctru hands out framebuffers from
  `linearAlloc()` without clearing them, so with the default "fit height" scale
  the ~66 px beside the picture would show uninitialised memory. A column of the
  rotated panel is `FB_STRIDE` contiguous halfwords, so the bars are cleared
  with two `memset`s; the pixel-perfect mode clears the whole screen instead.
  The top screen is also blacked at startup and whenever the browser is open.
* **Frame skip = render skip.** `emu_set_rendering(false)` swaps the core's
  `lcd_draw_line` pointer to `NULL` (the core explicitly supports this), so a
  skipped frame costs no blit and never leaves two hardware buffers showing
  different frames.
* **CSND block scheduler.** 548 (or 549) stereo samples are pushed per emulated
  frame into a 4096-frame FIFO. 1024-frame blocks then go out on a *deadline*,
  not "whenever a frame ends": the frame loop is idle ~11 ms out of every
  16.7 ms and `audio_next_deadline()` tells it when to wake up, so a block is
  submitted just before the previous one runs out. Stereo needs two channels
  (a CSND channel is mono, with separate left/right volumes for one sample), and
  two slots alternate so the next block is already playing. Underruns resync
  instead of playing stale audio, and are counted in the log. See §7.
* **Config as text.** `sdmc:/3ds/gbabe/config.cfg`, `key=value`, so a bad setting
  is fixable without recompiling.

---

## 4. Open questions / risks

1. **CGB speed on an Old 3DS.** The core is an interpreter with an optional
   dual-fetch dispatch and 32-bit DMA paths. DMG should be comfortably
   full-speed; CGB (double-speed mode, more VRAM traffic) is the case to watch.
   Mitigations already in place: `-O3 -flto`, `-march=armv6k`, a power-of-two
   mask instead of a software divide in the cartridge-RAM callbacks, and frame
   skip in the menu. Whatever is next depends on the HUD split (below): more
   core speed means a dynarec or a lower `-O` trade, less blit means a different
   scale default.
2. **The UI was the real bottleneck, not the emulator.** The first report
   from hardware was that the file browser was too slow to reach a ROM at all.
   Nothing in the browser was doing heavy work in the obvious sense; the cost
   was in how it talked to libctru:

   * it rescanned the directory on **every key press** - `opendir`/`readdir`
     plus a `stat()` per entry, each of which is an IPC round-trip to the SD
     filesystem. On a card with a few hundred entries that alone is a second
     per tap;
   * libctru's console calls `gfxFlushBuffers()` (a 300 KiB framebuffer flush)
     on every `\r` and `\n`, and `consoleClear()` redraws 1200 glyphs and
     flushes again. Per-line `printf` plus cursor addressing per redraw
     multiplied both.

   Two rounds of fixes followed. First the console was made to behave: one
   `fwrite` of a 1200-byte shadow screen with no newlines, one flush, the
   directory scanned only when it changes, ROM-named entries never `stat()`'d.
   That made it navigable. Then the console was dropped altogether - see §5 -
   because even a well-behaved text console redraws glyph by glyph and cannot
   draw a selection highlight, an icon or a smooth bar.

   The lesson generalises past this port: on an Old 3DS, **per-character text
   output is the expensive thing, not pixels**. A full 320x240 RGB565 canvas
   costs 0.2 ms to compose and 0.3 ms to present, i.e. ~3% of a frame, and it
   only happens when something changes.

3. **Where a frame goes.** The HUD now reports the split (core / blit /
   wait) once a second, because tuning this thing without a console in hand
   means guessing. What the host harness says, and therefore what the code is
   shaped around:

   * emulating a DMG frame costs ~0.06-0.10 ms on a desktop x86 (10-16k fps),
     so the interpreter is not what is slow on a PC; an Old 3DS is one to two
     orders of magnitude slower, which is the whole question. `tools/host bench`
     prints the same split as the HUD so a change can be compared without the
     console, but it cannot answer what the ARM11 does;
   * the scaled copy into the 3DS's column-major framebuffer is *as expensive
     as the whole emulator core* on the same machine (~0.03 ms, a third to a
     half of a presented frame) because every store lands 480 bytes from the
     last one. Two rewrites (a transposed blit, then a cache-tiled transpose)
     both came out slower than the simple version on the only machine this port
     could be measured on, so it stays simple until the HUD numbers from real
     hardware say otherwise;
   * compiler flags are *not* the lever this was first believed to be: with a
     real ROM, `-O2`, `-O3` and `-O3 -flto` come out within 2% of each other
     on the host (`make -C tools/host bench ROM=...`). An earlier note here
     claimed 45%; that was one unrepresentative measurement and is withdrawn.
     LTO stays on because the core is one enormous switch in a header-only
     library and the win should be larger where the inliner is weaker, but on
     the evidence available it is a bet, not a measurement;
   * the pacing bug that cost the most was a unit error: `svcSleepThread()`
     takes nanoseconds and was being handed ARM11 ticks, i.e. 3.7x too short, so
     the loop spun through the whole idle period instead of sleeping;
   * the readout used to hide overhead: core + blit + wait did not have to add
     up to the frame time, so anything else in the loop (a blocking service
     call, a syscall) simply made "fps" lower with no explanation. The HUD now
     reports the unaccounted remainder as *other*, shows the worst frame in the
     last second (a smooth average hides a stutter), and names the bottleneck
     in words: "core", "draw", "overhead" or "full speed".
   * one such overhead was real: the CSND pump called `CSND_UpdateInfo(true)`
     and `csndIsPlaying()` on **every frame** - 120 service calls a second, each
     of which waits for the command queue - to decide whether a 31 ms audio
     block had finished. It now tracks the block's deadline and asks once per
     block, and the FIFO is copied with `memcpy` instead of a sample at a time.

4. **Audio latency.** CSND blocks of 1024 samples ≈ 31 ms; the FIFO adds up to
   ~100 ms of slack. Tunable, but until this runs on hardware the exact numbers
   are estimates.
5. **The ARM build compiles in CI, but nothing has run on hardware yet.**
   devkitPro is not installable in the environment this port was written in, so
   the Makefile was written against libctru's headers and devkitARM's 3DS
   template; the platform-independent code was compiled and executed on the
   host. The first real-hardware run should expect small fixes (framebuffer
   orientation flags in `video.c`, CSND channel choice, console font metrics).

   `.github/workflows/build.yml` now produces both artifacts on GitHub's
   runners (`.3dsx` always, `.cia` when requested), and pushes a tagged build
   straight to a release with a QR code FBI can scan. The build runs inside
   `docker/gbabe-devcontainer.Dockerfile`: a pinned devkitARM plus
   makerom/bannertool compiled from pinned sources, cached in GHCR and tagged
   with a hash of its own Dockerfile, so the toolchain cannot change under a
   commit and a local build matches CI byte for byte. That image is also the
   devcontainer (`.devcontainer/devcontainer.json`), and `docker/ci-build.sh` is
   the single entry point both use. If the image is missing or broken the
   workflow builds it; failing that it falls back to the stock
   `devkitpro/devkitarm` image, which `ci-build.sh` sets the packaging tools up
   in itself.

   The packaging inputs are checked offline rather than in CI, by feeding a
   synthetic ARM ELF (`tools/make-test-elf.py`) through the same `bannertool`
   and `makerom` invocations that `make cia` uses: the RSF parses, every `$(...)`
   placeholder resolves from the Makefile's `-D` flags, `resources/banner.png`
   is exactly 256×128, `icon.png` is exactly 48×48, and the 32768 Hz mono
   `resources/banner.wav` converts to CWAV. (Every one of those is a hard error
   in the tools, so the rehearsal is worth keeping when the assets change.)
   What still cannot be checked offline:

   * whether the service list is complete at runtime — a missing entry shows up
     as a `Result` error from `csndInit()`/`fsInit()` on first boot;
   * whether the `.cia` installs and boots on hardware.

   Two CI lessons worth recording, because both cost a debugging round and
   neither is visible from the workflow file:

   * the container is Debian bookworm (glibc 2.36) while both tools' published
     Linux archives are built on Ubuntu 24.04 (glibc 2.39), so the prebuilt
     binaries do not execute there at all. The install script therefore
     compiles them, and runs each one once so a broken tool fails loudly
     instead of being masked;
   * inside that container the default `run:` shell is **dash**, and `/bin/bash`
     is not on `PATH` for `run:` steps either, so a step written for bash
     (`set -o pipefail`, arrays, `[[ ]]`) fails with "Illegal option" and exit 2
     *before* any of its commands run - which looks exactly like the build
     command itself failing. Every step in that job is now POSIX sh, including
     the packaging-tools installer, and the `.cia` step does not depend on
     `pipefail` either way.
6. **Not implemented:** save states, link cable, cheat codes, SGB border/colour
   tables (`third_party/walnut/extras/sgb.h` is available for a future
   per-game palette table), MBC7/rumble, rewinding.

---

## 5. The bottom screen is drawn, not printed

The browser, menu and status readout are a software-rendered UI in a 320x240
RGB565 canvas (`source/ui_gfx.c`, `source/ui_screens.c`, `source/ui_font.c`).
The layout language - dark panels, one accent colour, value pills, a footer of
key hints, touch hit-testing - follows bheppe's
[gpSP 3D](https://github.com/bheppe/gpsp-3d); the code is written from scratch
and only the look is shared (gpSP 3D is GPLv2, this is MIT).

* **Text.** Five anti-aliased faces (11/13/13/17/26 px) baked into 4-bit
  coverage atlases by `tools/make-font.py`, so there is no font file to load and
  no runtime rasteriser: a glyph is a nibble lookup, a blend and a store. The
  atlases are ~136 KiB of source and cost nothing at runtime.
  DejaVu Sans is the source font (permissive licence, `third_party/fonts/`);
  the generator takes any TrueType file.
* **Shapes.** Rounded rectangles and circles are drawn with integer edge
  coverage: the corner distance is compared against the circles of radius
  `r-1` and `r+1`, which gives anti-aliasing without a square root, without
  floats and without a per-pixel table.
* **Present.** One `ui_present()` copies the canvas into the bottom
  framebuffer in 16x16 tiles - the bottom LCD is also a rotated panel, so this
  is a transpose with flipped rows - then flushes only that screen's 153 KiB and
  swaps. Nothing is drawn per frame during gameplay except when the status
  readout refreshes, once a second.
* **Redraws.** Screens are composed when something changes (a key, a tap, a new
  directory), never per frame; the browse/menu loops block on `hidScanInput()`
  and only touch the canvas when their state actually moved.
* **Touch.** The 3DS reports touch coordinates in the same 320x240 landscape
  space the canvas uses, so hit-testing is `scr_browser_hit()` /
  `scr_menu_hit_row()` on the layout constants - no second geometry to keep in
  sync, and every row is a large target. In the browser one tap opens the
  highlighted entry; in the menu a tap selects and a second tap on the same row
  uses it, because "Reset game" and "Save and quit" must not fire on a stray
  thumb. The touch bit is taken from `hidKeysDown()`, not
  `hidKeysDownRepeat()`: repeat re-emits held keys, and a finger resting on the
  glass reads as "held", which would have activated whatever it was touching
  every 15 frames.
* **Offline preview.** `tools/uipreview` links the real `ui_gfx.c`,
  `ui_screens.c` and `ui_font.c` against the host stub and renders every screen
  to an image, through the same rotation the hardware applies. CI attaches the
  PNGs as an artifact, which is how these layouts were designed without the
  console to look at them on.

Cost on the host, for calibration: a full screen compose is ~0.2 ms and the
transpose+flush ~0.3 ms. Even an order of magnitude slower on an Old 3DS, a
keypress costs a small fraction of a frame - against roughly 400 ms per redraw
for the same screen printed through the text console.

---

## 6. Debugging a console you cannot attach to

Everything in this port was written without a 3DS in hand, so the question
"why is it slow on *your* console" has to be answerable by the person holding
it. Two things make that possible:

* **the status readout** (§5) names the bottleneck in words, so a screenshot is
  enough to triage;
* **`sdmc:/3ds/gbabe/debug.log`** (`source/log.c`) records what a screenshot
  cannot: one line a second with the whole frame split, a line for every frame
  over 20 ms with the audio queue depth, every settings change, every ROM
  loaded with its header details and path, CSND start-up failures and the
  emulator core's own errors.

The log is written to be free at runtime: lines are formatted into a 2 KiB RAM
buffer and pushed to the card only every few seconds (or immediately for
events, so a crash does not lose them), it stops at 384 KiB, and it writes
nothing at all once switched off. `make -C tools/host logtest` checks the
format, because the file is the interface: a report is read from it by hand.

That is also why the numbers it prints are defined precisely. `core` is the
emulator core (CPU + PPU + APU), `blit` is the scaled copy, `post` is the
once-a-second work (status readout, log, cartridge save), `sync` is waiting for
the panel to take the finished frame and `wait` is pacing - since §14 both of
those happen in the same idle window, so `sync` is reported as part of `wait`
rather than as frame time. `other` is the rest of the frame, which is where a
blocking service call would show up. A frame
that is slow with a large `other` is the port's fault; a large `core` is the
ARM11 and needs a dynarec, not tuning.

---

## 7. The two bugs the console found, and how they are tested now

Both were reported from hardware as "the graphics slide" and "the sound is
wrong". Neither was an arithmetic bug in the emulation: both were about *when*
something happened, which is why neither showed up in any of the checks that
existed at the time - they all compared still frames.

### The picture tore, and the tear slid down the screen

libctru's double buffering is a two-deep swap chain. `gfxGetFramebuffer()`
returns the buffer that is *not* displayed, and `gfxScreenSwapBuffers()` flips
the caller's idea of which one that is **immediately**, while the panel only
switches at the next vblank.

So the loop was: draw into B, ask for a swap, and — right away — draw the next
frame into A. For the whole of that frame, A was still the buffer the panel was
scanning out, so the scanout and the writes raced down the same memory; the tear
appeared wherever the scanout happened to be. And because the Game Boy runs at
59.7275 Hz against a 60 Hz panel, the position of that race drifts by 0.45 % of
a frame every frame - the tear line slides.

The fix is one wait: the panel has to take the swap before the buffer it
released is written to again, and `gspIsPresentPending()` says whether it has.
Where that wait happens moved in §14 - it belongs in the idle time before the
next frame, not inside the frame - but the reasoning here is unchanged: it is
kept out of the `blit` figure in the log (`sync`), or it would look as if the
copy had got 12 ms slower.

The wait is skipped when the frame has already overrun its own 16.74 ms period
(`video_end_frame()` compares against `video_begin_frame()`). Waiting then would
land on the *next* vblank and hold the frame for 33 ms, so the game would drop
to 30 fps exactly when the console is struggling - a tear is the lesser evil.

Waiting for the panel does not force the game to 60 Hz. The frame loop paces
itself off the ARM11 tick counter at 59.7275 Hz and the wait only fills the idle
gap, so the emulated clock stays exact. What cannot be avoided is the beat
between 59.7275 and 60: about once every 3.7 s a frame is presented just after
a vblank has gone by and the panel shows it for two panel periods. That is one
frame held 33 ms instead of 16.7 ms, and no amount of scheduling removes it
short of running the Game Boy 0.45 % fast or interpolating frames.

The host harness counts torn frames - a frame that fetches the top framebuffer
while a swap is still pending - and `make -C tools/host presenttest` covers both
halves of the decision, including the deliberate exception: a frame that has
already overrun its own 16.74 ms period skips the wait and presents at once
(one torn frame beats 30 fps). The same test confirms the wait comes back on the
next on-time frame.

| | torn frames out of 60 |
|---|---|
| before | 59 |
| after | **0** |

### The sound was pitch-shifted and stuttering

Two separate mistakes, both in `source/audio.c`:

1. **A CSND channel is mono.** It has one volume word holding a left and a right
   volume for the *same* sample. Feeding it interleaved L,R data does not play
   stereo; it plays the interleaved samples as a mono stream - so every sample
   is played at twice the rate, the whole thing is an octave high, and each
   block ends in half the time it should. Stereo needs two channels, one per
   ear, started together.
2. **A block was submitted once per emulated frame.** A channel plays one buffer
   and stops; a block is 31.25 ms and a frame is 16.7 ms, so the submit landed
   up to a whole frame *after* the previous block had ended. That is a silence
   hole every 31 ms - a 32 Hz stutter on top of the pitch problem.

The fix for the second one is the interesting one: the submit time is a
deadline, not a side effect of a frame ending. `audio_next_deadline()` returns
when the next block is due, the frame loop sleeps to whichever comes first (the
next frame or that deadline), and the block is handed over `margin` early, where
`margin` is the measured `csndPlaySound()` round trip plus the worst recent
wake-up. Two slots alternate, so the next block is playing before the current
one runs out. Being early means a brief overlap of the same audio, which is
inaudible; being late means a hole, which is not - so the error budget is spent
on the early side. The FIFO is kept at two blocks (62 ms) rather than bone dry,
so an occasional late wake-up does not become an underrun.

None of that can be heard in CI, so `make -C tools/host audiotest` measures it:
the stand-in CSND in `tools/host/stub.c` plays each block in real time and
records when it started, and the test looks for holes in the union of those
intervals.

| | holes | total silence | worst hole |
|---|---|---|---|
| per-frame submit (the old way) | 54 | 121 ms | 2.30 ms |
| deadline-driven submit | 28 | 0.54 ms | 0.07 ms |

The remaining holes are sub-millisecond - 20-odd samples at 32768 Hz - and are
what a scheduler that parks right at the block boundary looks like. The test
allows 0.5 ms per hole and 1 % of the run in total, and runs twice: with four
CSND channels (stereo) and with two (the mono fallback, for when the service
will not give up four).

---

## 8. If you want to do the same port from scratch

The recipe this project ended up with, in order:

1. Take a core that is fast enough before you take one that is convenient.
   Measure it on the host against a real ROM; on a 268 MHz ARM11 with no L2 the
   interpreter is the whole budget.
2. Draw straight into the rotated framebuffer in RGB565, down a destination
   column (§3), and present on the vblank with a wait (§7).
3. Pace the emulator with `svcGetSystemTick()` at 59.7275 Hz, not with vblank -
   the panel is 60 Hz and the drift is real.
4. Feed CSND from a FIFO on a deadline, stereo, two channels per ear (§7).
5. Draw the UI yourself (§5). A text console is fine for a splash screen and too
   slow for anything a user navigates on an Old 3DS.
6. Log to a file (§6). You will not have the console, and neither will whoever
   reports the first bug.
7. Make the still-picture checks and the timing checks part of the build, not
   something you ran once (`make -C tools/host acid2`, `audiotest`).
8. Read the first log from real hardware as if it were a bug report, because it
   is: the numbers in §9 found two more bugs and explained a third symptom.

---

## 9. What the console actually reports

The first debug log from a real Old 3DS run of 0.2.0 (CGB title, `SCALE_FIT`,
no frame skip, 205 seconds) is worth reading line by line, because it is the
first measurement of this port that was not taken on a PC:

```
[    1.225] perf fps=59.8 frame=16.7 worst=18.7 core=10.0 blit=0.1 sync=6.6
                wait=0.0 other=0.0 audio=55ms sync_skip=0 sync_late=0
[    8.914] slow frame 26.2 ms: core 10.6 blit 0.1 vblank 15.6 audio 41 ms
[   10.518] perf fps=52.9 frame=18.9 worst=33.4 core=15.1 blit=0.1 sync=3.1
                wait=0.6 other=0.0 audio=62ms sync_skip=3 ...
[   12.510] audio: 301 blocks, late 207 (worst 648.96 ms), underruns 8 ...
```

| what | measured on the console | what it means |
|---|---|---|
| `core` | 8.6 - 16.5 ms | the interpreter, scene dependent. Inside the 16.74 ms budget, but not by much in heavy scenes |
| `blit` | 0.1 ms | the whole-frame column-major blit is free. The PC said 0.05 ms; the console agrees |
| `sync` | 3 - 8 ms, occasionally 16.6 | the vblank wait. In 0.2.0 it was taken out of the frame; §14 moved it into the idle window, so it is part of `wait` in the log now, not extra frame time |
| `fps` | 57.8 - 58.8 | **not** 59.7275, which is the one number that explains the rest |
| a 26 ms frame | about once a second | 10 ms of core plus a whole 16.7 ms panel period: the swap landed just after a vblank |
| `sync_skip` | 0 in light scenes, 1 - 8/s when `core` >= 14 | frames that had to be presented without the wait: the tearing that is left |
| `underruns` | ~0.6/s, `late` on 60 % of blocks | the APU produces one frame of samples per emulated frame, so an emulator running 1.5 % slow starves CSND 1.5 % of the time |

So the sound stutter is not an audio bug any more - it is downstream of the
frame loop running at ~98.5 %. Fixing the pacing fixes both, which is why the
log line carries `pace_late` and `resync` now: they say whether the loop starts
frames late (overruns) or throws away backlog (a visible jump), and the next
round of work can be aimed with those two instead of a guess.

Two other things in that log were real bugs, both invisible on the PC because
`tools/host/3ds.h` used to define `SYSCLOCK_ARM11` as a `double` while libctru
defines it as an integer constant:

* `tick rate 0 Hz` - `%.0f` was handed an `int`. The stub now uses libctru's
  integer, so a format mismatch fails on the host too.
* `AUTOSAVE_TICKS` was `SYSCLOCK_ARM11 * 30` = 8.04e9, which overflows the
  32-bit `unsigned int` the constant has on hardware and wrapped to 3.75e9 -
  an SD write every 14 seconds instead of every 30.

The ROM title in the log was `"BUGS BUNNY4AO4E\xff"`: the header is space padded
and may contain anything above 0x7F, and the loader only stopped at control
characters. It stops at the first byte outside printable ASCII now, and trims
the padding.

The same log has three stalls of 0.67 s, 4 s and 50 s in `wait`, all of them
outside any frame (the emulator was not running slowly, the console was not
running at all). Those are the lid, the HOME menu or a system applet; the loop
resyncs after them rather than trying to catch up, which is what the `resync`
counter is there to confirm.

---

## 10. The frame loop was never keeping time

The pacing code in `main.c` looked harmless and had a bug nobody could see from
the outside: an unsigned comparison used for a signed question.

```c
next_frame_tick += FRAME_TICKS;
if (now - next_frame_tick > 4 * FRAME_TICKS)   /* "four frames behind" */
        next_frame_tick = now + FRAME_TICKS;   /* start a new schedule  */
```

When a frame starts a little *before* the following slot - the normal case,
and the one the sleep is calibrated for - `now - next_frame_tick` is negative,
so in `uint64_t` it wraps to about 2^64 and the test fires. The loop then
re-anchored its schedule to `now + FRAME_TICKS`, i.e. "one frame after this
frame started", for **every single frame**.

That turns an absolute schedule into a relative one, and a relative schedule
cannot recover: a frame that overruns by 3 ms moves every frame after it 3 ms
later, for ever. Simulated with 10 ms frames and every tenth frame at 20 ms
(heavy but still inside the 16.74 ms budget between them):

| | frames/s | schedule re-anchors |
|---|---|---|
| unsigned comparison | 58.59 | 600 of 600 |
| signed (fixed) | **59.71** | 0 |

58.6 fps is what the console reported - 98 % speed, and the reason the log's
`audio:` line showed underruns at the same time (the APU produces one frame of
samples per emulated frame, so an emulator running 1.4 % slow starves CSND
1.4 % of the time). It was not a sound bug, and it was not the core being slow.

The fix is a signed comparison, and the arithmetic now lives in
`include/pace.h` where the host can test the cases that matter:
`make -C tools/host pacetest` covers a frame that is early (no resync - the
regression), one that is late (counted, schedule unmoved), jitter, a 200 ms
stall (one resync), the three-and-four-frames-behind boundary, and a million
frames without drift. The same file asserts the autosave interval, which had
its own overflow bug (§9).

Two lesson-shaped notes. First: the counter added in §9 to *find* this would
have reported 100 % of frames as resyncs, which is how it was found - a metric
that reads implausible is a bug in the metric or in what it measures, and this
time it was both. Second: this is exactly the kind of arithmetic that should
never live inside a loop that can only be exercised on hardware you do not have.

---

## 11. The once-a-second work was costing a frame a second

The second log from hardware, same Old 3DS, with §10 in it:

```
[    1.128] perf fps=59.8 frame=16.9 worst=33.4 core=9.2 blit=0.1 sync=7.3
                wait=0.3 other=0.0 audio=39ms sync_skip=0 sync_late=0
                pace_late=55 resync=0
[    3.167] slow frame 26.4 ms: core 9.5 blit 0.2 vblank 16.7 audio 34 ms
[   10.153] perf fps=56.8 frame=17.6 worst=66.9 core=9.0 ... resync=1
```

The pacing fix worked - `resync` is 0 almost everywhere, `fps` is 59.8 where
it used to be 57.8 - but two numbers in it are still wrong, and they turned
out to be the same thing.

`worst=33.4` in *every* second. A 33.4 ms frame is two panel periods, i.e. a
frame that missed its vblank and had to wait for the next one. One per second,
not one per 3.67 s (which is what the 59.7275-vs-60 Hz beat alone would give),
so something in the loop happens once a second and costs a vblank.

The log has the answer in its own line ordering: of the 117 `slow frame` lines
between 26 s and 320 s, 110 come within 60 ms of a `perf` line, with a median
of 33 ms. The `slow frame` line is rate-limited to one a second, so this is
not the whole population - but the trigger is obvious once asked for: the
`perf` line is written by `ui_note_frame()`, which at the one-second mark also
calls `ui_draw_hud()` - a bottom-screen redraw plus a 153 KiB cache flush -
and it did it *after* `video_end_frame()` had waited for the vblank, in the
~0.1 ms of slack the loop has left at that point. It could only ever be paid
for by missing the next vblank.

The loop had 7.3 ms of dead time per frame - the average `sync`, spent
blocked in `gspWaitForVBlank()` doing nothing - and the fixed budget was being
spent right after it.

So the swap and the wait for it are now two calls:

```c
video_end_frame();     /* hand the frame to the panel            */
ui_idle_work();        /* redraw the status/HUD if due, 1/s      */
log_flush();           /* the line buffered up over the last s   */
audio_log_stats();
emu_flush_save();      /* every 30 s                             */
video_present_sync();  /* wait for the vblank - absorbs all of it */
```

`video_present_sync()` is also called by `video_begin_frame()`, so no other
caller can tear by forgetting; `main.c` calls it first only to get the wait
for free. `presenttest` grew a check for exactly this: two frames with 6 ms of
"once-a-second work" between the swap and the wait still present and still do
not tear.

The `perf` line gained `post=` for that work, between `blit` and `sync`, so
the next log says how much it was. If `post` climbs above about 7 ms the work
no longer fits in the gap and the frame rate pays for it, which is the number
to watch.

Two things this deliberately does *not* do. It does not move the frame start
onto the vblank: that would lock the emulator to the panel's 60 Hz and run the
Game Boy 0.45 % fast, and it is a much larger change than the log justifies
yet. And it does not touch `pace_late`, which still reports 55-60 of 60 frames
a second: either the loop really is starting frames a frame late, or the
counter measures the wrong thing once the frame is being presented at the
panel's pace rather than the schedule's. The next log will say which, because
`post` removes the delay that was making it plausible.


---

## 12. Versions, and telling two builds apart

The version sat at 0.2.0 through the whole of §10 and §11 — a rewritten frame
loop, a restructured present path, a version-string bug, and a dozen fixes —
because nothing forced it to move. It is 0.3.0 now: the frame loop is a
different design and the present path is a different shape, which is a minor
bump rather than a patch.

The lesson worth keeping is the one from §9's table: a user cannot tell two
builds apart by a version number that only changes at releases. So the log
identifies its own binary now:

```
# GBabe 0.3.0 build 20261028-9562afd debug log
[    0.018] GBabe 0.3.0 build 20261028-9562afd, Old 3DS/2DS, installed title (cia)
```

`GBABE_BUILD` is stamped in by the Makefile as `date -u +%Y%m%d` plus
`git rev-parse --short HEAD`, with `nogit` for a tree unpacked from an archive
and `dev` for the host tools. It exists because the one thing a bug report has
to answer first - *which binary wrote this log* - could not be answered before.

`check-version.sh` still refuses any version typed into a source file, so the
stamp is the only new string and it comes from the build, not from the source.

---

## 13. The status readout was spending a frame a second, and §11 did not fix it

The third log from hardware (0.3.0, Croc 2, CGB, 191 s) says the same thing
as the second one, only more precisely, because §11 added a counter for it:

| | third log |
|---|---|
| `worst` | **33.4 ms or more in 157 of 157 seconds** |
| slow frames | 150 in 191 s, one every **1.003 s** |
| `post` (in the slow-frame lines) | **16.4 ms** median |
| `core` | 10.0 ms median |
| `vblank` | 5.9 ms median |
| `core + post + vblank` | 33.0 ms - the whole dropped frame |

So the diagnosis in §11 was right about *what* and wrong about *how much*.
Moving the once-a-second work into the gap between the swap and the vblank wait
is only free if the work is smaller than the gap. The gap is ~6 ms. The work is
16 ms. It was never going to fit, and moving it only made it visible - which is
worth something, but it is not a fix, and the frame was still lost.

Two other things in §11 were simply wrong, and the log is what caught them:

* the `perf` line was documented (in the commit, the pull request and the
  README) as having gained a `post=` field. It had not: the edit that would have
  added it silently did nothing, so `post` appeared only in the `slow frame`
  lines and the whole 16 ms sat inside `other` in the once-a-second summary. The
  field is there now, and `other` no longer absorbs it.
* "`post` should sit around 1-2 ms" - asserted, not measured. It is 16 ms.

### What the 16 ms is

`ui_present()` transposes the 320x240 canvas into the bottom panel's 240x320
layout - 76 800 pixel writes, strided by a panel row - and then flushes 153 KiB
of framebuffer cache so the GPU does not read stale pixels. On a 268 MHz ARM11
that is about 16 ms, once a second, for a debug readout.

### The fix: one band per frame

Both halves of the present are now done in bands of panel *columns*, which are
contiguous in the framebuffer, so each band is one region and one
`GSPGPU_FlushDataCache()` call:

```
ui_present_begin();                     /* take the back buffer          */
ui_present_band(UI_W / 8) x 8 frames    /* 2 ms each, inside the gap     */
ui_present_swap();                      /* show it - nothing before this */
```

Eight frames of ~2 ms rather than one frame of 16 ms. The panel keeps showing
the previous buffer until the swap, so a banded update is never visible
half-done, and `ui_present()` still exists as the immediate whole-screen path
for menus.

The `hud:` log line reports the split every second:

```
hud: render 0.9 ms + present 16.8 ms in 8 bands (worst band 2.1 ms)
```

`present` staying at ~16 ms while `worst band` stays near 2 ms is the success
condition: the work still costs 16 ms, but it is paid in eight slices that fit
in the waiting, so no frame is lost to it. If `worst band` approaches the frame
period, the band count needs to go up.

### The test

`tools/uipreview` now checks the banded present against the one-shot present at
band widths 320, 7, 40 and 1, with a canvas that differs in every pixel, and
checks that the bottom screen does not change until the swap. Dropping the last
column of every band leaves `present: ok` - the whole-screen path hides it -
and fails the banded check with 9600 wrong pixels, which is why both exist.

### The lesson

A fix that moves work into slack has to know how much slack there is. §11
assumed 1-2 ms because it was a text redraw; it is 16 ms because it is also a
153 KiB cache flush, and nobody had measured the flush. The counter came first
and the number was in the next log, which is the only reason this section can
state it.

---

## 14. What the old RetroArch 3DS port does, and the one thing worth taking

RetroArch's 3DS driver is the largest body of working 3DS emulator front-end
code there is, so it is worth reading before inventing anything. Read at
v1.6.9 and v1.7.7 (`gfx/drivers/ctr_gfx.c`, `audio/drivers/ctr_dsp_audio.c`,
`audio/drivers/ctr_csnd_audio.c`) - the versions from before the port grew a
citro3d shader pipeline, an OSD and a threaded GPU.

What it does, and whether it applies here:

| RetroArch | applies to GBabe? |
|---|---|
| Core draws into a **linear** texture; `GX_DisplayTransfer` / `ctrGuCopyImage` moves it to the swizzled framebuffer, the GPU does rotation and scaling with a textured quad | **No, not for speed.** Our blit is 0.1 ms - already free. It would buy cache pressure, not time, and cost a GPU rewrite on hardware we cannot test. |
| Audio through **ndsp** (the DSP service, `ndspChnSetRate(32728)`, `NDSP_INTERP_NONE`, one looping wave buffer) or CSND | Maybe later. The DSP's native rate is 32730 Hz, so the core resamples to it and the hardware does not. Ours resamples to 32768 and CSND interpolates. Worth a measurement, not a guess - and note their own comment marks `ndspSetClippingMode` with "??". |
| Pacing by `svcGetSystemTick()`, video driven by the core's own frame rate, `gspWaitForEvent(VBlank0)` only when vsync is on | **Yes - and this is the one.** |
| `-O3`, `-fno-exceptions -fno-rtti`, no RTTI in the core | Already done here. |

### The one that matters

`ctr_gfx.c` has a long comment admitting the framework's own vsync problem:

> If we are running at below the display refresh rate, then frame updates will
> happen entirely out of sync with VBlank0 events. […] we'll wait for a VBlank0
> here, but it will already have happened partway through the previous frame
> […] resulting in 'overlap' and screen tearing. […] This fixes screen tearing,
> but it has a significant impact on performance.

They hit the same wall from the other side: **you cannot both block on the
panel's vblank and keep an emulator of a different frame rate on time.** Their
workaround is to wait on the *next* vblank, which they say costs performance.

§7 of these notes chose the same trade in the other direction: block after every
swap, and accept a tear only when the frame has already overrun. That is why the
console log reads

```
perf fps=57.8 frame=17.3 worst=33.4 core=13.2 blit=0.1 post=0.2 sync=5.7 wait=0.0 ...
```

`frame=17.3` is the frame *period*, and 1000/17.3 = 57.8 - the whole reported
frame rate is the period, and 5.7 of those 17.3 ms are spent blocked in
`gspWaitForVBlank()` after the swap. The Game Boy's frame is 16.74 ms, so
10.7 ms of work + 5.7 ms of waiting does not fit, the loop never sleeps
(`wait=0.0`), and every frame starts late (`pace_late=58`).

### The change

The swap and the wait for it are now separated by the *whole rest of the frame*,
not by a few microseconds of bookkeeping. `video_end_frame()` posts the swap and
returns. The loop goes back to the top, finds the next frame is not due yet, and
spends that idle time - which it would have slept through - draining the swap:

```c
if (!frame_due()) {
        ...
        if (video_swap_pending()) {          /* poll, do not block */
                const u64 slice = FRAME_TICKS / 64;   /* ~0.26 ms */
                if (now + slice < wake) wake = now + slice;
        }
        svcSleepThread(TICKS_TO_NS(wake - now));
}
```

`video_swap_pending()` is a non-blocking `gspIsPresentPending()`, so the loop
wakes a few times, sees the panel has taken the frame, and goes back to sleep.
`video_present_sync()` still exists - `video_begin_frame()` calls it as a safety
net, so nothing can draw into a buffer the panel has not released.

The work is now ~10.7 ms in a 16.74 ms schedule with ~6 ms of slack, which is
where the panel wait goes. `sync` stops being time taken from the frame; it is
reported as part of `wait`, and the `slow frame` line now measures work over one
panel period (16.7 ms) rather than wall time, because a frame can no longer be
made slow by waiting.

### The simulation, and its honest limit

The decision itself - what the loop is willing to wake for - is in
`include/pace.h` as `pace_wake()`, because it is the third piece of loop
arithmetic in this file and the first two both had bugs. Writing the tests found
one in the new code with no hardware involved: a deadline already in the past
returned a wake time *behind* the clock, which the caller turns into a 0.2 ms
sleep - a spin, polluting the very idle time the change exists to use. It now
returns `now` at worst, and `pacetest` holds it to that along with "a pending
swap cannot delay a frame" and "a zero-length frame cannot divide by zero".

Modelling the loop against a 60 Hz panel grid (`/tmp` scratch, not committed):
late starts go from 11 % to 0 % with identical throughput when the wait fits in
the slack. That is the mechanism, and the console numbers say the wait does not
currently fit - 10.7 + 5.7 = 16.4 ms of a 16.74 ms schedule, with the jitter that
makes it 17.3. What the simulation cannot prove is the size of the slack on
ARM11, because it cannot model cache or `emu_run_frame()`'s real distribution.
The next log answers it: `wait` should rise to ~6 ms, `frame` should fall to
~10.7, `pace_late` should collapse toward 0, and `fps` should be 59.7.

---

## 15. The next question is not a front-end question

§14 moved the panel wait out of the frame, and the third log says it worked -
`sync` 5.7 → 0.0 ms, `sync_skip` 0, `sync_late` 0 - while the frame rate got
*worse* (57.8 → 56.8) because the time freed did not stay free: it reappeared in
`core`, 13.2 → 16.6 ms on the same ROM, and `wait` is 0.0 ms, i.e. the loop
never sleeps. The emulator now runs while the previous frame's GPU transfer and
cache flush are still in flight and pays for the contention.

With `core` at 16.6 ms of a 16.74 ms budget there is nothing left to schedule
around. Before reworking the presentation a fourth time, the question worth
answering is whether *any* frontend on this hardware runs Gambatte faster - so
`docs/RETROARCH-3DS.md` reads RetroArch's own 3DS drivers and `libretro/`
provides this project's emulation behind the libretro API to test it:

```
make -C libretro test      # passes the same dmg-acid2 reference as tools/host
make -C libretro ctr       # archive for RetroArch's 3DS build
```

If Gambatte under RetroArch 3DS is smooth where GBabe is not, the front-end is
the answer and its video path is worth copying (legally: ideas, not code - it is
GPL-3.0). If it stutters the same way, the core is the wall and the work moves
to making Gambatte faster.

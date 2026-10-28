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
emulator core (CPU + PPU + APU), `blit` is the scaled copy, `sync` is waiting
for the panel to take the finished frame, `wait` is pacing, and `other` is the
rest of the loop - which is where a blocking service call would show up. A frame
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

The fix is one wait: after asking for the swap, wait for the vblank that takes
it, and re-check `gspIsPresentPending()` in case the request missed that vblank
by a hair. The wait costs nothing, because it happens in the idle part of the
frame anyway. It is kept out of the `blit` figure in the log (`sync`), or it
would look as if the copy had got 12 ms slower.

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

The host harness now counts torn frames - a frame that fetches the top
framebuffer while a swap is still pending - and fails if there are any:

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
| `sync` | 3 - 8 ms, occasionally 16.6 | the vblank wait, as designed |
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

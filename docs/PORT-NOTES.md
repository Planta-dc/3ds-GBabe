# Porting a Game Boy core to the Nintendo 3DS — notes

The original question was "can Walnut-CGB be ported to the Old 3DS?". The
answer was yes, and that is what this port started as (§1 onwards). It then
turned out to be too slow to play, so the core was replaced by Gambatte 0.5.0
while the rest of the port stayed. Both cores are still built and can be put
side by side on one console.

This document records why, what was measured, and what is still missing.

---

## 0. The default core is Gambatte 0.5.0

`make` builds `CORE=gambatte` (the vendored core in `third_party/gambatte/`,
GPL-2.0, backend `source/emu_gambatte.cpp`). `make CORE=walnut` builds the
original Walnut-CGB version (`source/emu.c`) as a separate installable —
`gbabe-walnut.cia`, its own title id, so both can sit on the same console and
be compared with the same ROM. Everything below `include/emu.h` is shared:
video scaling, audio FIFO and CSND pump, input, `.sav` files, UI, debug log.

### What it was worth, measured

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
measurement that matters is `core=` in `sdmc:/3ds/gbabe/debug.log` on the
console, once with `gbabe.cia` and once with `gbabe-walnut.cia`.

### Correctness, not only speed

The DMG rendering test ROM `dmg-acid2` is rendered by both cores through the
same front-end, and compared against the reference image its author ships:

| Build | Pixels differing from the reference (of 23 040) |
|---|---|
| Gambatte | **0** — pixel-identical |
| Walnut-CGB | 530, all in one band around the sprite/OBP tests |

So the change fixes a rendering bug as well as a speed problem. The CGB path
is unchanged: the same CGB ROM (blargg `cpu_instrs`) renders identically in
both builds, 0 of 96 000 pixels different.

### What the backend has to do

| Concern | How |
|---|---|
| Video | `runFor()` draws a whole 160x144 RGB565 frame into a static buffer (`-DVIDEO_RGB565` on both sides, or the pointer types disagree); each line then goes to `video_draw_line()`, so scaling stays in `video.c`. Frame skip passes a NULL buffer and skips the line loop. |
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

## 1. Why Walnut-CGB suited the 3DS (it was the first core)

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
* **Line-exact scaling.** `video_draw_line()` maps source line `n` to destination
  rows `[n*H/144, (n+1)*H/144)`, so scaling stays coherent per scanline and
  there is no tearing inside a line.
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
* **The letterbox is cleared every frame.** `video_draw_line()` writes only the
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
* **CSND block pump.** `audio_frame()` pushes 548 stereo samples per emulated
  frame into a 6-frame FIFO; whenever the channel is idle and the FIFO holds at
  least half a block, a 1024-sample one-shot block is queued. Underruns produce
  silence rather than stalls.
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
   core speed means `WALNUT_GB_16_BIT_OPS`/`SAFE_DUALFETCH_*` trades or a
   dynarec, less blit means a different scale default.
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
emulator core (CPU + PPU + APU), `blit` is scaling plus cache flush plus
present, `wait` is pacing, and `other` is the rest of the loop - which is
where a blocking service call would show up. A frame that is slow with a large
`other` is the port's fault; a large `core` is the ARM11 and needs a dynarec,
not tuning.

## 7. If you want to do the same port from scratch

The minimal recipe, in order:

1. Copy `walnut_cgb.h` in; define the five callbacks; call `gb_init()`.
2. Call `gb_get_save_size_s()` and allocate the cart RAM buffer.
3. Implement `lcd_draw_line` writing RGB565 into the rotated framebuffer.
4. Call `gb_init_lcd()`; then `while (true) gb_run_frame_dualfetch()` inside
   your main loop.
5. Map `hidKeysHeld()` into `gb.direct.joypad` (active-low).
6. Add `audio_read`/`audio_write` to `minigb_apu` and pump samples to CSND.
7. Pace with `svcGetSystemTick()` at 59.7275 Hz instead of vblank.
8. Draw the UI yourself (§5). A text console is fine for a splash screen and
   too slow for anything a user navigates on an Old 3DS.
9. Log to a file (§6). You will not have the console, and neither will whoever
   reports the first bug.

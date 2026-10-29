# What other Game Boy emulators do, and what it says about this one

Asked, reasonably: before spending another week on frame-loop arithmetic, is
there a Game Boy Color emulator that already runs at full speed on this
hardware, and what does it do differently?

Yes, there is one on the *same console*. This is what it and three other
reference implementations do, measured against what GBabe does. Sources were
read directly; the licences are noted because GameYob is MIT (readable, but
nothing is copied here anyway) and Gearboy is GPL-3.0 (commentary and file
references only, as with RetroArch).

## The reference points

| emulator | hardware it runs full speed on | pixels the CPU writes per frame | audio |
|---|---|---|---|
| **GameYob** (Stewmath/GameYob, MIT) | **Nintendo DS: 67 MHz ARM9.** Also ported to the 3DS | **23,040** (160x144), 1:1, one scanline at a time, straight into the rotated framebuffer | **one mono sample every 84 emulated cycles**, mixed inline, ~50 kHz, straight into the CSND buffer |
| **Gearboy** (drhelius, GPL-3.0) | ESP32 @ 240 MHz, and many others | 160x144 into a u16 framebuffer, per scanline | output-rate, one 227-line file |
| **Peanut-GB** (deltabeard, MIT) | **RP2040 @ 133 MHz Cortex-M0+** - no cache, no FPU | 160 px/line, handed to a callback | external APU, separate thread |
| **Gambatte-libretro** | RetroArch on 3DS | 23,040 into a 160x144 buffer; **the GPU scales** | 2,097,152 Hz, resampled in the frontend (blipper) |
| **GBabe 0.3.2** | - | **64,080** (267x240), CPU-scaled *and* transposed | 2,097,152 Hz, two-stage decimator, measured at **3.0 ms** |

## Finding 1: the console was never the problem

GameYob ran Game Boy and Game Boy Color at full speed on a **67 MHz ARM9 with
4 MB of RAM** - a quarter of the clock, a tenth of the RAM, and a CPU without
ARM11's branch predictor. It has also been ported to the 3DS and runs at full
speed there. Whatever GBabe's frame budget problem was, "268 MHz ARM11 is not
enough for a Game Boy" was never a defensible answer, and the 0.3.1 log agreed:
13.35 ms of work where GameYob would be doing well under 8.

## Finding 2: nobody else scales on the CPU

`platform/3ds/gbgfx.cpp:343-356` in GameYob: after compositing one scanline it
writes **160 pixels**, at `offsetX = (400 - 160) / 2`, straight into
`gfxGetInactiveFramebuffer()`. The picture is 160x144 in the middle of the
panel and the borders are drawn once, separately. The rotation is handled by
the addressing inside `drawPixel()`, so there is no intermediate frame and no
transpose either.

Gearboy (`Video.h:66-71`) and Peanut-GB (`peanut_gb.h:1784`) are the same
shape: a `ScanLine()`/`lcd_draw_line()` per line, native resolution, and the
frontend decides what to do with the 160x144 it is given. RetroArch's 3DS
driver takes that 160x144 and lets the **GPU** stretch it.

GBabe is the only one of the five that scales in software. At `SCALE_FIT` that
is 64,080 writes and a transposed gather, measured at 1.0 ms/frame - 2.8x the
pixels of everyone else, on a CPU with a 16 KiB L1 and no L2. Banding (§16)
made it affordable; not doing it at all would make it free.

## Finding 3: everybody else mixes audio at the output rate

This is the expensive one. GameYob's sound engine
(`platform/3ds/soundengine.cpp:33-37, 69-75, 330-375`) takes **one sample every
84 emulated cycles**, mixes the four channels into a single scalar on the spot,
and writes it into the CSND buffer. About 836 samples per frame, mono, and the
cost is a handful of adds per scanline - effectively zero.

Gambatte mixes at **2,097,152 Hz** in stereo, for accuracy, and hands the
frontend 35,112 stereo samples per frame. `gambatte.h:53-79` has no sample-rate
control at all, which is why upstream Gambatte-libretro also decimates in the
frontend. GBabe's decimator is the cheapest form of that (two box averages, no
history buffer) and it is still **3.0 ms, 18 % of the frame** - because it reads
140 KiB of intermediate samples, writes 17.5 KiB, then reads that back.

So the 3.0 ms is the price of Gambatte's accuracy, not a bug. It can be made
cheaper (one fused pass over the 2 MHz buffer keeps it in L1: ~1.5 ms, and it
is a self-contained change with a host test), but it cannot be removed without
changing the core's APU.

## Finding 4: the pacing shape this project just adopted is the standard one

GameYob runs the emulator until VBLANK, *then* draws and swaps
(`platform/common/gbmanager.cpp:66-118`), and blocks on `gspWaitForVBlank()`
after presenting (`platform/3ds/inputhelper.cpp:140`). RetroArch's 3DS driver
waits on the next vblank *after* the transfer too. That is exactly the shape
0.3.2 moved GBabe to: present at the end of the frame, wait with the frame's
slack, never block the emulator on the panel.

## So: switch cores?

No, and this is the important conclusion.

Gambatte's 8.4 ms is the price of it being right. GameYob is faster because it
does less - it is a scanline renderer with a simplified PPU, and its own
documentation says it can be inaccurate. Swapping to it would buy back maybe
4 ms that, after 0.3.2, the console does not need: 13.35 ms of work became
11.8-12.4 ms of work in a 16.74 ms budget, and the DMG case is 9.5 ms.

mGBA and SameBoy were considered and are the wrong direction: both are
accuracy-first, both are slower than Gambatte on ARM without a cache, and both
would trade a solved problem for an unsolved one.

## What is actually worth taking, in order

1. **Nothing, until the 0.3.2 log lands.** If `frame` is ~16.7 and `fps` ~59.7
   with `underruns 0`, the remaining 3 ms decimator is not costing anything and
   optimising it would be the same speculative work that wasted the last six
   rounds.
2. **If a heavy CGB scene is still short: fuse the decimator** into one pass
   over the 2 MHz buffer (~1.5 ms back, host-testable, no picture risk).
3. **If scaling ever needs to be free: render 1:1 and let the GPU scale**, the
   way the other four do. That is a real piece of work (a textured quad or a
   display transfer, and a re-think of the scale modes) and it should wait for
   evidence that the 1.0 ms matters, which at 267x240 it currently does not.
4. **Do not copy a fast core's PPU.** The accuracy is the reason this port
   exists, and it is cheaper to keep it than to replace it.

*Sources read: `Stewmath/GameYob` (MIT), `drhelius/Gearboy` (GPL-3.0),
`deltabeard/Peanut-GB` (MIT), and this repository's vendored Gambatte 0.5.0.
File and line references are for orientation; no code was taken from any of
them.*

## Addendum: three more emulators, read afterwards

Four more references were pointed at this survey; one of them contradicts a
claim made above, so it is corrected here rather than left to stand.

| emulator | licence | scale | audio | note |
|---|---|---|---|---|
| **gnuboy-3ds** (RetroGamer02) | GPL-2.0 | **none - `delegate_scaling = 1`**, the GPU stretches 160x144 (`sys/sdl2/sdl2.c:104,130`, honoured in `lcd.c:591-593`) | **mixed at the output rate, inline, per line** (`sound.c:164-175`: `for (; cpu.snd >= RATE; cpu.snd -= RATE)`, called from `emu.c:153-161`) | the classic speed-first GBC emulator; its frame loop is emulate-whole-frame -> swap -> mix -> submit -> sleep until the audio says otherwise |
| **PlutoBoy 3DS** (RossMeikleham) | GPL-3.0 | **CPU scale to 266x240** (`graphics_3DS.c:13-16`), 24-bit BGR8, one `memcpy(3)` per pixel, into the rotated panel | **blargg `Gb_Apu` + `Blip_Buffer`**, band-limited synthesis *at the output rate* (`src/core/audio/Blip_Buffer.h:32-38`) | renders 266x240 - almost exactly GBabe's 267x240 - and still works |
| **GBEMU43DS** (TS-DEV-DEBUG-V2) | GPL-3.0 | **none** - 160x144 into a 256x256 RGBA8 texture, `GPU_NEAREST`, `GX_DisplayTransfer` + citro3d quad (`platform/3ds/source/main.cpp:1266-1300`) | **11,025 Hz mono, mixed inline** (`src/gb_apu.h:6`, `src/gb_apu.cpp:387`) | full 3DS-native feature set: camera, printer, savestates |

### Correction: CPU scaling is not unique to GBabe

The first half of this document said "nobody else scales on the CPU". PlutoBoy
does, on the 3DS, at 266x240 - within one pixel per row of GBabe's 267x240 in
the default mode - and writes **three bytes per pixel into a 24-bit
framebuffer**, which is strictly more memory traffic per pixel than GBabe's
sixteen bits. That does not make the transpose free (PlutoBoy's `graphics_3DS.c`
even carries a commented-out nearest-neighbour variant marked "Slow :("), but it
does mean the 1.0 ms measured here is in line with what another 3DS port
considers acceptable, and it further weakens "the scaling is why this port was
slow". The blocking vblank in section 17 was.

### The audio finding gets stronger, not weaker

Five of the six emulators read for this document generate audio **at the output
rate, inline, only when a sample is due**: GameYob (84 cycles), GBEMU43DS
(11,025 Hz), gnuboy (accumulator, per line), PlutoBoy (Blargg band-limited),
Peanut-GB (external APU). Only Gambatte mixes at 2 MHz and leaves the
decimation to its frontend - which is why both Gambatte frontends in this
document (RetroArch's and this one) carry a resampler.

That is not an argument for replacing Gambatte (its API has no sample-rate
control, and the accuracy is the reason for the port), but it does say where the
last 3.0 ms lives: in a fixed cost of Gambatte 0.5.0's interface. The cheapest
realistic recovery is fusing GBabe's two decimation stages into one pass over
the 2 MHz buffer (~1.5 ms), not a new core.

### On "just port one of them"

All three of the new ones are GPL (gnuboy GPL-2.0, PlutoBoy and GBEMU43DS
GPL-3.0), as is RetroArch, so nothing of theirs can be copied into this
MIT-frontend/GPL-core tree's frontend; and all three are speed-first cores of
the same kind as GameYob, which is the trade the earlier section already
rejected. What they are good for is exactly what they were used for here:
confirming what "full speed on a 3DS" looks like from three independent
implementations.

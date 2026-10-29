# The old RetroArch 3DS port, read closely

The 3DS build of RetroArch is the largest body of working 3DS emulator
front-end code that exists, and this project has spent three releases fighting
the same presentation problems it solved years ago. So: clone it at the last
version before the port grew a citro3d shader pipeline, an OSD and a threaded
GPU, read the drivers, try to build it, and put the emulation behind a libretro
core so the comparison can be made on hardware instead of argued about.

What was cloned: `libretro/RetroArch` at **v1.7.7** (`62da78c`, *"CTR / Update
Makefile.cores"*), 153 MB, into `/home/user/retroarch-1.7.7` — outside this
repository, and not committed, because 153 MB of somebody else's GPL-3.0
frontend has no business in an MIT project's history. Everything quoted below
is a handful of lines with a file and a line number so it can be checked
against a fresh clone.

## 1. Can it be built here? No — and the reason is worth recording

```
$ make -f Makefile.ctr
Makefile.ctr:91: *** recipe commences before first target.  Stop.
```

That is GNU Make 4.3 rejecting the file before it ever looks for a compiler.
`Makefile.ctr:89-95` has

```
ifeq ($(strip $(DEVKITPRO)),)
	$(error "Please set DEVKITPRO in your environment. ...
endif
```

and the leading tab makes Make read `$(error ...)` as a recipe line, which is
illegal before the first target. It was legal-and-ignored in the Make 3.x that
the port was written against. The same tab appears twice (`DEVKITPRO`,
`CTRULIB`), and removing it makes the build system itself work:

```
$ sed 's/^\t$(error/$(error/' Makefile.ctr > /tmp/Makefile.ctr.fixed
$ make -f /tmp/Makefile.ctr.fixed -n DEVKITPRO=/tmp/fake | head -4
/bin/arm-none-eabi-gcc -c -o ctr/3dsx_custom_crt0.o ctr/3dsx_custom_crt0.s ...
/bin/picasso gfx/drivers/ctr_shaders/ctr_sprite.vsh gfx/drivers/ctr_shaders/ctr_sprite.gsh ...
/bin/bin2s gfx/drivers/ctr_shaders/ctr_sprite.shbin | /bin/arm-none-eabi-as -o ...
/bin/arm-none-eabi-gcc -c -o ctr/ctr_system.o ctr/ctr_system.c -mword-relocations ...
```

Past that point it needs, in order:

1. **devkitARM + libctru.** Not on this machine, and not installable: the
   sandbox reaches `github.com`, `pypi.org` and `registry.npmjs.org` only, and
   devkitPro ships from its own repositories. `docker` is not available either,
   so there is no way to use the image this repository's CI builds with.
2. **A forked libctru.** The link line is
   `-lretro_ctr -lctru` (`Makefile.ctr:151`), and `libretro_ctr` is a patched
   libctru with the GSP/GPU internals `gpu_old.c` expects. It is not in the
   tree; the CI script of the era builds it separately.
3. **`picasso` and `bin2s`**, for the shader blobs (`Makefile.ctr:214-222`).
4. **The cores.** `libretro_ctr.a` (`Makefile.ctr:177`) is dropped in from
   outside; the buildbot supplies it.

So: **not buildable in this environment, and the first blocker is a two-character
Make 4 incompatibility, not the toolchain.** That is a genuinely useful finding
for anybody reviving the port, and it is why nothing below claims to be a
built binary.

## 2. What the port is made of

| path | lines | what it is |
|---|---|---|
| `gfx/drivers/ctr_gfx.c` | 1294 | the video driver: GPU-drawn textured quad, display transfer to the framebuffer |
| `audio/drivers/ctr_csnd_audio.c` | 305 | CSND (the sound service GBabe uses) |
| `audio/drivers/ctr_dsp_audio.c` | 212 | ndsp (the DSP service) |
| `frontend/drivers/platform_ctr.c` | 626 | threads, files, clocks, CPU time limit |
| `ctr/ctr_system.c`, `ctr/gpu_old.c`, `ctr/ctr_linear.cpp` | — | libctru fork against the GSP |
| `Makefile.ctr` | 240 | the 3DS build |

The 3DS build is **griffin**: one translation unit containing the whole
frontend (`Makefile.ctr:51-56`), `-O3 -ffast-math -mword-relocations`
(`Makefile.ctr:106-129`), `HAVE_THREADS` commented out (`Makefile.ctr:78`) —
so it is single-threaded, like GBabe.

**It shipped a Gambatte build.** `pkg/ctr/Makefile.cores:90`:

```
else ifeq ($(LIBRETRO), gambatte)
	APP_TITLE            = Gambatte Libretro
	APP_PRODUCT_CODE     = RARCH-GAMBATTE
	APP_UNIQUE_ID        = 0xBAC01
```

That is the control experiment this document ends with: an official
*Gambatte Libretro* `.cia` for the 3DS existed, running the same core this
project vendors, under this frontend.

## 3. The video path

`ctr_gfx.c` never touches the top-screen framebuffer with the CPU:

1. the core hands over a frame; the driver copies it row by row into a
   **linear texture** (`ctr_gfx.c:729-737`), flushing only that texture;
2. `ctrGuCopyImage()` swizzles it on the GPU (`ctr_gfx.c:740`);
3. it binds the texture (`ctr_gfx.c:753`) and draws a quad scaled by a
   vertex-shader uniform (`ctr_gfx.c:750`), so **rotation, scaling and
   filtering all happen on the GPU**;
4. `GPU_FinishDrawing()` (`ctr_gfx.c:877`) → `GPU_Finalize()` →
   `ctrGuFlushAndRun()` → `ctrGuDisplayTransfer()` (`ctr_gfx.c:881`) copies
   the rendered scene into the framebuffer.

Against GBabe's measured numbers that is solving a problem this project does
not have: the whole-frame scaled blit here is **0.1 ms** on the console. A GPU
path would buy cache pressure and filtering for free, and nothing in time — at
the cost of a driver rewrite on hardware that cannot be tested in this
environment. **Not adopting it**, and the log is the reason.

## 4. The audio paths

Two drivers, both of which resample to the hardware's rate rather than making
the hardware cope:

- **CSND** (`ctr_csnd_audio.c:40-47`): `CTR_CSND_AUDIO_RATE 32730`, 2048-sample
  chunks, and it sets CSND's ticks-per-sample register (2048) so the pitch is
  exact.
- **ndsp** (`ctr_dsp_audio.c:48-76`): `ndspInit()`, one channel,
  `NDSP_FORMAT_STEREO_PCM16`, `NDSP_INTERP_NONE`, rate `32728.0f`, one looping
  wave buffer of 2048 stereo samples. The author marks
  `ndspSetClippingMode(ctr->channel, NDSP_CLIP_SOFT)` with `/* ?? */`, which is
  the most honest line in the file.

GBabe opens CSND at **32768 Hz** and decimates to it with two 8:1 box stages.
The hardware's native rate is 32730, so 32768 is 0.12 % off, which CSND's
interpolation absorbs; RetroArch's approach resamples to 32730 in the core
instead. Neither is obviously right — it is a measurable question (does the
pitch drift, and does the resampler cost more than the interpolation?) and it
is not the reason for this project's frame-rate trouble. **Not adopting it
now**, recorded as a possible follow-up.

## 5. Pacing: they hit the same wall, from the other side

This is the part worth reading twice. `ctr_gfx.c:587-614`:

> If we are running at below the display refresh rate, then frame updates will
> happen entirely out of sync with VBlank0 events. […] we'll wait for a VBlank0
> here, but it will already have happened partway through the previous frame
> […] resulting in 'overlap' and screen tearing.
>
> This is a consequence of using the GPU directly. Other 3DS homebrew typically
> uses the ctrulib function `gfxSwapBuffers()`, which ensures an immediate
> buffer swap every time, and no tearing. We can't do this […]
>
> I don't know how to fix this 'properly' (probably needs some low level
> rewriting, maybe switching to an implementation based on citro3d), but I can
> at least implement a hack/workaround […] If content frame rate is more than
> 10 % lower than the 3DS display refresh rate, don't wait on the *current*
> VBlank0 event […] but instead wait on the *next* VBlank0 event […] This fixes
> screen tearing, but it has a significant impact on performance.

and the code (`ctr_gfx.c:619-623`) does exactly that: wait on the next vblank
when the core's fps is below 90 % of the panel's.

That is the same structural problem this project has been unwinding in
`docs/PORT-NOTES.md` §10–§14, arrived at from the opposite direction: they
sacrifice performance to protect the picture, GBabe sacrificed the picture
(sometimes) to protect the frame rate. §14's answer — poll
`gspIsPresentPending()` in the idle time instead of blocking on the event —
is the one neither version does, and it is the reason GBabe no longer blocks at
all.

Their own conclusions are the useful part: *"I don't know how to fix this
'properly'"*, and the workaround costs performance. Copying their driver would
inheriting the problem.

## 6. Two things worth stealing (no code, just ideas)

Both are one-line experiments for the *next* build, not part of this change:

1. **CPU time budget.** `frontend/drivers/platform_ctr.c:436` calls
   `osSetSpeedupEnable(true)` (a no-op on Old 3DS), and the CPU-time-limit call
   appears only in the svchax path — `ctr/ctr_svchax.c:216`,
   `APT_SetAppCpuTimeLimit(5)`: 5 % of the core to the system, the rest to the
   app. GBabe calls `APT_SetAppCpuTimeLimit(80)` unconditionally. On a patched
   console (Luma + svchax-style patches) 5 is accepted; on a stock one 80 is the
   documented ceiling. Whether the lower value is available here, and what it
   buys, is a measurable question with a real payoff — `core` is 16.6 ms of a
   16.74 ms budget in the last log.
2. **Whole-program optimisation of the frontend.** griffin exists because the
   ARM11 has no L2 and cross-TU calls are expensive. GBabe already builds with
   `-O3 -flto`, so this is likely already captured — but it is worth one A/B on
   `core`, which is the number that now matters.

## 7. The licensing wall

RetroArch is **GPL-3.0** (`COPYING`). None of its source may be copied into this
project: GBabe's own code is MIT, and although the linked Gambatte makes the
binary GPL-2.0 (`third_party/gambatte/README.md`), GPL-2.0-only and GPL-3.0
code still cannot be combined. Everything in this document is commentary with
file:line references — reading, not copying.

The one file vendored from the clone is `libretro/libretro.h`, which is MIT in
its own header (`Copyright (C) 2010-2018 The RetroArch team … Permission is
hereby granted, free of charge …`) and is vendored by every libretro core in
existence.

## 8. The base: GBabe as a libretro core

`libretro/` is the part of this work that is code rather than reading:

| file | what it is |
|---|---|
| `gbabe_core.cpp` | the libretro core: Gambatte, the same 8:1+8:1 decimation, one video frame and one audio batch per `retro_run()` |
| `test_frontend.c` | a four-function frontend that drives it and checks what a frontend would notice |
| `Makefile` | host `.so` + test, and a `ctr` target that produces the arm archive |
| `libretro.h` | the MIT API header |

It deliberately does **not** reuse `source/emu_gambatte.cpp` or `video.c` /
`audio.c`: those are the 3DS front-end, and they are the thing under test. The
glue is its own 120 lines, and the two paths are held to the same reference
frame.

```
$ make -C libretro test
core: GBabe 0.3.1 (gb|gbc)
av: 160x144, 59.7275 fps, 32768 Hz
ran 300 frames: 300 video callbacks, ...
audio: 548.7 samples per frame (548.6 expected)
audio: 1.00 batches per frame, 548.7 samples each
state round trip: 4e82b672 vs 4e82b672
obj/core-acid2.ppm: 0 of 23040 pixels differ (4 shades, palette-independent)
core: ok
```

The render check is the same one the 3DS path passes (`make -C tools/host
acid2`), against the same `test/dmg-acid2-reference.ppm`: **the core draws the
same picture as the application.** It also checks the two numbers a frontend
paces itself from — 59.7275 fps, not 60, and 32768 Hz — a savestate round trip,
and that a batteryless cart reports no save RAM.

`make` on its own produces `libgbabe_libretro.so` — 25 `retro_*` entry points,
loadable by a desktop RetroArch — and that is where a small trap lives: the
Gambatte archive the test links is built without `-fPIC`, and a static archive
of non-position-independent objects cannot go into a shared object

```
/usr/bin/ld: libgambatte-host.a(envelope_unit.o): relocation R_X86_64_PC32
              against symbol ... can not be used when making a shared object
```

so the Makefile builds a second `libgambatte-host-pic.a` with `-fPIC` for the
`.so` and keeps the plain one for the test binary. Two archives on disk, one
line of explanation, and neither is committed.

Wired into the host CI job and `docker/ci-build.sh`, which also runs
`make -C libretro ctr` so the arm archive is compiled by the real toolchain on
every build.

## 9. The experiment this exists for

The question the last three logs cannot answer: **is GBabe's front-end the
problem, or is 60 fps of Gambatte simply at the edge on a 268 MHz ARM11?**

Run the same ROM, on the same console, in a frontend that is not this one:

1. **Get an old RetroArch 3DS** — the era of `v1.7.7`, which shipped
   `Gambatte Libretro` (`pkg/ctr/Makefile.cores:90`). Either a build from the
   period, or build it: `make -f Makefile.ctr LIBRETRO=gambatte` with the tab
   fix from §1 and devkitARM installed.
2. **Run the ROM that shows the trouble** — Croc 2 and Avenging Spirit are the
   two in the logs. Watch for a full second; GBabe's hitch is once a second.
3. **Run the same ROM in GBabe**, with the debug log on.
4. Compare what you *see*, then compare `fps` in GBabe's `perf` line with what
   RetroArch's OSD reports.

- If RetroArch + Gambatte is smooth where GBabe is not, the front-end is the
  difference and its video driver is where to look next (§3, §5).
- If RetroArch + Gambatte stutters the same way, the core is at its limit on
  this hardware, no presentation change will fix it, and the work moves to
  making Gambatte itself faster (`core` is 16.6 ms of a 16.74 ms budget).

Alternatively, run **GBabe's own core** under RetroArch 3DS, which isolates the
front-end completely:

```
make -C libretro ctr                     # libgbabe_libretro_ctr.a
cp libretro/libgbabe_libretro_ctr.a  <RetroArch>/libretro_ctr.a
cd <RetroArch> && make -f Makefile.ctr LIBRETRO=gbabe     # with a
                                                          # Makefile.cores entry
```

`libretro_ctr.a` is exactly the archive name `Makefile.ctr:177` links
(`-lretro_ctr`), and `Makefile.cores` only supplies the title/icon metadata, so
a new entry is five lines. Unverified here for the reason in §1 — but that is
what the control experiment is for.

## 10. Where this leaves the port

Nothing here changes GBabe's runtime. What it changes is what to do next,
because the three logs so far are consistent with one uncomfortable reading:
**the frame loop is now as good as it can be and the core is the wall.** The
scheduling work of §10-§14 removed ~6 ms of panel wait from the frame; the
time did not become free, it reappeared in `core` (`sync 5.7 → 0.0`, `core
13.2 → 16.6`, `pace_late` still ~52/60).

So before another driver rewrite: measure the same emulation under a frontend
that is not this one (§9), then either chase §6's two one-liners or attack
Gambatte's per-frame cost. The first is a morning; the second is a project.

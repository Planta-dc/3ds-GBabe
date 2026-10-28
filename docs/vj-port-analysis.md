# Virtual Jaguar on Android — can we convert it to C++ to make porting (and low-end devices) easier?

*Analysis written 2026-10-28. Both upstream repos were cloned and compared
side by side, the libretro core was built and benchmarked locally, and the
C++-conversion idea was tested by actually doing it. Everything below is
either measured here or quoted from the upstream repos with a file/issue
reference — nothing is guessed without saying so.*

---

## 1. Short answer

**Yes it is technically possible — and it is the wrong move.**

| Question | Answer |
|---|---|
| Can the code be converted to C++? | Yes. The core is ~95% valid C++ already: a syntax-only check of 9 core files produced **3 errors**. |
| Would it make porting to other systems easier? | **No, harder.** Upstream did the opposite on purpose: the libretro core was converted **C++ → C** in 2015, and the project now enforces **C89/GNU89 strict** so MSVC and every console/phone toolchain can build it. |
| Would it run faster on a low-end Android? | **No.** I compiled the hot files both ways: the machine code is the same to within 0.03%–1.6% (table in §4). Language is not what is slow. |
| So what is slow? | The two RISC cores (GPU + DSP) being interpreted: **24–74% of every frame** (§5). That is an algorithm problem, not a language problem. |
| Does an Android build already exist? | **Yes.** The libretro repo ships Android `arm64-v8a`, `armeabi-v7a`, `x86_64` and `x86` `.so` builds from CI, with NEON/SSE2 picked per ABI. RetroArch on Android already runs this core. |

**The recommendation:** keep the core in C, write the Android **front-end** in
whatever you like (C++ is fine — `src/core/jaguar.h` is already
`extern "C"`-guarded), and spend the effort on the RISC interpreter, which is
where the frame time actually goes.

---

## 2. What the two repos actually are

| | [`lubomyr/virtualjaguar`](https://github.com/lubomyr/virtualjaguar) | [`libretro/virtualjaguar-libretro`](https://github.com/libretro/virtualjaguar-libretro) |
|---|---|---|
| Version | **v2.1.3** (`docs/WHATSNEW`) | **v3.10.0** (`src/core/version_fallback.h`) |
| Last commit | **2016-11-06** (`glshim library renamed to gl4es`) | **2026-10-23** (10 years newer) |
| Languages | C++: 41 `.cpp`, 7 `.c`, 46 `.h` | C: 72 `.c`, 85 `.h` — **C89/GNU89 strict** |
| Code size, excluding embedded BIOS arrays | **~39,200 lines** | **~138,100 lines** core + 8,542-line `libretro.c` front-end + **92,300 lines of tests** |
| External dependencies | Qt5 (widgets + OpenGL), SDL 1.2, SDL_ttf, guichan, pkg-config, libcdio (optional), gl4es on Android | **None.** A C compiler. (`libretro-common` and `libchdr` are vendored in-tree.) |
| OS coupling inside the emulation core | **Yes** — `src/dsp.cpp`, `src/dac.cpp`, `src/jaguar.cpp`, `src/joystick.cpp` call SDL directly | **None** — the only 2 `windows.h` includes in `src/` are `#if defined(_WIN32)`-guarded |
| SIMD | **None anywhere** (no NEON, no SSE2 in any file) | NEON + SSE2 for blitter, object processor, shadow framebuffer, line scan |
| Android story | SDL 1.2 through the abandoned `pelya/commandergenius` glue, NDK r12b, API 23, gl4es as a fake `libGL.a`; needs a downloaded `data.zip`; last built in 2016 | `jni/Android.mk` + release CI: `arm64-v8a`, `armeabi-v7a` (both NEON), `x86_64`, `x86` (SSE2) |
| Other platforms | Linux / macOS / Windows + that Android hack | Linux (incl. per-SoC Raspberry Pi 1–5 builds), macOS, Windows, Android, iOS, tvOS, WASM/Emscripten, PS Vita, Switch |
| Test / CI | none | unit tests + benchmarks + C89/MSVC lint gates + a release matrix in GitHub Actions |
| Features the other one lacks | — | Jaguar CD HLE boot, Memory Track, GameDrive, netlink netplay, Team Tap, light gun, paddle, cheats, GDB stub, texture dump/replace, crash detect, blit memoization, per-title database, save states, frameskip, clock scaling |

They are the **same emulator**: the libretro core is a fork of the same
Virtual Jaguar source (James Hammons / Underground Software), and lubomyr's
repo is a **2016 snapshot of it with an Android/SDL wrapper bolted on**. The
libretro fork is that same code plus ten years of work.

### The C++ → C conversion is deliberate, and it is in the git history

```
682b6fd  Convert everything over to C                      (2015-08-28)
2ee290f  Convert most of the remaining files over
0b9c624  Convert mmu.cpp to C
b25fef2  Convert eeprom.cpp to C
c2f44a2  Convert joystick.cpp to C
4a2c5fb  Convert vjag_memory.cpp to C
a719196  Convert cdrom.cpp to C
...      (one commit per file, ~20 in total)
```

And it is still policy today — `app/README.md` says *"The core stays C89 and is
linked statically"*, and `AGENTS.md` / `CONTRIBUTING.md` say:

> **C89 / GNU89, strict.** No mid-block declarations… no C99 (`for(int i…)`,
> compound literals, designated initializers, VLAs).
> *The libretro buildbot uses MSVC on Windows — MSVC C89 mode is strict.*

That rule exists **for portability**: it is what lets one C source tree build
under MSVC, clang, GCC, the Android NDK, Emscripten, the Vita and Switch
toolchains. Converting to C++ would delete the property that makes the code
portable in the first place.

---

## 3. Portability is an architecture property, not a language property

Measured, not argued:

**libretro core — cross-compiles clean, no source changes:**

| Target | Result |
|---|---|
| `aarch64-linux-gnu` + `-DBLITTER_SIMD_NEON` | blitter, blitter_simd_neon, gpu, op, tom, dsp, jaguar, m68k — **all pass** |
| `arm-linux-gnueabihf` (32-bit ARM) + NEON | blitter, blitter_simd_neon, gpu, dsp — **all pass** |
| native `make` on a bare sandbox (only `gcc` + `make` installed) | **builds a working `.so` in ~3 minutes** |

**lubomyr's C++ core — cross-compiles to `aarch64` too, but:**

```
PASS aarch64 C++: src/blitter.cpp
PASS aarch64 C++: src/gpu.cpp
FAIL aarch64 C++: src/dsp.cpp     -> fatal error: 'SDL.h' file not found
FAIL aarch64 C++: src/jaguar.cpp  -> fatal error: 'SDL.h' file not found
```

Note what that proves: **C++ is not the problem and not the cure.** The C++
files compile for ARM just fine. What stops them is that the *emulation core
calls SDL* — `dsp.cpp` opens SDL audio, `jaguar.cpp` and `dac.cpp` call SDL,
`video.cpp` has 48 SDL calls and OpenGL. The UI, audio and input are welded to
the emulator. That is the portability bug, and rewriting it in C++ leaves it
exactly as welded.

The libretro tree has the opposite shape: all 72 `.c` files under `src/` are
pure emulation with no OS dependency at all, and everything platform-specific
lives in one file, `libretro.c`, behind a 24-function C ABI:

```
retro_set_environment, retro_set_video_refresh, retro_set_audio_sample(_batch),
retro_set_input_poll, retro_set_input_state, retro_init, retro_deinit,
retro_get_system_info, retro_get_system_av_info, retro_set_controller_port_device,
retro_reset, retro_run, retro_serialize(_size), retro_unserialize,
retro_cheat_reset, retro_cheat_set, retro_load_game(_special),
retro_unload_game, retro_get_region, retro_get_memory_data, retro_get_memory_size,
retro_api_version
```

That is why the same core drives RetroArch, the SDL3 desktop app in `app/`
(which uses *only* the `retro_*` API), WASM, Vita and Switch — and it is why
the whole Android port is a 76-line `jni/Android.mk` whose only real content is
a per-ABI NEON/SSE2 selection.

Also: `src/core/jaguar.h` — the core's own public API (`JaguarInit`,
`JaguarReset`, `JaguarExecuteNew`, `JaguarRead/WriteLong`, …) — is **already
wrapped in `extern "C"`**. A C++ Android front-end can call it directly today.

---

## 4. The experiment: I converted it to C++ and measured

I copied the tree, fixed what broke, and compared the generated machine code
(`-O3 -msse2 -DBLITTER_SIMD_SSE2 -fomit-frame-pointer -DNDEBUG`, the project's
own release flags).

**What broke, in order:**

1. **3 syntax errors in the 9 core files sampled** (extrapolates to a few
   dozen across 72 files):
   - `src/tom/gpu.c:646-647` — C allows a repeated tentative definition of a
     static; C++ calls it a redefinition.
   - `src/m68000/cpuemu.c:33` — `0xFFFFFFFF` into an `int` table: C allows the
     narrowing, C++ does not.
2. **2 silent-rule differences** in `src/cd/cdintf.c` — `char x[32] = "32 characters"`
   is legal in C and rejected by C++ (`-fpermissive` papers over it, but this is
   a real behavioural seam: the string is not NUL-terminated in either
   language).
3. **Linkage damage in headers** — `src/core/state.h` has an `extern "C"`
   block, `src/core/nvmbios.h` does not, so `NVMBiosStateSave/Load` got two
   different linkages. **Every header in a C-only project needs guards added.**
4. **Link failure in vendored dependencies** — the full C++ build does not
   link. Undefined `rfopen/rfclose/rfread/rfseek/rftell/rfgets`
   (`libretro-common` declares them as C in
   `streams/file_stream_transforms.h` but they compile mangled), plus
   `mz_free`, `tdefl_write_image_to_png_file_in_memory_ex`,
   `tinfl_decompress_mem_to_heap` from vendored `libchdr`/miniz, plus
   `GPUCPUINTCallback`, `JERRYI2SCallback`, `UARTRXCallback`, `UARTTXCallback`,
   `op_smalltbl_5_ff`.

**What it bought — the generated instructions, C vs C++:**

| hot file | C instrs | C++ instrs | delta |
|---|---:|---:|---:|
| `src/jerry/dsp.c` (≈33–67% of a frame) | 10,249 | 10,246 | −0.03% |
| `src/tom/blitter.c` | 15,982 | 15,769 | −1.3% |
| `src/tom/op.c` | 3,981 | 3,957 | −0.6% |
| `src/tom/tom.c` | 3,579 | 3,576 | −0.08% |
| `src/core/jaguar.c` | 2,787 | 2,533 | −9% |
| `src/core/event.c` | 636 | 636 | **identical** |
| `src/core/settings.c` | 152 | 152 | **identical** |
| `src/jerry/dac.c` | 778 | 778 | **identical** |

The differences are noise: the C++ build emits a few *fewer* instructions in
some files and a few more in others, because the two front-ends feed the same
GCC back-end the same integer code. (`src/tom/gpu.c` and `src/m68000/cpuemu.c`
don't compile as C++ at all without the edits above.)

**Net: a few hundred edits, several vendored-dependency linkage fights, and
0% performance.** The hot loops are plain scalar integer interpretation; C++
has nothing to add to them.

---

## 5. Where the frame time actually goes

From upstream's own profiling (`docs/op-perf-profile.md`,
`docs/perf-audit-2026-08.md`, `docs/settings-and-performance-guide.md`; Apple
Silicon, mid-game save states):

| Subsystem | Share of frame time |
|---|---|
| **DSP (Jerry RISC)** | **33–67%** — top cost on every title measured |
| **GPU (Tom RISC)** | title-dependent, 11–74% |
| Blitter, fast mode | 5–17% |
| Blitter, accurate mode | up to ~34% |
| Object Processor | **~1%** |
| 68000 | **0.7–2.6%** |

Two consequences, stated by upstream:

- **The 68000 and the Object Processor are not your problem.** A 10× 68K JIT
  would buy at most 2–3% wall clock — which is why they refused to take the
  GPLv2/GPLv3 licence hit of UAE-JIT or the ARM-only Cyclone68k.
- **The biggest single saving is not emulating the waiting.** The DSP sits in a
  3–5 instruction wait loop for **90–99.7%** of every frame while the emulator
  interprets 416k–590k opcodes. That is what `risc_idle_skip` removes.

### My own benchmark (this sandbox, for reference)

Host: 1 vCPU of a 2.6 GHz Xeon, x86_64/SSE2, headless, no video or audio
output, `test/roms/jagniccc.j64`, 60 warmup + 600 measured frames:

```
Blitter: fast     227.18 fps   avg 4.402 ms/frame   p50 4.077   p99 10.007
Blitter: accurate 207.05 fps   avg 4.830 ms/frame   p50 5.281   p99 11.894
```

Upstream got **355 / 258 fps** for the same ROM on Apple Silicon — so this
sandbox is a fair bit slower than a modern phone's big core, and *much* faster
than a cheap phone's little core. Treat the absolute number as "a server-class
core runs a light demo at ~4.4 ms/frame"; a low-end ARM core at a third of
that throughput lands in the 13–22 ms/frame range, i.e. borderline. Heavy
commercial titles will be worse. **This is the gap you actually have to close,
and no compiler flag or language change closes it.**

*(Reproduce: `make -j2`, then
`./test/tools/test_benchmark ./virtualjaguar_libretro.so test/roms/jagniccc.j64 600 --warmup 60 --blitter fast`.
Small upstream nit found while doing this: the benchmark harness is compiled
`-std=c99`, where glibc hides `CLOCK_MONOTONIC`, so `make benchmark` fails to
build the harness. It builds fine with `-std=gnu99` — a one-character-class fix
worth sending upstream.)*

---

## 6. What I'd actually do

### Don't: fork lubomyr's C++ tree

It is v2.1.3 from 2016, needs Qt5 + SDL 1.2 + SDL_ttf + guichan, has **no SIMD
anywhere**, its Android path depends on an abandoned SDL-Android glue project
and NDK r12b / API 23, and its emulator core is welded to SDL. It failed to
build in this sandbox for lack of `sdl-config` and `guichan.hpp` alone. You
would be starting ten years behind.

### Do: use the libretro core, write a new Android front-end

Three paths, cheapest first:

**A. Ship through RetroArch (zero porting work).** Android `arm64-v8a` and
`armeabi-v7a` builds are already produced by upstream CI. If the goal is "play
Jaguar games on a cheap phone", it already works — the remaining work is
configuration, not code.

**B. Standalone Android app (the interesting one).**
- Core: `src/` unchanged. Zero OS dependencies, builds for ARM64/ARMv7 with
  NEON (verified above); the whole thing links into a 2.9 MB shared library
  including the embedded BIOS images.
- Front-end: Java/Kotlin UI + a C++ JNI shim. Either drive the core through the
  24 `retro_*` functions (you get core options, save states and cheats for
  free — that's what `app/` does on the desktop, in C++ and SDL3), or call
  `src/core/jaguar.h` directly, since it is already `extern "C"`.
- Video: the core hands you an RGB framebuffer; a GLES2/3 texture blit is
  enough. Audio: AAudio/Oboe. Input: the Jaguar's 21-button keypad is the real
  UI problem on a phone, not the code.
- This is the same shape as this repo: GBabe is a **C front-end**
  (`source/main.c`, `audio.c`, `video.c`, `ui.c`) wrapped around a **C++ core**
  (`source/emu_gambatte.cpp` → Gambatte) through an `extern "C"` header
  (`include/emu.h`). It works on a 268 MHz ARM11. The language on either side
  of that boundary has never mattered — only that the boundary is a C ABI.

**C. Port the desktop app to Android.** `app/` is SDL3 + Dear ImGui +
**OpenGL 3.3** and is desktop-only (no Android hits in its CMake or docs).
SDL3 does support Android and ImGui works with GLES3, so it's feasible — but
you'd be rewriting the renderer and adding a touch UI. Biggest lift, and it
buys you a nice library UI, not speed.

### Performance checklist for a low-end device

From upstream `docs/settings-and-performance-guide.md` §4.5, in the order they
recommend (their words: *"on a low-end device this is step one, not step
three"*):

1. **Build ≥ 3.5.0.** v3.5.0 fixed a bug where cross-compiled ARM64 builds
   shipped the **scalar** blitter instead of NEON (#560). On Android this is a
   large, free win.
2. **Verify NEON is really selected.** `jni/Android.mk` maps each ABI to
   `BLITTER_SIMD` explicitly, with a comment explaining that
   `Makefile.common`'s autodetection silently lands every Android ABI on the
   scalar path under `ndk-build`. If you write your own build, do the same.
3. **Turn off per-title enhancement defaults.** Those presets (2x internal
   resolution, true color) were chosen on desktop hardware — ~30% of the frame
   on Alien vs Predator.
4. **Keep `risc_idle_skip` enabled** (it is on by default). It removes 66–87%
   of interpreted DSP opcodes. It silently disables itself if you enable DRAM
   Timing, GPU Pipeline Timing, or Blit Memoization — don't.
5. **Fast blitter last**: 1.21× typical, up to 1.58×, but it costs correctness
   (wireframe artifacts, missing geometry).
6. **Don't overclock** a device that is already struggling — it asks for more
   work, not less. Note `frameskip` is presentation-only; it does not make the
   emulated machine run any faster.

### The one change that would really matter

A **cached-IR / threaded-code dispatcher for the Tom+Jerry RISC** (upstream
issue #122, "RISC half only"). Both chips share the same ISA, so one
dispatcher covers both, and it attacks the 24–74% slice on *every* title and
every blitter mode. Upstream's own cost/benefit table rates it **High** and
says a 68K JIT is **not worth it** (0.7–2.6%). Cached IR also works on hosts
that forbid JIT (iOS without the entitlement, Switch) — unlike a true
dynarec. It is language-agnostic: you can write it in C89 like the rest of the
core, or in C++ if you insist.

If you want a Jaguar emulator that is genuinely fast on cheap phones, **that**
is the project — not a language conversion.

---

## 7. Caveats

- The profiling percentages in §5 are upstream's, measured on Apple Silicon
  desktop hosts with mid-game save states, not on Android hardware. Upstream
  says so explicitly (*"no on-device profile has been captured"* for the
  low-end class). They are the best data available and the ratios should hold,
  but the absolute numbers on your target device are unmeasured.
- My benchmark is x86_64/SSE2 on a shared vCPU with a light demo ROM
  (`jagniccc.j64`). It is here to show the build works end to end and to give a
  reproducible baseline, not to predict phone performance.
- "3 errors in 9 files" is a sample, not a census. Expect a few dozen
  syntax fixes across all 72 files, plus the header-guard and vendored-dep
  linkage work, which is where the real cost is.
- The C++ conversion experiment is throwaway. Nothing in this repo was
  modified except this document.

## 8. Working copies

| Path | What |
|---|---|
| `~/vj-qt` | `lubomyr/virtualjaguar` (v2.1.3, last commit 2016-11-06) |
| `~/vj-libretro` | `libretro/virtualjaguar-libretro` (v3.10.0) — built and benchmarked here |
| `~/vj-cpptest` | throwaway copy with the C++ build experiment (can be deleted) |

Neither clone was added to this repository.

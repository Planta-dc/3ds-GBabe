# Which console should we port Virtual Jaguar to? (PSP / PS2 / Wii / 3DS / …)

*Companion to [`docs/vj-port-analysis.md`](vj-port-analysis.md). Written
2026-10-28. Hardware facts are sourced; performance figures are **estimates**
with the method shown, because nobody has run this core on these devices yet —
§5 tells you how to replace them with measurements in an afternoon.*

---

## 1. Short answer

**PS Vita first, Wii U second. Of the four you named, Wii is the only one worth
doing, and even it is marginal.**

| Rank | Target | Est. fps, typical commercial game | Already an upstream build target? | Verdict |
|---|---|---|---|---|
| 1 | **PS Vita** | ~15–30 | **yes** (`platform=vita`, static lib) | best supported + fast enough |
| 2 | **Wii U** | ~35–55 | no (but RetroArch Wii U exists) | fastest Nintendo home console |
| 3 | **Nintendo Switch** | ~45–70 | **yes** (`platform=libnx`) | easiest win, if "modern" is allowed |
| 4 | **Wii** | ~22–33 | no | good toolchain, marginal CPU |
| 5 | **New 3DS only** | ~14–22 | **yes** (`platform=ctr`, but see §4.3) | your home turf; Old 3DS is out |
| 6 | **PSP** | ~10–16 | **yes** (`platform=psp1`) | three separate problems, weak result |
| 7 | **GameCube** | ~15–25 | no | Wii with less of everything |
| 8 | **PS2** | ~8–14 | no | slowest CPU + hardest SDK + 32 MB |
| — | **Old 3DS** | ~5–9 | yes | not viable |

*Estimated as described in §5. Treat the spread as ±2×. The **rank order** is
much more reliable than the numbers.*

**The single most important thing on this page:** the core is OS-free C89 with
zero dependencies and it already builds as a **static library** for `vita`,
`ctr`, `psp1`, `switch` and `ps3`. So the cost of *testing* a platform is a few
hours, not a porting project. Don't pick on estimates — build
`libretro_ctr.a` / `libretro_vita.a`, run 600 frames of a real ROM on the
device, and let the number decide.

---

## 2. What actually decides it

### 2.1 It is a pure interpreter, and interpreters are per-clock hungry

There is no dynarec anywhere in either repo (verified: no `dynarec`/`recompil`
/JIT in any source file). Every Jaguar instruction is dispatched through a
host switch loop, which is the worst case for embedded in-order cores.

Upstream's profiling (`docs/op-perf-profile.md`) says where the time goes:

| Subsystem | Share of frame time |
|---|---|
| DSP (Jerry RISC) | 33–67% |
| GPU (Tom RISC) | 11–74% |
| Blitter (fast / accurate) | 5–17% / up to 34% |
| Object Processor | ~1% |
| 68000 | 0.7–2.6% |

Measured here (2.6 GHz Xeon, headless, `test/roms/jagniccc.j64`): **4.4 ms/frame
with the fast blitter, 4.83 ms with accurate** — and `risc_idle_skip` is already
on in those numbers, so the 66–87% DSP-opcode saving is already banked. Upstream
sees 215–355 fps for the same class of ROM on Apple Silicon.

So: **a Jaguar frame costs ~3–5 ms on a modern desktop core.** To hit 16.6 ms
you can only afford to be ~4× slower. That is the whole game.

### 2.2 RAM: 15.6 MB of static BSS before you write a line of code

```c
#define JAG_MEMSPACE_BYTES  0x00F20000          /* src/core/vjag_memory.c */
uint8_t jagMemSpace[JAG_MEMSPACE_BYTES] = { 0 };
```

plus ~0.77 MB of embedded boot ROMs (`jaguarBootROM` 128 KB, `jaguarBootROM_M`
128 KB, `jaguarCDBootROM` 256 KB, `jaguarDevCDBootROM` 256 KB), plus libchdr
(vendored LZMA + zstd + miniz) for Jaguar CD — which **cannot currently be
switched off** (`Makefile.common` adds `SOURCES_LIBCHDR` unconditionally).

| Target | RAM available to an app | 16 MB BSS fits? |
|---|---|---|
| PSP-1000 | 32 MB total, ~24 MB in practice | **tight** |
| PS2 | 32 MB total | **tight** |
| GameCube | 24 MB MEM1 (+16 MB ARAM) | **no, needs work** |
| Old 3DS | 64 MB | yes, but wasteful |
| New 3DS | 124 MB | yes |
| Wii | 88 MB (24 MB MEM1 + 64 MB MEM2) | yes |
| Wii U | ~1 GB+ (2 GB total) | yes |
| PS Vita | ~256 MB+ | yes |
| Switch | 4 GB | yes |

If you need to trim: the 6 MB cartridge window at `jagMemSpace + 0x800000` is
mostly empty for a 1–2 MB cart and could be split out behind the accessors.

### 2.3 SIMD: only two of your candidates have it

The core has NEON and SSE2 implementations for the blitter, object processor,
shadow framebuffer and line scan, with a scalar fallback.

| Target | Integer SIMD available? | Blitter path you get |
|---|---|---|
| PS Vita (Cortex-A9) | **NEON** | full SIMD ✅ |
| Switch (Cortex-A57) | **NEON** | full SIMD ✅ |
| 3DS (ARM11, `armv6k`) | **none** | scalar fallback |
| Wii / Wii U / GameCube | paired-singles (float only) | scalar fallback |
| PSP | VFPU is **single-precision float only** | scalar fallback |
| PS2 | MMI 128-bit integer — unused today | scalar (MMI needs new code) |

Note this only matters much if you use the **accurate** blitter (up to 34% of
the frame). With the default fast blitter it's 5–17%.

---

## 3. Platform by platform

### PS Vita — the best supported-and-fast combination
- Cortex-A9 quad-core at 333 MHz (444 MHz with WiFi off), NEON, ~512 MB RAM,
  HW float, little-endian.
- `platform=vita` already produces `virtualjaguar_libretro_vita.a` with
  `arm-vita-eabi-gcc` and `STATIC_LINKING=1`. RetroArch Vita exists, so the
  core is already distributed for it.
- Nothing in the core fights you: NEON works, no endianness issue, no
  soft-float issue, RAM is ample.
- Est. ~15–30 fps on commercial titles. Playable-ish, improvable.

### Wii U — fastest Nintendo home console
- Espresso: three PowerPC 750-family cores at ~1.24 GHz (vs the Wii's single
  729 MHz), 3 MB L2, 2 GB RAM, devkitPPC works, RetroArch Wii U exists.
- **Big-endian** → needs the `MSB_FIRST` fast path (§4.1).
- Est. ~35–55 fps. No Makefile target yet, but adding one is ~10 lines.

### Wii — the best of the four you named
- Broadway: 729 MHz PowerPC 750CL, out-of-order superscalar, 32/32 KB L1 +
  **256 KB L2**, 7-stage FPU, 88 MB total RAM ([Broadway specs][bway]).
  Mature homebrew (devkitPPC + libogc), SD card loading, Wiimote/Classic/GC
  controllers, and plenty of existing emulator front-ends to model on.
- Two caveats: **big-endian** (§4.1) and no upstream Makefile target (you'd add
  one, or build through RetroArch's own build system).
- Est. ~22–33 fps. Playable for lighter titles; heavy ones (AvP, Iron Soldier)
  will struggle.

### 3DS — your home turf, but only the New 3DS
- Old 3DS: 2× ARM11 @268 MHz, **no L2**, 64 MB app RAM. New 3DS: 4× ARM11
  @804 MHz, **2 MB L2**, 124 MB app RAM ([3DS architecture][3dsarch]).
- `platform=ctr` exists and you already have devkitARM + libctru from GBabe —
  the front-end work (video blit, audio, input, file browser) is code you've
  already written once in this very repo. **That is worth a lot.**
- But: **no NEON** on `armv6k` (scalar blitter), and **the `ctr` target is
  currently broken** (§4.3).
- Old 3DS est. ~5–9 fps → not viable. New 3DS est. ~14–22 fps → marginal.

### PSP — three separate problems
- Allegrex MIPS32r2 at 222–333 MHz, **no L2**, 32 MB (64 MB on 2000+), a
  **single-precision FPU only** plus a float-only VFPU (Sony's own hardware
  doc: *"On board single precision (32bit) FPU"*), and a second 333 MHz Media
  Engine that's awkward to use.
- `platform=psp1` exists with `psp-gcc` + `STATIC_LINKING=1`.
- Three hits: **doubles fall to software** (§4.2), **no integer SIMD** for the
  blitter, and **16 MB of BSS in a 32 MB machine**.
- Est. ~10–16 fps, and that's before the soft-float penalty. Do it because you
  love the PSP, not because it's a good target.

### PS2 — don't
- Emotion Engine R5900 at 294.912 MHz, 16 KB I$ / 8 KB D$ and **no L2**, 32 MB
  RDRAM. Slowest CPU here.
- No upstream Makefile target, and the hardest environment of the set: EE/IOP
  split, GS for video, SPU2 for audio, no filesystem of its own (you load
  through uLaunchELF/OPL), and PS2SDK is a much rougher ride than devkitPPC or
  VitaSDK.
- One genuine upside: the EE has 128-bit **MMI integer** SIMD, which is a
  plausible target for a blitter back-end the core doesn't have yet.
- Est. ~8–14 fps. Research project, not a port.

---

## 4. Three things to fix before you port anything

### 4.1 Big-endian hosts pay a silent 3–4× memory penalty (Wii, Wii U, GC, PS3)

`src/core/vjag_memory.h` has two implementations of `GET32/SET32/GET16/SET16`:

```c
#if defined(VJ_BSWAP32)          /* fast: memcpy + rev/bswap */
#  define GET32(r, a) vj_get32(...)
#else                            /* slow: 4 byte loads + shifts + ors */
#  define GET32(r, a) ((r[(a)] << 24) | (r[(a)+1] << 16) | (r[(a)+2] << 8) | r[(a)+3])
#endif
```

…and the bswap macros are only defined under `#if !defined(MSB_FIRST)`. So a
big-endian host gets the **byte-wise fallback** — and these macros are on the
GPU and DSP instruction-fetch path.

**Fix (~5 lines):** on a BE host the memory is already big-endian, so no swap is
needed — just define the macros as identity so the fast `memcpy` path is taken:

```c
#else   /* MSB_FIRST */
#  define VJ_BSWAP16(v) (v)
#  define VJ_BSWAP32(v) (v)
#  define VJ_BSWAP64(v) (v)
#endif
```

(Safe: `memcpy` keeps unaligned accesses working, which a raw `*(uint32_t *)`
load would not.) Worth sending upstream regardless of which console you pick —
it also helps PS3 and Xbox 360.

Also note `src/core/vjag_memory.c` casts ~20 CD/Butch/I2S registers directly as
`uint32_t *` into `jagMemSpace`. Cartridge-only builds never touch them, but
Jaguar CD on a BE host is worth testing.

### 4.2 `double` in the event scheduler hurts any soft-float target (PSP, PS2)

`src/core/event.c` keeps all event times in `double` (`eventTime`,
`GetTimeToNextEvent`, `SubtractEventTimes`), and the main loop calls it **every
iteration**:

```c
do {
   double timeToMainEvent  = GetTimeToNextEvent(EVENT_MAIN);
   double timeToJerryEvent = GetTimeToNextEvent(EVENT_JERRY);
   ...
} while(!frameDone);
```

That loop runs once per scheduled event — hundreds to thousands of iterations
per frame. `src/jerry/dac.c` also does its audio resampler in `double`
(`i2sPhase`, `i2sRateRatio`, `frac`).

On the Wii's hardware FPU or the 3DS's VFPv2 (`-mfloat-abi=hard`, as your own
GBabe Makefile uses) this is fine. On the **PSP, which has no double-precision
FPU**, every one of those becomes a library call.

**Fix:** convert the scheduler to fixed point (integer microseconds, or Q32.32
where the fractional part is genuinely needed). It's contained in `event.c` +
`dac.c`, and it's the kind of change upstream would accept.

### 4.3 `make platform=ctr` is broken as shipped (3DS)

```make
else ifeq ($(platform), ctr)
	TARGET := $(TARGET_NAME)_libretro_$(platform).a
	CC = $(DEVKITARM)/bin/arm-none-eabi-gcc$(EXE_EXT)
	CC = $(DEVKITARM)/bin/arm-none-eabi-g++$(EXE_EXT)   <-- second assignment wins
```

The last assignment wins, so every `.c` file is handed to **g++**, which
compiles `.c` as C++ (verified: `g++ t.c` emits the mangled symbol `f(int*)`).
That lands you on exactly the failures from
[`vj-port-analysis.md`](vj-port-analysis.md) §4:

```
src/tom/gpu.c:646: error: redefinition of 'uint32_t gpu_opcode_first_parameter'
src/m68000/cpuemu.c:33: error: narrowing conversion of '4294967295' from 'unsigned int' to 'int'
src/cd/cdintf.c:1152: error: initializer-string for 'const char [32]' is too long
```

Deleting the `g++` line is the fix. Fittingly, this is the C++-conversion trap
arriving for free — in the one target you'd most want to use.

---

## 5. How to replace my estimates with real numbers

My fps figures come from anchoring on how these consoles run *other*
emulators (PS1/N64-class on Wii, SNES-class on 3DS) and scaling by clock ×
per-clock throughput for branchy interpreter code. **That is a ±2× method.**
The rank order is solid; the numbers are not.

Because the core builds as a static lib with no OS dependencies, you can get
real numbers cheaply:

```sh
# on the host, for a supported target
make platform=vita            # or ctr, psp1, switch, ps3
# -> virtualjaguar_libretro_vita.a   (STATIC_LINKING=1)
```

Then write a ~200-line `main()` for the device that:

1. links the `.a`, calls `JaguarInit()` / loads a ROM (`src/core/file.c`),
2. runs 600 frames of `JaguarExecuteNew()` with video/audio stubs,
3. prints ms/frame from a hardware timer.

That's the same shape as `test/tools/test_benchmark.c` upstream, minus the
`dlopen`. Compare against the host number: **4.4 ms/frame fast blitter on
`jagniccc.j64` at 2.6 GHz x86_64/SSE2**. Do this for two or three devices
before writing any UI code.

Reproduce the host baseline:

```sh
cd virtualjaguar-libretro
make -j2                      # needs nothing but gcc + make
cc -O2 -Wall -std=gnu99 -I. -I./src -I./src/core -I./src/tom -I./src/jerry \
   -I./src/cd -I./src/bios -I./src/m68000 -I./src/debug \
   -I./libretro-common/include -o test/tools/test_benchmark \
   test/tools/test_benchmark.c -ldl
./test/tools/test_benchmark ./virtualjaguar_libretro.so test/roms/jagniccc.j64 \
   600 --warmup 60 --blitter fast
```

(`-std=gnu99`, not the Makefile's `-std=c99` — see
[`vj-port-analysis.md`](vj-port-analysis.md) §5 for that upstream nit.)

---

## 6. What I'd do, in order

1. **Fix `platform=ctr`** (delete one line, §4.3) and send it upstream. It's a
   10-minute change that unblocks the platform you already know.
2. **Add the `MSB_FIRST` fast path** (§4.1) — 5 lines, benefits Wii/Wii U/GC/
   PS3/Xbox 360, and is worth upstreaming on its own.
3. **Build the static lib for Vita + New 3DS + Wii-U/PPC if you have the
   toolchains**, run the 600-frame benchmark on each, and pick the winner from
   the numbers.
4. **Only then write the front-end.** For the 3DS you can reuse GBabe's whole
   architecture — `main.c` / `video.c` / `audio.c` / `ui.c` behind an
   `extern "C"` interface — with `source/emu_gambatte.cpp` replaced by a thin
   `emu_jaguar.c` wrapping `jaguar.h` (already `extern "C"`-guarded).
5. **Skip PS2.** If you want a Sony target, the Vita is strictly better on
   every axis that matters here, and it's already a supported build.

And the standing recommendation from the previous analysis: don't convert the
core to C++ for this. It buys nothing (measured: identical machine code), and
on the 3DS it's actively harmful — §4.3 is what happens when C code accidentally
gets compiled as C++.

---

## Sources

[bway]: https://en.wikipedia.org/wiki/Broadway_(processor) — Broadway: 729 MHz, 32/32 KB L1, 256 KB L2, 7-stage FPU, PowerPC 750CL.
[3dsarch]: https://www.copetti.org/writings/consoles/nintendo-3ds/ — Old 3DS 2× ARM11 @268 MHz, 128 MB FCRAM (64 MB for apps); New 3DS 4× ARM11 @804 MHz, 2 MB L2, 256 MB (124 MB for apps).
- PSP: TyRaNiD, *The Naked PSP* (ps2dev.org) — Allegrex 33–333 MHz, **on-board single-precision FPU** plus VFPU, 32 MiB main RAM.
- Core internals: `src/core/vjag_memory.{c,h}`, `src/core/event.c`, `src/jerry/dac.c`, `Makefile`, `Makefile.common`, `docs/op-perf-profile.md`, `docs/settings-and-performance-guide.md` (all in `libretro/virtualjaguar-libretro` v3.10.0).
- RetroArch availability for Wii / Wii U / 3DS / PSP / PS2 / Vita / Switch: RetroArch platform listings.

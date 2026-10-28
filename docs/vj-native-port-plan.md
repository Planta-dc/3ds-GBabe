# Native port (no RetroArch) with a dynarec and your own renderer

*Follow-on to [`vj-port-analysis.md`](vj-port-analysis.md) (C vs C++) and
[`vj-port-targets.md`](vj-port-targets.md) (which console). Written 2026-10-28.
Facts are sourced to upstream files/issues; effort numbers are one-person
estimates and are labelled as such.*

---

## 0. Verdict on the three asks

| You want | Verdict | Reality |
|---|---|---|
| **Native port, no RetroArch** | ✅ **Yes, straightforward** | 2–4 weeks. The core links as a static lib and has a clean C API. Upstream's own `app/` does exactly this. |
| **Your own renderer** | ✅ **Yes, but it's ~300 lines, not a project** | The Jaguar has no GPU. "Rendering" here means: upload the framebuffer as a texture and scale it. Days. |
| **A dynarec** | ⚠️ **Yes, and it's the real project** | 1–3 months for a host-independent one, 3–6 for a true JIT. This is where all the work is. |

**Native port ≠ RetroArch.** RetroArch is just one frontend that talks to the
core through the `retro_*` C ABI. Nothing forces you to use it:

- `make platform=vita` (or `ctr`/`psp1`/`switch`/`ps3`) produces
  `virtualjaguar_libretro_<plat>.a` with `STATIC_LINKING = 1`.
- `src/core/jaguar.h` is already `extern "C"`-guarded — link it and call
  `JaguarInit()` / `JaguarSetScreenBuffer()` / `JaguarExecuteNew()` directly.
- Upstream's `app/` (SDL3 + ImGui desktop app) *is* a native port: it links the
  core statically and only uses the `retro_*` API. Copy that idea, replace SDL
  with your console's SDK.

On the 3DS you've already built this exact shape in this repo: `main.c` /
`video.c` / `audio.c` / `ui.c` in front of `emu_gambatte.cpp` behind an
`extern "C"` header. Same pattern, different core.

---

## 1. "Renderer": there is nothing to hardware-accelerate

This is the part most people get wrong, so here is the evidence.

**The Jaguar has no GPU.** Tom is a RISC CPU. It computes polygon edges in
*software* and hands the blitter a per-scanline **span** (`A1_PIXEL` start +
`B_COUNT` inner width). The blitter is a span filler, not a rasterizer. The core
is therefore a pure software renderer:

```
libretro.c: video_cb(videoBuffer, game_width, game_height, game_width << 2)
            with RETRO_PIXEL_FORMAT_XRGB8888
src/tom/tom.c: rasterizes into screenBuffer (the uint32_t* you passed to
               JaguarSetScreenBuffer())
```

**Upstream measured that there is no sub-pixel information to recover**
(`docs/hires-upscaling-design.md` §0), in Cybermorph gameplay:

> **658,759 of 659,263 gouraud blits have OUTER == 1** (one span, one
> scanline), `UPDA1F` is **never** set … For those titles there is **no
> sub-pixel edge information anywhere in the blitter interface** — none to
> recover, at any N.

Compare with DuckStation: the PS1 hands its rasterizer *vertex coordinates*, so
running it at Nx recomputes real edges. The Jaguar can't — the edges were
already flattened to integer spans by a RISC program before the blitter saw
them. Running the blitter at Nx is bit-for-bit nearest-neighbour upscaling.

**Upstream already explored GPU offload and said no**
(`docs/libretro-compute-exploration.md`, §0):

> **Recommendation: (i) — skip GPU compute for now.** … Reject (iv)
> (paraLLEl-style LLE blitter) outright — **the Jaguar's blitter/CPU
> interleaving makes the synchronization cost structurally fatal in a way the
> N64 RDP's does not**. Option (c) (output-stage-only compute) adds nothing
> over frontend shaders … do not build it.

### So what your renderer actually is

Per frame: take the XRGB8888 buffer, upload it as a texture, scale it, maybe
run a CRT/scanline shader, present. Handle mid-run geometry changes
(`game_width`/`game_height` are set per content and can change —
`retro_get_system_av_info` + `SET_GEOMETRY`; base is 320×240 × `shadowHiresN`).

| Platform | Presentation path |
|---|---|
| 3DS | PICA200 texture, or the 2D blitters (GBabe already does a whole-frame RGB565 blit via libctru) |
| Wii / Wii U | GX texture + TV-out (`GX_*` / libogc) |
| PS Vita | GLES2 through vitaGL, or direct GXM |
| PSP | `sceGu*` texture + `sceGuTexImage` |
| Switch | deko3d or SDL |

**And the counter-intuitive consequence:** on a handheld, turn hi-res *off*.
Upstream's own risk **R4** is that the Nx OP composite cost is screen-area
driven and "**worst on handhelds**"; the per-title 2x presets cost ~30% of the
frame on Alien vs Predator. More rendering = slower, and it buys no detail on
the blitter side. Ship 1x and spend the budget on the CPU core.

---

## 2. The dynarec: what it has to be

### Target the two RISC cores, not the 68K

| Subsystem | Frame share | Dynarec it? |
|---|---|---|
| DSP (Jerry RISC) | 33–67% | **yes** |
| GPU (Tom RISC) | 11–74% | **yes** |
| Blitter | 5–34% | no (see §1) |
| Object Processor | ~1% | no |
| 68000 | 0.7–2.6% | **no** — upstream: "not worth the GPLv2/GPLv3 licence dance with UAE-JIT or the ARM-only constraint of Cyclone68k" |

Tom's GPU and Jerry's DSP **share an ISA** → one dispatcher covers both. That is
why upstream rates this the single highest-ROI item (issue #122).

### Upstream's own ordering

From `docs/op-perf-profile.md` §"Recommendation":

> **Path A — RISC dynarec / cached IR (#122, RISC half only).** … New spike:
> feasibility of a Tom RISC **cached-IR / threaded-code dispatcher** — the
> lowest-cost approach, works on JIT-restricted platforms (iOS, Switch). Block
> JIT comes later if the cached IR proves the model.

Take that ordering. A cached-IR / threaded-code dispatcher decodes each guest
instruction once into `(handler pointer, operands)` and then runs the array —
no host machine code, works on every platform including ones that forbid JIT,
and gets you most of the fetch/decode win.

### Three things that will bite you

**a) Self-modifying code.** GPU/DSP programs live in *writable* memory —
GPU RAM at `jagMemSpace[0xF03000]`, DSP RAM at `[0xF1B000]`, `tomRam8[0x4000]`,
plus the 2 MB of main RAM. Games patch their own code (upstream's v3.7.1 notes
describe a self-modifying anti-tamper check). You need write-triggered
invalidation wired into the existing write path
(`JaguarWriteByte/Word/Long` → the MMU dispatch). Cart ROM at `0x800000+` is
read-only and needs no invalidation.

**b) Cycle accounting.** Pipeline hazards are only partially modelled
(`gpu_reg_ready`, `GPU_PIPE_LOCAL_LOAD` in `src/tom/gpu.c`). Issue **#313**: the
emulated DSP *outruns real hardware* because hazards aren't modelled, and the
core already interprets more RISC opcodes per frame than silicon could retire
(416k–590k vs ~443k cycles/field). Several titles **already run too fast**. A
dynarec that doesn't charge the same cycles per block makes that worse. Every
translated block must carry the interpreter's cycle cost.

**c) Re-entrancy.** From the comment at the top of `GPUExec()`:

> Re-entrant runs (the OP executes the GPU inline from a halfline callback, see
> op.c) re-read on each entry…

Your dispatcher must tolerate being entered recursively from inside a halfline
callback.

### Expected return

Rough, and it depends on the title (RISC is 24–74% of the frame):

| Approach | RISC-slice speedup | Whole-frame speedup |
|---|---|---|
| Cached IR / threaded code | ~1.4–2× | ~1.2–1.7× |
| True JIT (host machine code) | ~2.5–5× | ~1.5–3× |

Which is how the earlier fps estimates move: New 3DS ~14–22 → ~20–40;
Vita ~15–30 → ~25–55; Wii ~22–33 → ~35–60; Wii U ~35–55 → ~55–90.

---

## 3. Which host, now that a dynarec is on the table

A dynarec emits **host** machine code, so you pick an ISA and that decides which
consoles you can serve with one backend:

| Host ISA | Consoles covered | JIT plumbing (verified) |
|---|---|---|
| **ARM** | Vita, Switch, 3DS | Vita/Switch: normal RWX + cache flush. **3DS: awkward — see below.** |
| **PowerPC (BE)** | Wii, Wii U, GameCube | Proven on Wii hardware (Wii64's dynarec); libogc `DCFlushRange` + `ICInvalidateRange`. **Bonus: host and guest are both big-endian**, so guest loads need no byte-swap — which also removes the `MSB_FIRST` accessor penalty from [`vj-port-targets.md`](vj-port-targets.md) §4.1. |
| **MIPS32r2** | PSP (and PS2's R5900 is MIPS64) | Standard PSPSDK calls: `sceKernelDcacheWritebackInvalidateAll()`, `sceKernelIcacheInvalidateAll()` (+ `...Range` variants). Mechanically easy. |

### The 3DS is the awkward one

libctru exposes `svcFlushProcessDataCache` / `svcStoreProcessDataCache` /
`svcInvalidateProcessDataCache` — **data cache only**. Instruction-cache
invalidation is privileged. From the RetroArch 3DS core report (dynarec crash
fix by justinweiss):

> After the dynarec writes new instructions, it has to flush the instruction
> and data caches. Some of these flush operations are privileged on the 3DS, so
> the clear cache functions have to run through `svcBackdoor`. The Nintendo
> implementation (and CFW reimplementation) of svcBackdoor has a problem where
> **interrupts and context switches will cause crashes**.

That's the source of the long-running PCSX-ReARmed / gpSP dynarec crashes on
3DS (only usable with Luma3DS 8.0+). It works — but you'd be debugging cache
coherence through a CFW backdoor on the weakest CPU of the set.

### Recommendation

**Primary: PS Vita (ARM).** Fastest ARM host of the handhelds, NEON (the
blitter SIMD path works), JIT is clean, 512 MB RAM, HW float, little-endian,
already an official upstream build target, and upstream's own profiling says the
Vita-class CPU is roughly where Jaguar becomes comfortably playable.

A single ARM backend then gives you **Switch** (trivial, 1 GHz A57 + NEON +
4 GB) for free, and **3DS** later if you want it (accepting the icache pain and
that Old 3DS will never be enough).

If you'd rather stay on Nintendo home consoles: **Wii U** for speed (1.24 GHz
tri-core PPC), **Wii** for the better-documented path (Wii64 proves PPC
dynarec there) — and on either, big-endian works in your favour.

---

## 4. Don't hand-roll an emitter — use SLJIT

[SLJIT](https://github.com/zherczeg/sljit) is a platform-independent low-level
JIT compiler (it's the code generator behind PCRE2). Its LIR compiles to:

> x86 32/64 · **ARM 32 (ARMv5, ARMv7, Thumb2)** · **ARM 64** · **PowerPC 32/64**
> · **MIPS 32/64** · RISC-V 32/64 · s390x · LoongArch

and it supports, out of the box, exactly the awkward bits you'd otherwise write
yourself: **self-modifying code**, **byte order reverse (endianness switching —
precisely what a big-endian Jaguar needs on a little-endian ARM host)**,
unaligned memory accesses, SIMD, direct register access, tail calls.

So the architecture is:

```
Jaguar RISC (Tom + Jerry, shared ISA)
        │  decode once
        ▼
   cached IR  ──────────────► interpreter over the IR array   (stage 2, portable)
        │
        └─ SLJIT LIR ───────► ARM (Vita/Switch/3DS) or PPC (Wii/Wii U)  (stage 3)
```

Verify SLJIT's licence against the core's GPLv3 before you ship, but it's a
permissive BSD-style licence and is already used inside GPL projects.

---

## 5. Staged plan (one person, rough)

| Stage | Work | Effort | Gate |
|---|---|---|---|
| **0** | Native frontend: video/audio/input/ROM browser/options. Link the static lib, call `jaguar.h` directly. | 2–4 weeks | A ROM boots and plays (slowly) |
| **1** | On-device benchmark harness: load a ROM, run 600 frames, print ms/frame from a hardware timer | 2–3 days | **Do this before the dynarec** — you need the baseline and it re-tests every later change |
| **2** | Cached-IR / threaded-code dispatcher for Tom+Jerry, with SMC invalidation and cycle accounting | 4–8 weeks | Frame-to-frame identical to the interpreter on upstream's test corpus (framebuffer + audio + savestate hashes) |
| **3** | SLJIT backend for your host ISA | 2–4 months | Same identity gate; ≥1.5× over stage 2 |
| **4** | *(optional)* OP Stage-3 hi-res scaled objects | — | Enhancement only. Skip on handhelds (upstream R4) |

Stage 2 is the one to send upstream — it's their #122 Path A, it's
host-independent, and it would land.

### Identity is the only correctness gate that matters

Upstream's standard for every optimisation (`risc_idle_skip`, blit memoization)
is byte-identical framebuffer + audio + savestate-hash streams across a 148-cart
corpus. They have `make benchmark`, `make test` and
`test/tools/test_benchmark`. Build your dispatcher against the same gate, or
you'll ship something that's fast and subtly wrong.

---

## 6. What you reuse vs. what you write

**Reuse as-is (don't touch):**

- All 72 `.c` files under `src/` — emulation, event scheduler, memory map,
  blitter (scalar/NEON/SSE2), object processor, CD interface, embedded BIOSes
- `src/core/jaguar.h` (already `extern "C"`)
- `src/core/event.c` (scheduler), `src/core/file.c` (ROM/COFF/ABS loading)
- The test corpus + benchmark harness

**You write:**

- Frontend: video, audio, input, ROM browser, options — a few thousand lines
- Renderer: a texture upload and a scale — a few hundred lines
- The dispatcher / dynarec + code cache + invalidation
- (Optional) your own video conversion if you want RGB565 on the 3DS

**You must not write:**

- A GPU rasterizer for Jaguar graphics. It does not exist to be written — see §1.
- A 68K dynarec. 0.7–2.6% of the frame.

---

## 7. Risks, honestly

1. **The dynarec is the project.** Stages 0+1 are a month; stage 3 is a
   multi-month, host-specific engineering effort with a save-state and
   timing-compatibility surface. Budget for it or stop at stage 2.
2. **Timing regressions.** Games already run too fast (#313). Any dispatcher
   that doesn't charge interpreter-accurate cycles will break more of them.
3. **SMC bugs are the classic dynarec failure mode** — they show up as one
   glitchy game three months in. Build invalidation in from day one, not later.
4. **Don't pick the 3DS for the JIT** unless you enjoy `svcBackdoor` debugging;
   and Old 3DS is out regardless (268 MHz, no L2, no NEON).
5. **Still don't convert the core to C++** ([`vj-port-analysis.md`](vj-port-analysis.md)):
   measured identical machine code, and on the 3DS the `ctr` build already
   accidentally does it and breaks (§4.3 of the targets doc). Keep the core C89
   and put C++ — if any — only in your frontend.

## Sources

- `docs/op-perf-profile.md`, `docs/hires-upscaling-design.md`,
  `docs/libretro-compute-exploration.md`, `docs/hires-stage0-census.md`,
  `docs/settings-and-performance-guide.md`, `src/tom/gpu.c`,
  `src/tom/tom.c`, `src/core/jaguar.c`, `Makefile` (platform targets) — all in
  `libretro/virtualjaguar-libretro` v3.10.0.
- SLJIT backends: <https://github.com/zherczeg/sljit>
- libctru cache SVCs: <https://libctru.devkitpro.org/svc_8h.html>; 3DS
  privileged icache-invalidate / `svcBackdoor` crashes: RetroArch 3DS core
  progress reports (justinweiss dynarec fix).
- PSPSDK cache APIs: <https://pspdev.github.io/pspsdk/group__Utils.html>

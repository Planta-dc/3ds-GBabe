# GBabe

A Game Boy / Game Boy Color emulator for the **Nintendo 3DS**, built around
[Walnut-CGB](https://github.com/Mr-PauI/Walnut-CGB) — a single-header,
performance-oriented CGB emulator core.

Target: **Old 3DS / 2DS first.** No DSP firmware dump, no GPU shaders, no
dynamic recompilation, no external libraries beyond libctru.

<p align="center"><img src="docs/screenshot-acid2.png" width="400" alt="dmg-acid2 rendered by GBabe"></p>

*(the image above is the `dmg-acid2` test ROM rendered through GBabe's actual
render path by `tools/host`, not a mock-up)*

---

## Status

| Area | State |
|---|---|
| DMG (original Game Boy) emulation | core provided by Walnut-CGB |
| CGB (Game Boy Color) emulation | core provided by Walnut-CGB |
| Video (RGB565, line-by-line blit, 3 scale modes) | done |
| Audio (minigb_apu → CSND, buffered) | done |
| ROM browser, in-game menu, config file | done |
| `.sav` cartridge RAM (auto-save every 30 s) | done |
| Optional boot ROM | done |
| Save states | **not yet** |
| Link cable, cheat codes, SGB borders | not planned |

`.github/workflows/build.yml` builds the release artifacts on GitHub's runners
(manually or on push) but has not had its first run yet; the
platform-independent parts are compiled and run on the host by `tools/host`,
which renders the screenshot above. See [docs/PORT-NOTES.md](docs/PORT-NOTES.md).

---

## Build

You need [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the
`3ds-dev` package group (devkitARM + libctru).

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export PATH=$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH

make -j4
```

That produces `gbabe.3dsx` (and `gbabe.smdh`). Copy `gbabe.3dsx` to
`sdmc:/3ds/gbabe.3dsx` and launch it from the Homebrew Launcher.

### Installable .cia

```sh
make cia
```

You need two extra tools that are **not** part of devkitPro:

* [makerom](https://github.com/3DSGuy/Project_CTR/releases) — builds the CIA
* [bannertool](https://github.com/carstene1ns/3ds-bannertool/releases) — builds
  the banner and the icon

Put both on `PATH`, or point the build at them:

```sh
make cia MAKEROM=/path/to/makerom BANNERTOOL=/path/to/bannertool
```

The result is `gbabe.cia`, installed either by copying it to the SD card and
opening it with [FBI](https://github.com/Steveice10/FBI), or with GodMode9.
The CIA is unsigned, so it needs a console that will run unsigned titles (CFW);
target title id `0x000400000EABE100`, product code `CTR-H-GBABE`, unique ID
`0xEABE1` — change `APP_UNIQUE_ID` in the `Makefile` if it clashes with
something you already have installed.

`.cia`, `.3dsx` and `.smdh` are git-ignored; only sources and artwork live here.

### Building in CI (no devkitPro on your machine)

`.github/workflows/build.yml` can be started manually: **Actions → build → Run
workflow**, then pick:

| Input | Meaning |
|---|---|
| `target` | `both` (default), `3dsx`, or `cia` |
| `release_tag` | optional — upload the results to this GitHub release tag (created if missing) |

Finished files appear under the run's *Artifacts* (`gbabe-3dsx`, `gbabe-cia`).
Pushes to `main` and pull requests build both automatically, after the host
tests pass.

Because GitHub only reports workflow syntax errors *after* a push — and a broken
file means no job runs at all — the repository ships a checker:

```sh
python3 tools/lint-workflow.py            # YAML, expressions, step ids, bash -n
```

It catches the traps that bite here, such as a step output named `3dsx`, which
is not addressable as `steps.plan.outputs.3dsx` (GitHub's expression grammar
rejects property names starting with a digit) and has to be called something
like `want_3dsx`.

The workflow runs in the official `devkitpro/devkitarm` container and fetches
makerom (v0.19.0) and bannertool (v1.2.3) with
`.github/scripts/install-3ds-packaging-tools.sh`, which you can also run locally
to set up the two packaging tools:

```sh
sudo bash .github/scripts/install-3ds-packaging-tools.sh
```

### The file browser

The browser opens in the folder of the last ROM you played, takes D-pad/Circle
Pad to move, `L`/`R` to jump a page and `A`/`B` to go in and back; `START`
quits. It only shows folders and `.gb`/`.gbc`/`.sgb`/`.dmg` files, and it scans
a folder when you enter it rather than on every key press — on a card with a lot
of files that difference is the difference between instant and a second per
tap.

### Expectations on hardware

Written for the Old 3DS: one interpreter, no dynarec, no GPU. DMG titles are
the comfortable case; most CGB titles should hold full speed with the default
"fit height" scaler, and the menu has a frame-skip toggle for the ones that
don't.

If a game *is* too slow, the bottom screen tells you why — it prints a
per-frame breakdown once a second:

```
                               <- fps, and the wall time per frame
fps 58.9  (17 ms)
core 9.8 + blit 1.6 + wait 5.1 ms
```

* a large **core** figure is the interpreter: nothing else is competing for
  that time, so it needs a faster core (see below) or frame skip;
* a large **blit** figure means the scaled copy is the cost, which scale mode
  changes (the pixel-perfect 160x144 mode is the cheapest to fill);
* a large **wait** figure is normal — it is the time the console spends idle
  waiting for the next Game Boy frame.

Known speed levers, in order of effect:

| lever | effect |
|---|---|
| `-O3 -flto` (the default, `OPT=` in the `Makefile`) | ~45% faster emulation loop than `-O2` |
| X (turbo) | runs two emulated frames per presented one, at the cost of dropped frames |
| frame skip in the menu | halves the blit cost, keeps emulation speed |
| sound off in the menu | removes the APU mixing and the CSND hand-off |
| `WALNUT_GB_16_BIT_OPS` in `third_party/walnut/walnut_cgb.h` | a few percent, at the cost of accuracy in some games |

---

## SD card layout

```
sdmc:/3ds/gbabe.3dsx          the app
sdmc:/3ds/gbabe/config.cfg    settings (created on first run)
sdmc:/3ds/gbabe/saves/        reserved for saved states
sdmc:/3ds/gbabe/bootrom/
    dmg_boot.bin              optional, not included
    cgb_boot.bin              optional, not included
```

ROMs can live anywhere; the browser starts at `sdmc:/` and remembers the last
folder. Save files are written **next to the ROM** as `<romname>.sav`, so they
work with other emulators too.

No ROMs or boot ROMs are included. Supply your own dumps.

---

## Controls

In game:

| 3DS | Game Boy |
|---|---|
| A / B | A / B |
| Start / Select | Start / Select |
| D-Pad or Circle Pad | D-Pad |
| X | toggle fast-forward (2x) |
| Y | next colour palette (DMG games) |
| Touch the bottom screen | open the menu |
| L + R | open the menu (alternative) |

In the menu: D-Pad to move, L/R or Left/Right to change a value, A to select,
B to resume. The menu has palette, scaling, frame skip, sound, status line,
reset, "load another ROM" and "quit".

---

## Settings

`config.cfg` is plain text, so a bad value can be fixed with any editor:

```ini
scale=0        ; 0 = fit height, 1 = stretch 400x240, 2 = original 160x144
palette=0      ; 0 = auto (per game), see source/palette.c for the table
frameskip=0    ; 0 = off, 1 = present every other frame
audio=1
hud=0          ; 1 = status lines on the bottom screen while playing
rom=sdmc:/roms/game.gbc
```

---

## How it fits together

```
source/main.c     services, input mapping, 59.7275 Hz frame pacing
source/emu.c      Walnut-CGB glue: ROM/save RAM callbacks, save files, boot ROM
source/video.c    RGB565 scanline blit into the rotated 3DS framebuffer
source/audio.c    minigb_apu -> software FIFO -> CSND blocks
source/palette.c  DMG palettes, including Game Boy Color "auto" colouring
source/ui.c       ROM browser, in-game menu, status readout
source/config.c   key=value settings on the SD card
resources/        CIA spec (gbabe.rsf) and banner, generated by tools/make-artwork.py
third_party/      Walnut-CGB (MIT) and minigb_apu (MIT), vendored unmodified
tools/host/       PC harness: builds and runs the front-end without a 3DS
```

Design notes, the reasoning behind the CSND choice, and the measurements taken
while validating the port are in [docs/PORT-NOTES.md](docs/PORT-NOTES.md).

### Host harness

```sh
make -C tools/host            # builds ./gbabe-host
make -C tools/host check      # type-checks main.c and ui.c too
./tools/host/gbabe-host game.gb 300 out.ppm 0
```

It substitutes `tools/host/3ds.h` for libctru (same prototypes, no behaviour),
runs the real `emu.c`/`video.c`/`palette.c` and dumps the top screen as a PPM —
handy for checking a rendering or palette change without a console.

---

## Credits and licences

* **[Walnut-CGB](https://github.com/Mr-PauI/Walnut-CGB)** by Mr-PauI, MIT —
  the emulator core, a reimplementation of and successor to
  [Peanut-GB](https://github.com/deltabeard/Peanut-GB) by Mahyar Koshkouei.
  Vendored unmodified in `third_party/walnut/`.
* **[minigb_apu](https://github.com/deltabeard/Peanut-GB)** by Alex Baines and
  Mahyar Koshkouei, MIT — the audio processing unit emulation.
  Vendored unmodified in `third_party/minigb_apu/`.
* Everything else in this repository is MIT licensed, see [LICENSE](LICENSE).

Neither upstream project endorses this port; bugs in the front-end are mine,
bugs in the emulation are theirs (and worth reporting upstream).

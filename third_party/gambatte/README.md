# Gambatte 0.5.0 (vendored emulation core)

This directory is the emulation core GBabe is built with (see the top-level
`Makefile`). It is the only one since the Walnut-CGB port was removed. It is not
written by this project and it is not under this project's MIT licence: Gambatte
is **GPL-2.0** (see `COPYING`), which makes every binary built with it GPL-2.0
as well.

## Where it comes from

| | |
|---|---|
| Upstream | https://github.com/libretro/gambatte-libretro |
| Commit | `7809bb6` ("Fetch translations & Recreate libretro_core_options_intl.h") |
| Core version string | `Gambatte v0.5.0` (`retro_get_system_info`) |
| Licence | GPL-2.0 |
| Author | Sindre Aamås, plus the libretro fork's maintainers |

Gambatte 0.5.0 has not been released since 2015; `gambatte-libretro` is where
the core is still maintained. The version string above is the one the fork
reports, which is also the one RetroArch shows.

Update with:

```sh
git clone https://github.com/libretro/gambatte-libretro /tmp/gambatte
git -C /tmp/gambatte checkout <new commit>
# copy the pieces listed below, then re-apply the three patches
```

## What was copied, and what was left out

Copied unmodified:

```
libgambatte/src/**          the emulator (cpu, ppu, memory, sound, MBCs)
libgambatte/include/**      gambatte.h, inputgetter.h, gbint.h
gambatte-array.h            from the repo's common/, used by statesaver.cpp
uncopyable.h                ditto
COPYING                     GPL-2.0
```

Left out: everything that only exists to serve the RetroArch front-end —
`libretro.cpp` (the libretro API wrapper), `libretro-common/` (VFS, file
streams, string helpers, core options), `blipper.c`/`cc_resampler.c` (its
audio resampler), `net_serial.cpp`, the translations, and the JNI bits.
GBabe uses the core's own C++ API (`gambatte.h`), because the video, audio,
input and save-file paths this front-end needs already exist in `source/`.

## Patches (three, all small)

1. **`libgambatte/src/gambatte-memory.h`** — the non-libretro branch of the
   class calls `cart_.loadSavedata()`/`saveSavedata()`, which do not exist in
   this tree (upstream only ever builds the libretro variant). The two lines
   are removed; GBabe owns the `.sav` file (see `source/emu_gambatte.cpp`).
2. **`compat/gambatte_log.h` + `compat/gambatte_log.cpp`** — the core calls
   `gambatte_log(RETRO_LOG_INFO, ...)`. Instead of vendoring libretro's
   `libretro.h` for one enum, this shim supplies the severity names and routes
   the message to GBabe's debug log.
3. **`compat/cartridge_accessors.cpp`** — `Cartridge::savedata_ptr()`,
   `savedata_size()`, `rtcdata_ptr()` and `rtcdata_size()` live in the repo's
   `mem/cartridge_libretro.cpp`, next to a lot of file-stream code. Only those
   four functions (plus the header tables they need) are copied here.

The core is compiled with `-D__LIBRETRO__`, because that is the configuration
upstream tests and several accessors (`savedata_ptr`, the colour-correction
hooks, `gbcToRgb32`) only exist in it. Nothing from the libretro API is linked.

`-DVIDEO_RGB565` makes `video_pixel_t` a `uint16_t` in the same 565 layout the
3DS framebuffer and `source/video.c` use, so the core draws straight into the
line buffer with no conversion. Both the core and the front-end's backend must
be compiled with it, or the pixel pointers do not match.

## Building

`make -C third_party/gambatte` on its own, or through either of the project's
Makefiles, which pass the compiler and the architecture flags down:

```sh
make -C third_party/gambatte CXX=arm-none-eabi-g++ AR=arm-none-eabi-ar \
     ARCH="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft" OPT="-O3 -flto"
```

It produces `libgambatte.a`, built without exceptions and RTTI (as upstream
does). `third_party/gambatte/build/` and the `.a` are git-ignored.

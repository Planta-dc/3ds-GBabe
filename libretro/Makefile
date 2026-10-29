# GBabe as a libretro core - the base for running the emulation under a mature
# frontend (RetroArch, including the old 3DS build).
#
#   make            libgbabe_libretro.so, loadable by a desktop RetroArch
#   make test       build and run test_frontend, then compare the picture with
#                   the reference frame the 3DS path is held to
#   make ctr        libgbabe_libretro_ctr.a, to link into RetroArch's 3DS build
#
# The core itself is host-independent: it uses the libretro API and Gambatte,
# and nothing from source/ or include/ - those are the 3DS front-end, and they
# are what the comparison is testing.
#
# Not part of the 3DS application build (the top-level Makefile).

CC   ?= cc
CXX  ?= c++
AR   ?= ar
ROOT := ..

# -fPIC for the shared library; the host test links the same objects.
CFLAGS  ?= -O2 -g -Wall -Wextra
PICFLAGS := -fPIC

CORE_DEFS := -DVIDEO_RGB565 -DHAVE_STDINT_H \
             -I$(ROOT)/third_party/gambatte/libgambatte/include

# Two archives on the host, for one reason: the shared library has to be built
# from position-independent code, and a static archive built without -fPIC
# cannot be linked into a .so ("relocation R_X86_64_PC32 ... can not be used").
# The test binary links the ordinary one, the .so links the PIC one.
CORE_LIB     := $(ROOT)/third_party/gambatte/libgambatte-host.a
CORE_LIB_PIC := $(ROOT)/third_party/gambatte/libgambatte-host-pic.a

OBJDIR := obj
CORE_OBJ := $(OBJDIR)/gbabe_core.o
TEST_BIN := gbabe-coretest

CXXSTD := -std=gnu++11 -fno-exceptions -fno-rtti
CSTD   := -std=gnu11

all: libgbabe_libretro.so

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(OBJDIR)/gbabe_core.o: gbabe_core.cpp libretro.h $(OBJDIR)
	$(CXX) $(CFLAGS) $(PICFLAGS) $(CXXSTD) $(CORE_DEFS) -c $< -o $@

libgbabe_libretro.so: $(CORE_OBJ) $(CORE_LIB_PIC)
	$(CXX) -shared -o $@ $(CORE_OBJ) $(CORE_LIB_PIC) -lm

$(CORE_LIB_PIC):
	$(MAKE) -C $(ROOT)/third_party/gambatte CXX="$(CXX)" AR="$(AR)" \
		OPT="-O2 -fPIC" BUILD=build-host-pic LIB=libgambatte-host-pic.a

# The vendored Gambatte core, built with the host compiler (the same archive
# tools/host links).
$(CORE_LIB):
	$(MAKE) -C $(ROOT)/third_party/gambatte CXX="$(CXX)" AR="$(AR)" \
		OPT="-O2" BUILD=build-host LIB=libgambatte-host.a

$(OBJDIR)/test_frontend.o: test_frontend.c libretro.h $(OBJDIR)
	$(CC) $(CFLAGS) $(CSTD) -c $< -o $@

$(TEST_BIN): $(OBJDIR)/test_frontend.o $(OBJDIR)/gbabe_core.o $(CORE_LIB)
	$(CXX) -o $@ $(OBJDIR)/test_frontend.o $(OBJDIR)/gbabe_core.o \
		$(CORE_LIB) -lm

# The render check is the same one the 3DS path passes (`make -C tools/host
# acid2`), on the same reference frame: whatever else the core does, it has to
# draw dmg-acid2 correctly, or running it under RetroArch is pointless.
test: $(TEST_BIN)
	./$(TEST_BIN) $(ROOT)/test/dmg-acid2.gb $(OBJDIR)/core-acid2.ppm 300
	python3 $(ROOT)/tools/check-render.py --render $(OBJDIR)/core-acid2.ppm \
		--reference $(ROOT)/test/dmg-acid2-reference.ppm \
		--window 0,0,160,144

# --------------------------------------------------------------------------- #
# 3DS: a static archive, to be linked into RetroArch's ctr build.
#
#   export DEVKITARM=$DEVKITPRO/devkitARM
#   make ctr
#
# RetroArch 3DS links cores into its own executable (it has no dlopen), so this
# is an archive rather than a .so. Nothing here touches libctru: a core talks to
# the frontend through the libretro API and through nothing else.
#
# Unverified for the same reason as everything else in this repository's 3DS
# build: no devkitARM outside the CI image. `make ctr` runs in docker/
# ci-build.sh, so at least it is compiled by the real toolchain on every build.
# --------------------------------------------------------------------------- #
# Same architecture flags as the 3DS application build, so the core's objects
# can be linked next to them.
CTR_ARCH  := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CTR_OBJDIR := obj-ctr
CTR_OBJ   := $(CTR_OBJDIR)/gbabe_core.o
ARM_LIB   := $(ROOT)/third_party/gambatte/libgambatte.a
PREFIX    := $(DEVKITARM)/bin/arm-none-eabi-

$(CTR_OBJDIR):
	@mkdir -p $(CTR_OBJDIR)

$(CTR_OBJDIR)/gbabe_core.o: gbabe_core.cpp libretro.h $(CTR_OBJDIR)
	$(PREFIX)g++ -c $(CTR_ARCH) -O3 -fomit-frame-pointer -fno-exceptions \
		-fno-rtti $(CXXSTD) $(CORE_DEFS) -o $@ $<

ctr: $(CTR_OBJ) $(ARM_LIB)
	$(PREFIX)ar rcs libgbabe_libretro_ctr.a $(CTR_OBJ)

# The 3DS build of Gambatte, in the shape the top-level Makefile produces it
# (third_party/gambatte/libgambatte.a) - so `make -C libretro ctr` works from a
# clean tree and also finds the archive the 3DS build already made.
$(ARM_LIB):
	$(MAKE) -C $(ROOT)/third_party/gambatte \
		CXX="$(PREFIX)g++" AR="$(PREFIX)ar" \
		ARCH="$(CTR_ARCH)" OPT="-O3 -flto -DNDEBUG"

clean:
	rm -rf $(OBJDIR) $(CTR_OBJDIR) libgbabe_libretro.so \
		libgbabe_libretro_ctr.a $(TEST_BIN)

.PHONY: all test ctr clean

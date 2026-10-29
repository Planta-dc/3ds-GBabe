# Host build of the GBabe front-end, for quick iteration without a 3DS.
#
#   make            build ./gbabe-host
#   make check      compile main.c as well (type check)
#   make acid2      render dmg-acid2 and compare it with the reference frame
#   make cross      check that both cores render the same picture, at every scale
#   make run ROM=game.gb FRAMES=300 OUT=shot.ppm
#   make bench ROM=game.gb     per-frame cost of the core and the blit
#   make logtest               check the debug log's format
#   make audiotest             check that the sound path keeps CSND fed
#   make pacertest             check the frame pacing arithmetic
#   make presenttest           check the present path's wait-or-tear trade
#   make drawtest              check the scaled transpose against a reference
#   make resamptest            check the audio rate matcher's arithmetic
#   make dectest               check the decimator is bit-exact, and faster
#
# Uses ../host/3ds.h in place of libctru. Not part of the 3DS build.

CC      ?= cc
CXX     ?= c++
AR      ?= ar
CFLAGS  ?= -O3 -flto -g -Wall -Wextra
ROOT    := ../..

CORE_SRC  := $(ROOT)/source/emu_gambatte.cpp
# video_pixel_t must be the same type in the backend and in the core, so
# VIDEO_RGB565 is set for both: 16-bit pixels, which is what video.c takes.
CORE_DEFS := -DVIDEO_RGB565 -DHAVE_STDINT_H \
             -I$(ROOT)/third_party/gambatte/libgambatte/include
# Its own library file: the 3DS build puts an arm-none-eabi archive at
# libgambatte.a in the same directory, and linking that into a PC binary fails
# with "ELF object format mismatch".
CORE_LIB  := $(ROOT)/third_party/gambatte/libgambatte-host.a
LD        := $(CXX)

INCLUDE := -I. -I$(ROOT)/include $(CORE_DEFS)

# CFLAGS goes to both compilers, so the language standard is picked per file:
# g++ rejects -std=gnu11 and gcc rejects -std=gnu++11.
CSTD    := -std=gnu11
CXXSTD  := -std=gnu++11 -fno-exceptions -fno-rtti

FRONT_C := $(ROOT)/source/video.c $(ROOT)/source/audio.c \
           $(ROOT)/source/resamp.c $(ROOT)/source/decimator.c \
           $(ROOT)/source/palette.c $(ROOT)/source/config.c \
           $(ROOT)/source/log.c \
           $(ROOT)/source/ui.c $(ROOT)/source/ui_gfx.c \
           $(ROOT)/source/ui_screens.c $(ROOT)/source/ui_font.c

OBJDIR  := obj
FRONT_OBJ := $(addprefix $(OBJDIR)/,$(notdir $(FRONT_C:.c=.o)))
CORE_OBJ  := $(addprefix $(OBJDIR)/,$(notdir $(CORE_SRC:.cpp=.o)))

BIN     := gbabe-host

vpath %.c $(sort $(dir $(FRONT_C)))
vpath %.cpp $(sort $(dir $(CORE_SRC)))

all: $(BIN)

$(BIN): $(FRONT_OBJ) $(CORE_OBJ) $(OBJDIR)/stub.o $(OBJDIR)/harness.o $(CORE_LIB)
	$(LD) $(CFLAGS) -o $@ $(FRONT_OBJ) $(CORE_OBJ) $(OBJDIR)/stub.o \
		$(OBJDIR)/harness.o $(CORE_LIB) -lm

$(OBJDIR)/%.o: %.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) $(CSTD) $(DEFS) $(INCLUDE) -c $< -o $@

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(OBJDIR)
	$(CXX) $(CFLAGS) $(CXXSTD) $(DEFS) $(INCLUDE) -c $< -o $@

# The vendored Gambatte core, built with the host compiler.
$(CORE_LIB):
	$(MAKE) -C $(ROOT)/third_party/gambatte CXX="$(CXX)" AR="$(AR)" \
		OPT="-O2" BUILD=build-host LIB=libgambatte-host.a

# main.c defines its own main(); compile it with main renamed. Everything else,
# the UI included, is linked for real by the $(BIN) rule above.
check:
	$(CC) $(CFLAGS) $(CSTD) $(DEFS) $(INCLUDE) -Dmain=gbabe_main -c \
		-o /tmp/gbabe-main.o $(ROOT)/source/main.c
	@echo "main.c and the UI: type check ok"

# Palette-independent render regression: the emulated picture is compared with
# test/dmg-acid2-reference.ppm by shade rank, so no DMG palette can change the
# result - only a wrong layer or a wrong shade fails it.
#
# SCALE_ORIG (2) puts the frame 1:1 in the middle of the 400x240 top screen
# (x 120..279, y 48..191), which is the only scale where the render can be
# compared with the 160x144 reference pixel for pixel.
acid2: $(BIN)
	./$(BIN) $(ROOT)/test/dmg-acid2.gb 300 $(OBJDIR)/acid2.ppm 2
	python3 $(ROOT)/tools/check-render.py --render $(OBJDIR)/acid2.ppm \
		--reference $(ROOT)/test/dmg-acid2-reference.ppm --window 120,48,160,144

# The sound path is real-time, so this takes two seconds of wall clock. The
# stand-in CSND is only enabled for this run.
audiotest: $(OBJDIR)/audio.o $(OBJDIR)/stub.o $(OBJDIR)/log.o \
	   $(OBJDIR)/config.o $(OBJDIR)/palette.o $(OBJDIR)/audiotest.o \
	   $(OBJDIR)/resamp.o $(OBJDIR)/decimator.o
	$(CC) $(CFLAGS) -o gbabe-audiotest $(OBJDIR)/audio.o $(OBJDIR)/stub.o \
		$(OBJDIR)/log.o $(OBJDIR)/config.o $(OBJDIR)/palette.o \
		$(OBJDIR)/resamp.o $(OBJDIR)/decimator.o $(OBJDIR)/audiotest.o -lm
	GBABE_HOST_AUDIO=1 ./gbabe-audiotest
	@echo
	@echo "audiotest: again with only two CSND channels (the mono fallback)"
	GBABE_HOST_AUDIO=2 ./gbabe-audiotest

# video.c on its own, plus the libctru stand-in: two frames that fit in a panel
# period must wait for the vblank, and one that does not must present at once.
presenttest: $(OBJDIR)/video.o $(OBJDIR)/stub.o presenttest.c \
	     $(ROOT)/include/video.h
	$(CC) $(CFLAGS) $(CSTD) $(DEFS) $(INCLUDE) -o gbabe-presenttest \
		$(OBJDIR)/video.o $(OBJDIR)/stub.o presenttest.c
	./gbabe-presenttest

# The decimator: bit-exact against the two-stage version it replaced, in every
# block size the backend can hand it, plus an indicative timing.
dectest: $(ROOT)/source/decimator.c $(ROOT)/include/decimator.h dectest.c
	$(CC) $(CFLAGS) $(CSTD) $(DEFS) $(INCLUDE) -o gbabe-dectest \
		$(ROOT)/source/decimator.c dectest.c
	./gbabe-dectest

# The audio rate matcher, as arithmetic: the control law and the resampler on
# their own, with no console, no DSP and no timing in the way.
resamptest: $(ROOT)/source/resamp.c $(ROOT)/include/resamp.h resamptest.c
	$(CC) $(CFLAGS) $(CSTD) $(DEFS) $(INCLUDE) -o gbabe-resamptest \
		$(ROOT)/source/resamp.c resamptest.c -lm
	./gbabe-resamptest

# The transpose, one panel pixel at a time, against a reference built from the
# documented geometry - at every band height, every scale and an odd pitch.
# video.o is compiled with GBABE_HOST_TESTS so the test can choose the band.
drawtest: $(OBJDIR)/video-test.o $(OBJDIR)/stub.o drawtest.c \
	  $(ROOT)/include/video.h $(ROOT)/include/gb3ds.h
	$(CC) $(CFLAGS) $(CSTD) $(DEFS) -DGBABE_HOST_TESTS $(INCLUDE) \
		-o gbabe-drawtest $(OBJDIR)/video-test.o $(OBJDIR)/stub.o \
		drawtest.c
	./gbabe-drawtest

$(OBJDIR)/video-test.o: $(ROOT)/source/video.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) $(CSTD) $(DEFS) -DGBABE_HOST_TESTS $(INCLUDE) -c $< -o $@

run: $(BIN)
	./$(BIN) $(ROM) $(or $(FRAMES),300) $(or $(OUT),screen.ppm)

# Same sources, but with its own main().
bench: $(FRONT_OBJ) $(CORE_OBJ) $(OBJDIR)/stub.o $(OBJDIR)/bench.o $(CORE_LIB)
	$(LD) $(CFLAGS) -o gbabe-bench $(FRONT_OBJ) $(CORE_OBJ) $(OBJDIR)/stub.o \
		$(OBJDIR)/bench.o $(CORE_LIB) -lm
	@rom="$(ROM)"; \
	case "$$rom" in \
	/*) ;; \
	*) [ -f "$$rom" ] || rom="$(ROOT)/$$rom" ;; \
	esac; \
	./gbabe-bench "$$rom" $(or $(FRAMES),600)

# The log is what a bug report is read from, so its shape is checked rather
# than assumed: timestamps, one line per event, truncation, off switch.
logtest: $(OBJDIR)/log.o $(OBJDIR)/stub.o $(OBJDIR)/logtest.o
	$(CC) $(CFLAGS) -o gbabe-logtest $(OBJDIR)/log.o $(OBJDIR)/stub.o \
		$(OBJDIR)/logtest.o -lm
	./gbabe-logtest

# Frame pacing is arithmetic on tick counts, so the cases that matter (a frame
# that is early, one that is late, a stall long enough to need a new schedule)
# are tested exactly, without a console and without waiting. Built in one step
# from the source, like the uipreview renderer.
pacetest: pacing_test.c $(ROOT)/include/pace.h $(ROOT)/include/gb3ds.h
	$(CC) $(CFLAGS) $(CSTD) $(DEFS) $(INCLUDE) -o gbabe-pacetest pacing_test.c
	./gbabe-pacetest
$(OBJDIR)/bench.o: bench.c $(ROOT)/include/emu.h
$(OBJDIR)/audiotest.o: audiotest.c $(ROOT)/include/audio.h
$(OBJDIR)/harness.o: harness.c $(ROOT)/include/emu.h

clean:
	rm -rf $(OBJDIR) $(BIN) gbabe-bench gbabe-logtest gbabe-audiotest \
		gbabe-pacetest gbabe-presenttest *.ppm

.PHONY: all check acid2 audiotest run bench logtest pacertest drawtest clean

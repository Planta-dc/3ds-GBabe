# Host build of the GBabe front-end, for quick iteration without a 3DS.
#
#   make            build ./gbabe-host
#   make check      compile main.c as well (type check)
#   make run ROM=game.gb FRAMES=300 OUT=shot.ppm
#   make bench ROM=game.gb     per-frame cost of the core and the blit
#   make logtest               check the debug log's format
#
# Which emulation core is compiled follows the top-level Makefile: gambatte by
# default, walnut to compare. Both are on the same sources otherwise, so
#
#   make bench ROM=test/dmg-acid2.gb CORE=walnut
#   make bench ROM=test/dmg-acid2.gb
#
# is the same measurement on the same ROM through two cores.
#
# Uses ../host/3ds.h in place of libctru. Not part of the 3DS build.

CC      ?= cc
CXX     ?= c++
AR      ?= ar
CFLAGS  ?= -O3 -flto -g -Wall -Wextra
ROOT    := ../..

CORE    ?= gambatte

ifeq ($(CORE),gambatte)
CORE_SRC  := $(ROOT)/source/emu_gambatte.cpp
# video_pixel_t must be the same type in the backend and in the core, so
# VIDEO_RGB565 is set for both: 16-bit pixels, which is what video.c takes.
CORE_DEFS := -DGBABE_CORE_GAMBATTE -DVIDEO_RGB565 \
             -I$(ROOT)/third_party/gambatte/libgambatte/include
CORE_LIB  := $(ROOT)/third_party/gambatte/libgambatte.a
LD        := $(CXX)
else ifeq ($(CORE),walnut)
CORE_SRC  := $(ROOT)/source/emu.c
CORE_DEFS := -DGBABE_CORE_WALNUT
CORE_LIB  :=
LD        := $(CC)
else
$(error CORE must be gambatte or walnut, not '$(CORE)')
endif

INCLUDE := -I. -I$(ROOT)/include -I$(ROOT)/third_party/walnut \
           -I$(ROOT)/third_party/minigb_apu $(CORE_DEFS)

DEFS    := -DMINIGB_APU_AUDIO_FORMAT_S16SYS

# CFLAGS goes to both compilers, so the language standard is picked per file:
# g++ rejects -std=gnu11 and gcc rejects -std=gnu++11.
CSTD    := -std=gnu11
CXXSTD  := -std=gnu++11 -fno-exceptions -fno-rtti

FRONT_C := $(ROOT)/source/emu.c $(ROOT)/source/video.c $(ROOT)/source/audio.c \
           $(ROOT)/source/palette.c $(ROOT)/source/config.c \
           $(ROOT)/source/log.c \
           $(ROOT)/source/ui.c $(ROOT)/source/ui_gfx.c \
           $(ROOT)/source/ui_screens.c $(ROOT)/source/ui_font.c \
           $(ROOT)/third_party/minigb_apu/minigb_apu.c

# Only one backend is compiled. The other one would duplicate its symbols.
ifeq ($(CORE),gambatte)
FRONT_C := $(filter-out $(ROOT)/source/emu.c,$(FRONT_C))
CORE_CXX_OBJ := $(CORE_SRC)
else
CORE_CXX_OBJ :=
endif

# Objects are per core, so switching CORE recompiles only what differs.
OBJDIR  := obj/$(CORE)
FRONT_OBJ := $(addprefix $(OBJDIR)/,$(notdir $(FRONT_C:.c=.o)))
CORE_OBJ  := $(addprefix $(OBJDIR)/,$(notdir $(CORE_SRC:.cpp=.o)))
ifeq ($(CORE),walnut)
CORE_OBJ :=
endif

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
		OPT="-O2"

# main.c defines its own main(); compile it with main renamed. Everything else,
# the UI included, is linked for real by the $(BIN) rule above.
check:
	$(CC) $(CFLAGS) $(CSTD) $(DEFS) $(INCLUDE) -Dmain=gbabe_main -c \
		-o /tmp/gbabe-main.o $(ROOT)/source/main.c
	@echo "main.c and the UI: type check ok"

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

$(OBJDIR)/bench.o: bench.c $(ROOT)/include/emu.h
$(OBJDIR)/harness.o: harness.c $(ROOT)/include/emu.h

clean:
	rm -rf $(OBJDIR) $(BIN) gbabe-bench gbabe-logtest *.ppm

.PHONY: all check run bench logtest clean

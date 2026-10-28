# Host build of the GBabe front-end, for quick iteration without a 3DS.
#
#   make            build ./gbabe-host
#   make check      compile main.c as well (type check)
#   make run ROM=game.gb FRAMES=300 OUT=shot.ppm
#   make bench ROM=game.gb     per-frame cost of the core and the blit
#   make logtest               check the debug log's format
#
# Uses ../host/3ds.h in place of libctru. Not part of the 3DS build.

CC      ?= cc
CFLAGS  ?= -O3 -flto -g -std=gnu11 -Wall -Wextra
ROOT    := ../..

INCLUDE := -I. -I$(ROOT)/include -I$(ROOT)/third_party/walnut \
           -I$(ROOT)/third_party/minigb_apu

SRC     := $(ROOT)/source/emu.c $(ROOT)/source/video.c $(ROOT)/source/audio.c \
           $(ROOT)/source/palette.c $(ROOT)/source/config.c \
           $(ROOT)/source/log.c \
           $(ROOT)/source/ui.c $(ROOT)/source/ui_gfx.c \
           $(ROOT)/source/ui_screens.c $(ROOT)/source/ui_font.c \
           $(ROOT)/third_party/minigb_apu/minigb_apu.c \
           stub.c harness.c

DEFS    := -DMINIGB_APU_AUDIO_FORMAT_S16SYS

BIN     := gbabe-host

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(DEFS) $(INCLUDE) -o $@ $(SRC) -lm

# main.c defines its own main(); compile it with main renamed. Everything else,
# the UI included, is linked for real by the $(BIN) rule above.
check:
	$(CC) $(CFLAGS) $(DEFS) $(INCLUDE) -Dmain=gbabe_main -c \
		-o /tmp/gbabe-main.o $(ROOT)/source/main.c
	@echo "main.c and the UI: type check ok"

run: $(BIN)
	./$(BIN) $(ROM) $(or $(FRAMES),300) $(or $(OUT),screen.ppm)

# Same sources, but with its own main().
BENCH_SRC := $(filter-out harness.c,$(SRC)) bench.c

# ROM may be given relative to the repository root, which is how the workflows
# call it (`make -C tools/host bench ROM=test/dmg-acid2.gb`).
bench: $(BENCH_SRC) bench.c
	$(CC) $(CFLAGS) $(DEFS) $(INCLUDE) -o gbabe-bench $(BENCH_SRC) -lm
	@rom="$(ROM)"; \
	case "$$rom" in \
	/*) ;; \
	*) [ -f "$$rom" ] || rom="$(ROOT)/$$rom" ;; \
	esac; \
	./gbabe-bench "$$rom" $(or $(FRAMES),600)

# The log is what a bug report is read from, so its shape is checked rather
# than assumed: timestamps, one line per event, truncation, off switch.
logtest: $(ROOT)/source/log.c logtest.c
	$(CC) $(CFLAGS) $(DEFS) $(INCLUDE) -o gbabe-logtest $(ROOT)/source/log.c \
		stub.c logtest.c -lm
	./gbabe-logtest

clean:
	rm -f $(BIN) gbabe-bench gbabe-logtest *.ppm

.PHONY: all check run bench logtest clean

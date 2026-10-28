# Host build of the GBabe front-end, for quick iteration without a 3DS.
#
#   make            build ./gbabe-host
#   make check      compile main.c and ui.c as well (type check)
#   make run ROM=game.gb FRAMES=300 OUT=shot.ppm
#
# Uses ../host/3ds.h in place of libctru. Not part of the 3DS build.

CC      ?= cc
CFLAGS  ?= -O2 -g -std=gnu11 -Wall -Wextra
ROOT    := ../..

INCLUDE := -I. -I$(ROOT)/include -I$(ROOT)/third_party/walnut \
           -I$(ROOT)/third_party/minigb_apu

SRC     := $(ROOT)/source/emu.c $(ROOT)/source/video.c $(ROOT)/source/audio.c \
           $(ROOT)/source/palette.c $(ROOT)/source/config.c \
           $(ROOT)/third_party/minigb_apu/minigb_apu.c \
           stub.c harness.c

DEFS    := -DMINIGB_APU_AUDIO_FORMAT_S16SYS

BIN     := gbabe-host

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(DEFS) $(INCLUDE) -o $@ $(SRC) -lm

# main.c and ui.c define their own main()/UI glue; only type-check them here,
# plus a full link with main renamed so nothing is left unverified.
check:
	$(CC) $(CFLAGS) $(DEFS) $(INCLUDE) -Dmain=gbabe_main -c \
		-o /tmp/gbabe-main.o $(ROOT)/source/main.c
	$(CC) $(CFLAGS) $(DEFS) $(INCLUDE) -c -o /tmp/gbabe-ui.o $(ROOT)/source/ui.c
	@echo "main.c and ui.c: type check ok"

run: $(BIN)
	./$(BIN) $(ROM) $(or $(FRAMES),300) $(or $(OUT),screen.ppm)

clean:
	rm -f $(BIN) *.ppm

.PHONY: all check run clean

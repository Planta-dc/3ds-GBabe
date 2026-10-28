# Renders the bottom-screen layouts to PPM files on a PC, using the real
# ui_gfx.c / ui_screens.c and the libctru stand-in from ../host.
#
#   make                  build ./uipreview
#   make run              build and write *.ppm into out/
#   make png              same, converted to PNG (needs python3 + Pillow)
#   make asan             build ./uistress with sanitizers and run it
#
# Not part of the 3DS build.

CC      ?= cc
CFLAGS  ?= -O2 -g -std=gnu11 -Wall -Wextra
ROOT    := ../..

INCLUDE := -I../host -I$(ROOT)/include

SRC     := $(ROOT)/source/ui_gfx.c $(ROOT)/source/ui_screens.c \
           $(ROOT)/source/ui_font.c \
           ../host/stub.c preview.c

BIN     := uipreview

# Same sources, plus the hostile-input test, under ASan/UBSan.
UI_SRC  := $(ROOT)/source/ui_gfx.c $(ROOT)/source/ui_screens.c \
           $(ROOT)/source/ui_font.c $(ROOT)/source/video.c ../host/stub.c
SAN     := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

all: $(BIN)

$(BIN): $(SRC) $(ROOT)/include/gb3ds.h $(ROOT)/include/ui_gfx.h \
	$(ROOT)/include/ui_screens.h
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $(SRC)

uistress: $(UI_SRC) stress.c $(ROOT)/include/gb3ds.h $(ROOT)/include/ui_gfx.h \
	  $(ROOT)/include/ui_screens.h
	$(CC) -std=gnu11 -g -O1 -Wall -Wextra $(SAN) $(INCLUDE) -o $@ \
		$(UI_SRC) stress.c

# ASan needs a shadow mapping it can fit in the address space, and a kernel with
# high ASLR entropy (vm.mmap_rnd_bits=32, the default on newer Ubuntu) hands out
# addresses it cannot: the binary dies with "AddressSanitizer:DEADLYSIGNAL"
# before it runs a line. That is the runner, not the test, so turn address
# randomisation off for this process when the platform lets us.
ASAN_RUN = if setarch $$(uname -m) -R true >/dev/null 2>&1; then \
		   setarch $$(uname -m) -R ./uistress; \
	   else \
		   ./uistress; \
	   fi

asan: uistress
	@$(ASAN_RUN)

run: $(BIN)
	@mkdir -p out
	./$(BIN) out

png: run
	python3 ppm2png.py out

clean:
	rm -f $(BIN) uistress out/*.ppm out/*.png

.PHONY: all run png asan clean

# Renders the bottom-screen layouts to PPM files on a PC, using the real
# ui_gfx.c / ui_screens.c and the libctru stand-in from ../host.
#
#   make                  build ./uipreview
#   make run              build and write *.ppm next to the binary
#   make png              same, converted to PNG (needs python3 + Pillow)
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

all: $(BIN)

$(BIN): $(SRC) $(ROOT)/include/ui_gfx.h $(ROOT)/include/ui_screens.h
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $(SRC)

run: $(BIN)
	@mkdir -p out
	./$(BIN) out

png: run
	@python3 -c "$$PNG_SCRIPT" 2>/dev/null || true

clean:
	rm -f $(BIN) *.ppm out/*.ppm out/*.png

.PHONY: all run png clean

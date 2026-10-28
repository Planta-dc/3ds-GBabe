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
           $(ROOT)/source/ui_font.c ../host/stub.c
SAN     := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

all: $(BIN)

$(BIN): $(SRC) $(ROOT)/include/ui_gfx.h $(ROOT)/include/ui_screens.h
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $(SRC)

uistress: $(UI_SRC) stress.c $(ROOT)/include/ui_gfx.h $(ROOT)/include/ui_screens.h
	$(CC) -std=gnu11 -g -O1 -Wall -Wextra $(SAN) $(INCLUDE) -o $@ \
		$(UI_SRC) stress.c

asan: uistress
	./uistress

run: $(BIN)
	@mkdir -p out
	./$(BIN) out

png: run
	python3 ppm2png.py out

clean:
	rm -f $(BIN) uistress out/*.ppm out/*.png

.PHONY: all run png asan clean

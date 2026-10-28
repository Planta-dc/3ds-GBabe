# libgambatte.a - the vendored Gambatte 0.5.0 core, built as a static library.
#
# Called from the top-level Makefile (devkitARM) and from tools/host/Makefile
# (native), so it takes the compiler and the architecture flags from the caller:
#
#   $(MAKE) -C third_party/gambatte CXX=$(CXX) AR=$(AR) ARCH="$(ARCH)" OPT="$(OPT)"
#
# Nothing here knows whether it is building for a 3DS or for a PC.

CXX      ?= g++
AR       ?= ar
OPT      ?= -O2
ARCH     ?=
CROSS    ?=

BUILD    := build
LIB      := libgambatte.a

# VIDEO_RGB565 makes video_pixel_t a uint16_t in the same 565 layout the 3DS
# framebuffer and source/video.c already use, so the core draws straight into
# the line buffer with no conversion.
#
# -fno-exceptions/-fno-rtti are what the core's own Makefile.libretro uses: the
# code has no exception or RTTI use, and on a 268 MHz ARM11 the tables and
# unwind info are dead weight.
# HAVE_STDINT_H is what upstream's Makefile.libretro passes: gbint.h needs it to
# pull in <stdint.h>, and without it the core falls back to its own typedefs and
# then relies on <string> happening to include <stdint.h> for uint16_t. That
# accident of libstdc++ version is why the core compiled with GCC 12 and not
# with 13.
CORE_FLAGS := -DVIDEO_RGB565 -DHAVE_STDINT_H -D__LIBRETRO__ -DNDEBUG \
              -fno-exceptions -fno-rtti -fno-strict-aliasing \
              -ffunction-sections -fdata-sections -fomit-frame-pointer

# The front-end's headers (log.h) live at the repository root.
ROOT    ?= ../..
INCLUDE := -Ilibgambatte/include -Ilibgambatte/src -Icompat -I$(ROOT)/include -I.

SRC := $(sort $(wildcard libgambatte/src/*.cpp) \
              $(wildcard libgambatte/src/*/*.cpp) \
              compat/gambatte_log.cpp compat/cartridge_accessors.cpp)
OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(SRC))

.PHONY: all clean
all: $(LIB)

$(LIB): $(OBJ)
	@rm -f $@
	$(AR) rcs $@ $(OBJ)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(OPT) -g $(CORE_FLAGS) $(ARCH) $(INCLUDE) -MMD -MP -c $< -o $@

clean:
	@rm -rf $(BUILD) $(LIB)

-include $(OBJ:.o=.d)

#---------------------------------------------------------------------------------
# GBabe - Game Boy / Game Boy Color emulator for the Nintendo 3DS
#
# Built with devkitARM + libctru. Typical use:
#   export DEVKITPRO=/opt/devkitpro
#   export DEVKITARM=$DEVKITPRO/devkitARM
#   make -j4
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# Which emulation core to build.
#
#   CORE=gambatte   (default) Gambatte 0.5.0. On the PC it needs 1.6x-3.4x less
#                   time per frame than Walnut-CGB, which is the whole reason
#                   this port exists: 268 MHz is not much.
#   CORE=walnut     Walnut-CGB. Kept so both cores can be put on the same
#                   console, with the same ROM, and compared from the debug
#                   log's "core=" figure instead of from an opinion.
#
# Both backends implement include/emu.h; nothing else changes.
#---------------------------------------------------------------------------------
CORE        ?=  gambatte

ifeq ($(CORE),gambatte)
# TOPDIR, not CURDIR: this Makefile is re-invoked from build/, where CURDIR is
# the build directory.
CORE_DEFS   :=  -DGBABE_CORE_GAMBATTE -DVIDEO_RGB565 -DHAVE_STDINT_H \
                -I$(TOPDIR)/third_party/gambatte/libgambatte/include
CORE_LIB    :=  $(TOPDIR)/third_party/gambatte/libgambatte.a
CORE_STRAY  :=  emu.c
else ifeq ($(CORE),walnut)
CORE_DEFS   :=  -DGBABE_CORE_WALNUT
CORE_LIB    :=
CORE_STRAY  :=  emu_gambatte.cpp
else
$(error CORE must be gambatte or walnut, not '$(CORE)')
endif

BUILD       :=  build
SOURCES     :=  source third_party/minigb_apu
DATA        :=  data
INCLUDES    :=  include third_party/walnut third_party/minigb_apu
GRAPHICS    :=  gfx
GFXBUILD    :=  $(BUILD)

APP_TITLE       := GBabe
APP_DESCRIPTION := Game Boy / Game Boy Color
APP_AUTHOR      := GBabe contributors

#---------------------------------------------------------------------------------
# Two installable builds must not share an id, or the second one replaces the
# first instead of sitting next to it.
#---------------------------------------------------------------------------------
ifeq ($(CORE),walnut)
TARGET      :=  gbabe-walnut
APP_TITLE   :=  GBabe (Walnut)
else
TARGET      :=  gbabe
endif

#---------------------------------------------------------------------------------
# packaging (CIA)
#---------------------------------------------------------------------------------
VERSION_MAJOR   :=  0
VERSION_MINOR   :=  1
VERSION_MICRO   :=  0

ifeq ($(CORE),walnut)
APP_PRODUCT_CODE :=  CTR-H-GBAW
APP_UNIQUE_ID    :=  0xEABE2
else
APP_PRODUCT_CODE :=  CTR-H-GBABE
APP_UNIQUE_ID    :=  0xEABE1
endif

ICON_PNG    :=  icon.png
BANNER_PNG  :=  resources/banner.png
BANNER_WAV  :=  resources/banner.wav
RSF         :=  resources/gbabe.rsf
CIA_TARGET  :=  $(TARGET).cia

# Packaging tools. Override on the command line if they are not on PATH, e.g.
#   make cia MAKEROM=/path/to/makerom BANNERTOOL=/path/to/bannertool
MAKEROM     ?=  makerom
BANNERTOOL  ?=  bannertool

#---------------------------------------------------------------------------------
# code generation
#---------------------------------------------------------------------------------
ARCH    :=  -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

# -O3 -flto. Measured on the host build (`make -C tools/host bench`), the
# difference from -O2 is only a couple of percent there; LTO is kept because the
# core is one giant switch in a header-only library and the win should be bigger
# where the compiler inlines worse (ARM11), but that part is unmeasured.
# Override with `make OPT=-O2` if a toolchain update ever miscompiles something.
OPT     ?=  -O3 -flto -DNDEBUG

CFLAGS  :=  -g -Wall -Wextra $(OPT) -ffunction-sections -fdata-sections \
            -fno-strict-aliasing -fomit-frame-pointer \
            -std=gnu11 $(ARCH)

CFLAGS  +=  $(INCLUDE) -D__3DS__ -DMINIGB_APU_AUDIO_FORMAT_S16SYS $(CORE_DEFS)

# The Gambatte backend is C++, and devkitARM's rules hand CFLAGS to g++ as well.
# -std=gnu11 is not a C++ mode, so it is dropped here and the C++ standard is
# set explicitly; the core is built without exceptions and RTTI, like its own
# Makefile.libretro does it.
CXXFLAGS :=  $(filter-out -std=gnu11,$(CFLAGS)) -std=gnu++11 \
             -fno-exceptions -fno-rtti

ASFLAGS :=  -g $(ARCH)
# -flto has to be given to the link as well for the objects to be optimised.
LDFLAGS  =  -specs=3dsx.specs -g $(ARCH) $(OPT) -Wl,-Map,$(notdir $*.map)

LIBS    :=  $(CORE_LIB) -lctru -lm

LIBDIRS :=  $(CTRULIB)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)

export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                    $(foreach dir,$(DATA),$(CURDIR)/$(dir)) \
                    $(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir))
export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))

# Only one backend is compiled: the other one's symbols would be duplicated.
CFILES      :=  $(filter-out $(CORE_STRAY),$(CFILES))
CPPFILES    :=  $(filter-out $(CORE_STRAY),$(CPPFILES))
SFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES   :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))
GFXFILES    :=  $(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))
BINFILES    :=  $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
export LD   :=  $(CC)
else
export LD   :=  $(CXX)
endif

export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES_BIN     := $(addsuffix .o,$(BINFILES)) \
                         $(PICAFILES:.v.pica=.shbin.o) $(SHLISTFILES:.shlist=.shbin.o)
export OFILES         := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES := $(PICAFILES:.v.pica=_shbin.h) $(SHLISTFILES:.shlist=_shbin.h) \
                 $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE   :=  $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                     $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                     -I$(CURDIR)/$(BUILD)
export LIBPATHS  :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export _3DSXDEPS :=  $(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(NO_SMDH)),)
export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

.PHONY: all clean distclean cia artwork check-tools core-lib

all: core-lib $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# The vendored core is its own small project with its own flags; building it
# here means `make` alone still produces a working .3dsx from a clean tree.
ifeq ($(CORE),gambatte)
core-lib:
	@$(MAKE) --no-print-directory -C $(TOPDIR)/third_party/gambatte \
		CXX="$(if $(strip $(CXX)),$(CXX),arm-none-eabi-g++)" \
		AR="$(if $(strip $(AR)),$(AR),arm-none-eabi-ar)" \
		ARCH="$(ARCH)" OPT="$(OPT)"
else
core-lib:
	@:
endif

$(BUILD):
	@mkdir -p $@

#---------------------------------------------------------------------------------
# CIA: ELF + SMDH icon + banner -> makerom
#
#   make cia          (needs makerom and bannertool on PATH, see README)
#---------------------------------------------------------------------------------
cia: check-tools all
	@echo banner ...
	@$(BANNERTOOL) makebanner -i $(BANNER_PNG) -a $(BANNER_WAV) -o $(BUILD)/banner.bnr
	@echo smdh ...
	@$(BANNERTOOL) makesmdh -s "$(APP_TITLE)" \
		-l "$(APP_TITLE) - $(APP_DESCRIPTION)" \
		-p "$(APP_AUTHOR)" -i $(ICON_PNG) -o $(BUILD)/icon.icn
	@echo makerom ...
	@$(MAKEROM) -f cia -o $(CIA_TARGET) \
		-target t -exefslogo \
		-elf $(OUTPUT).elf \
		-rsf $(RSF) \
		-icon $(BUILD)/icon.icn \
		-banner $(BUILD)/banner.bnr \
		-major $(VERSION_MAJOR) -minor $(VERSION_MINOR) -micro $(VERSION_MICRO) \
		-DAPP_TITLE="$(APP_TITLE)" \
		-DAPP_PRODUCT_CODE="$(APP_PRODUCT_CODE)" \
		-DAPP_UNIQUE_ID=$(APP_UNIQUE_ID)
	@echo "built $(CIA_TARGET)"

check-tools:
	@command -v $(BANNERTOOL) >/dev/null 2>&1 || { \
		echo "error: bannertool not found (needed for the CIA)"; \
		echo "       install it or pass BANNERTOOL=/path/to/bannertool"; \
		exit 1; }
	@command -v $(MAKEROM) >/dev/null 2>&1 || { \
		echo "error: makerom not found (needed for the CIA)"; \
		echo "       install it or pass MAKEROM=/path/to/makerom"; \
		exit 1; }

# Regenerate icon.png / resources/banner.{png,wav} from code.
artwork:
	@python3 tools/make-artwork.py

# `clean` leaves the vendored core's objects alone: switching CORE back and
# forth should not cost a full rebuild of 30 C++ files. `distclean` removes
# those too.
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf $(CIA_TARGET)

distclean: clean
	@$(MAKE) --no-print-directory -C $(TOPDIR)/third_party/gambatte clean

else

$(OUTPUT).3dsx : $(OUTPUT).elf $(_3DSXDEPS)
$(OFILES_SOURCES) : $(HFILES)
$(OUTPUT).elf : $(OFILES)

.PRECIOUS : %.t3x %.shbin

-include $(DEPSDIR)/*.d

endif

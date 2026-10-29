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
# The emulation core: Gambatte 0.5.0, vendored in third_party/gambatte/.
#
# There used to be a second one (Walnut-CGB, CORE=walnut). It was removed: it
# needed 1.6x-3.4x more time per emulated frame on the PC, which on a 268 MHz
# ARM11 is the difference between full speed and not, and keeping two backends
# behind include/emu.h meant every front-end change had to be checked twice.
#---------------------------------------------------------------------------------
# TOPDIR, not CURDIR: this Makefile is re-invoked from build/, where CURDIR is
# the build directory.
CORE_DEFS   :=  -DVIDEO_RGB565 -DHAVE_STDINT_H \
                -I$(TOPDIR)/third_party/gambatte/libgambatte/include
CORE_LIB    :=  $(TOPDIR)/third_party/gambatte/libgambatte.a

BUILD       :=  build
SOURCES     :=  source
DATA        :=  data
INCLUDES    :=  include
GRAPHICS    :=  gfx
GFXBUILD    :=  $(BUILD)

APP_TITLE       := GBabe
APP_DESCRIPTION := Game Boy / Game Boy Color
APP_AUTHOR      := GBabe contributors

TARGET      :=  gbabe

#---------------------------------------------------------------------------------
# packaging (CIA)
#---------------------------------------------------------------------------------
VERSION_MAJOR   :=  0
VERSION_MINOR   :=  3
VERSION_MICRO   :=  5

APP_PRODUCT_CODE :=  CTR-H-GBABE
APP_UNIQUE_ID    :=  0xEABE1

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
# -marm and -mfpu=vfp are named rather than implied. RetroArch's 3DS build
# passes both explicitly, and the emulator is a big switch over opcodes: if the
# toolchain were configured --with-mode=thumb this would be Thumb-1 code (ARMv6K
# has no Thumb-2), which is the last place anyone would look for a frame that
# does not fit. Naming the state costs nothing and makes the binary's instruction
# set a property of this file rather than of the toolchain build.
ARCH    :=  -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft \
            -marm -mfpu=vfp

# -O3 -flto. Measured on the host build (`make -C tools/host bench`), the
# difference from -O2 is only a couple of percent there; LTO is kept because the
# core is one giant switch in a header-only library and the win should be bigger
# where the compiler inlines worse (ARM11), but that part is unmeasured.
# Override with `make OPT=-O2` if a toolchain update ever miscompiles something.
OPT     ?=  -O3 -flto -DNDEBUG

CFLAGS  :=  -g -Wall -Wextra $(OPT) -ffunction-sections -fdata-sections \
            -fno-strict-aliasing -fomit-frame-pointer \
            -std=gnu11 $(ARCH)

# Which build this is: the version says 0.3.5 for everything that lands while
# 0.3.4 is current, so the log identifies its own binary by date + revision
# instead. `date` is used rather than only the revision because a build from a
# dirty tree is exactly the one worth identifying. Git is absent when the tree
# was unpacked from an archive, hence the fallback.
BUILD_DATE  :=  $(shell date -u +%Y%m%d)
GIT_REV     :=  $(shell git rev-parse --short HEAD 2>/dev/null || echo nogit)
GBABE_BUILD :=  $(BUILD_DATE)-$(GIT_REV)

CFLAGS  +=  $(INCLUDE) -D__3DS__ $(CORE_DEFS) \
            -DGBABE_BUILD='"$(GBABE_BUILD)"'

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
core-lib:
	@$(MAKE) --no-print-directory -C $(TOPDIR)/third_party/gambatte \
		CXX="$(if $(strip $(CXX)),$(CXX),arm-none-eabi-g++)" \
		AR="$(if $(strip $(AR)),$(AR),arm-none-eabi-ar)" \
		ARCH="$(ARCH)" OPT="$(OPT)"

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

# `clean` leaves the vendored core's objects alone, so relinking the front-end
# does not cost a full rebuild of 30 C++ files. `distclean` removes those too.
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

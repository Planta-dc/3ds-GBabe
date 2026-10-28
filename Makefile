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

TARGET      :=  gbabe
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
# packaging (CIA)
#---------------------------------------------------------------------------------
VERSION_MAJOR   :=  0
VERSION_MINOR   :=  1
VERSION_MICRO   :=  0

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
ARCH    :=  -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

# -O3 -flto is worth ~45% over -O2 on the emulation loop: the core is one giant
# switch inside a header-only library, so link-time inlining of the front-end
# hooks into it is what costs the time. Override with `make OPT=-O2` if a
# toolchain update ever miscompiles something.
OPT     ?=  -O3 -flto -DNDEBUG

CFLAGS  :=  -g -Wall -Wextra $(OPT) -ffunction-sections -fdata-sections \
            -fno-strict-aliasing -fomit-frame-pointer \
            -std=gnu11 $(ARCH)

CFLAGS  +=  $(INCLUDE) -D__3DS__ -DMINIGB_APU_AUDIO_FORMAT_S16SYS

ASFLAGS :=  -g $(ARCH)
# -flto has to be given to the link as well for the objects to be optimised.
LDFLAGS  =  -specs=3dsx.specs -g $(ARCH) $(OPT) -Wl,-Map,$(notdir $*.map)

LIBS    :=  -lctru -lm

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

.PHONY: all clean cia artwork check-tools

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

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

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf $(CIA_TARGET)

else

$(OUTPUT).3dsx : $(OUTPUT).elf $(_3DSXDEPS)
$(OFILES_SOURCES) : $(HFILES)
$(OUTPUT).elf : $(OFILES)

.PRECIOUS : %.t3x %.shbin

-include $(DEPSDIR)/*.d

endif

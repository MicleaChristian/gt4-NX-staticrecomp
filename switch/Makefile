# TASK-174: Atmosphere NRO for GT4Recomp.
# Requires DevKitPro switch-dev. Without a usable toolchain, `make` / `make all` exit 0 with a skip message.
# Use `make require` to fail when the NRO cannot be built.

TOOLCHAIN_OK :=
ifneq ($(strip $(DEVKITPRO)),)
  ifneq ($(wildcard $(DEVKITPRO)/libnx/switch_rules),)
    TOOLCHAIN_OK := 1
  endif
endif

ifeq ($(TOOLCHAIN_OK),)

.PHONY: all clean require
all:
	@echo [switch] skipped: install DevKitPro switch-dev and set DEVKITPRO - see switch/README.md
clean:
	@echo [switch] nothing to clean
require:
	$(error DEVKITPRO/libnx not available (DEVKITPRO='$(DEVKITPRO)'). See switch/README.md)

else

# MSYS getcwd() resolves the D:\gt4sr junction to the real path (spaces), which splits
# unquoted make prerequisites. Prefer the short junction when it points here.
ifneq ($(wildcard /d/gt4sr/switch/Makefile),)
  ifneq ($(realpath $(CURDIR)),$(realpath /d/gt4sr/switch))
  else
    override CURDIR := /d/gt4sr/switch
  endif
endif
# Force the short path when invoked from the junction (realpath equal is the common case).
ifeq ($(findstring Gt4 ercomp,$(CURDIR)),Gt4 ercomp)
  override CURDIR := /d/gt4sr/switch
endif

include $(DEVKITPRO)/libnx/switch_rules

APP_TITLE   := GT4Recomp
APP_AUTHOR  := GT4Recomp
APP_VERSION := 0.1.0
# Fixed 64-bit hex title id (homebrew 05… range) so NSP forwarders can bind a unique app.
APP_TITLEID := 0500475434520001
NACPFLAGS   := --titleid=$(APP_TITLEID)

TARGET   := gt4recomp
BUILD    := build
SOURCES  := source
INCLUDES := include ../native
APP_ICON ?= $(LIBNX)/default_icon.jpg

# Embed control.nacp + icon so hbmenu / SLaunch / NSP forwarders recognize the NRO.
export NROFLAGS := --icon="$(APP_ICON)" --nacp="$(CURDIR)/$(BUILD)/$(TARGET).nacp"

# Quote every -I: this repo path contains spaces ("Gt4 ercomp switch").
INCLUDE := $(foreach dir,$(INCLUDES),-I"$(CURDIR)/$(dir)") \
           -I"$(CURDIR)/$(BUILD)" \
           -I"$(LIBNX)/include"
export INCLUDE

# Resolve EE generated dir to an absolute path and keep it quoted (repo path has spaces).
ifneq ($(strip $(SWITCH_EE_GENERATED)),)
SWITCH_EE_GENERATED_ABS := $(abspath $(SWITCH_EE_GENERATED))
INCLUDE += -I"$(CURDIR)/../private/references/ps2-web-recomp/runtime/include" -I"$(SWITCH_EE_GENERATED_ABS)"
endif

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS   := -g -Wall -O2 -ffunction-sections -ffp-contract=off $(ARCH) $(DEFINES) $(INCLUDE) -D__SWITCH__
# Exceptions required once the donor runtime is linked (SWITCH_EE_GENERATED).
ifeq ($(strip $(SWITCH_EE_GENERATED)),)
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++20
else
CXXFLAGS := $(CFLAGS) -fno-rtti -std=gnu++20
endif
LIBDIRS  := $(LIBNX)/lib
LDFLAGS  = -specs="$(DEVKITPRO)/libnx/switch.specs" -g $(ARCH) -Wl,-Map,"$(BUILD)/$(TARGET).map" $(foreach dir,$(LIBDIRS),-L"$(dir)")
LIBS     := -ldeko3d -lnx

CPPFILES := main.cpp iso_find.cpp runtime_bridge.cpp deko3d_present.cpp audio_nx.cpp pad_nx.cpp settings_nx.cpp
NXCPP    := paths_nx.cpp clock_nx.cpp deko3d_present_nx.cpp

# Full EE+donor link when SWITCH_EE_GENERATED=<dir from prepare-ee-from-iso.sh>
ifneq ($(strip $(SWITCH_EE_GENERATED)),)
CPPFILES += game_main_nx.cpp vu_register_stub.cpp
include runtime.mk
endif

.PHONY: all clean require

all: $(BUILD)/$(TARGET).nro

$(BUILD):
	@mkdir -p "$@"

ifeq ($(strip $(SWITCH_EE_GENERATED)),)
$(BUILD)/$(TARGET).elf: $(BUILD) $(addprefix $(BUILD)/,$(CPPFILES:.cpp=.o)) $(addprefix $(BUILD)/,$(NXCPP:.cpp=.o))
	$(CXX) $(LDFLAGS) -o "$@" $(addprefix $(BUILD)/,$(CPPFILES:.cpp=.o)) $(addprefix $(BUILD)/,$(NXCPP:.cpp=.o)) $(LIBS)
endif

$(BUILD)/%.o: source/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c "$<" -o "$@"

$(BUILD)/paths_nx.o: ../native/platform/nx/paths_nx.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c "$<" -o "$@"

$(BUILD)/clock_nx.o: ../native/platform/nx/clock_nx.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c "$<" -o "$@"

$(BUILD)/deko3d_present_nx.o: ../native/platform/nx/deko3d_present.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c "$<" -o "$@"

$(BUILD)/$(TARGET).nacp: | $(BUILD)
	@nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" "$@" $(NACPFLAGS)
	@echo built $@

$(BUILD)/$(TARGET).nro: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).nacp
	@elf2nro "$<" "$@" $(NROFLAGS)
	@echo built $@
	@$(MAKE) export

.PHONY: export
export:
	@cmd.exe /c "D:\gt4sr\switch\tools\export-switch.cmd"

require: all

clean:
	@rm -rf $(BUILD)

endif

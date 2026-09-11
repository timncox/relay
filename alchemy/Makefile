# =============================================================================
# relay-alchemy — Relay (MIDI to CV) for the Hermetic Modular Alchemy Lab V2
#
# Standard Daisy workflow (libDaisy core Makefile underneath):
#   make                — build firmware -> build/relay_alchemy.bin
#   make program-dfu    — flash over USB (module in DFU mode first: B3 held
#                         at power-on, front USB-C)
#   make clean          — remove the build tree
#
# There is no `program-live`: in its default USB mode this firmware is a
# USB MIDI device, not a HostLink one, so nothing can reboot it into the
# bootloader over the cable. Flash it through the SD picker of whatever
# family firmware is running, or hold B3 at power-on.
#
# Unlike its siblings this repo carries no submodules: the SDK and the
# libDaisy it vendors are taken from ~/tim-os/daisy-sdk (override
# ALCHEMY_DIR / LIBDAISY_DIR to point elsewhere). LIBDAISY_DIR must be the
# SDK's own vendor/libDaisy, not the tree the Patch build uses: they are
# different commits.
#
# Default goal is libDaisy's `all` (a build). Nothing here flashes unless a
# program-* target is named explicitly.
# =============================================================================

TARGET = relay_alchemy

BOARD ?= v2
ifeq ($(filter $(BOARD),v1 v2),)
$(error BOARD must be 'v1' or 'v2' (got '$(BOARD)'))
endif

ALCHEMY_DIR  ?= $(HOME)/tim-os/daisy-sdk/alchemy-sdk
LIBDAISY_DIR ?= $(ALCHEMY_DIR)/vendor/libDaisy

# ── App sources ─────────────────────────────────────────────────────────────
CPP_SOURCES = \
    src/relay_alchemy.cpp \
    src/picker.cpp

C_SOURCES = ../core/relay.c

# ── Alchemy SDK, compiled straight from its tree ────────────────────────────
CPP_SOURCES += $(sort $(shell find $(ALCHEMY_DIR)/framework/src -name '*.cpp'))
CPP_SOURCES += $(sort $(wildcard $(ALCHEMY_DIR)/hardware/alchemy-lab/$(BOARD)/src/*.cpp))

C_INCLUDES += \
    -Isrc \
    -I../core \
    -I$(ALCHEMY_DIR)/framework/include \
    -I$(ALCHEMY_DIR)/hardware/include \
    -I$(ALCHEMY_DIR)/hardware/alchemy-lab/$(BOARD)/include

ifeq ($(BOARD),v2)
C_DEFS += -DALCHEMY_BOARD_V2
endif

RELAY_VERSION  := $(shell cat ../VERSION 2>/dev/null || echo 0.0.0)
RELAY_GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo dev)
C_DEFS += -DRELAY_VERSION=\"$(RELAY_VERSION)\" -DRELAY_GIT_HASH=\"$(RELAY_GIT_HASH)\"

# ── Daisy bootloader build (BOOT_SRAM) ──────────────────────────────────────
# The STM32H750 has 128 KB of internal flash; the SDK alone is over that, so
# the app runs from SRAM under the Daisy bootloader like every Alchemy Lab
# firmware. USE_FATFS=1 compiles libDaisy's FatFS set into the app (the SD
# picker needs it; libdaisy.a lacks the code-page helper).
USE_FATFS = 1
APP_TYPE = BOOT_SRAM
# The SDK's alchemy_stm32h750ib_sram.lds plus the .axi_bss section the SDK's
# SD-card code needs (shared by copy with smack/mark/belt-alchemy).
LDSCRIPT = src/relay_alchemy.lds

CPP_STANDARD = -std=gnu++17
OPT = -O2

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

.DEFAULT_GOAL := all

# newlib-nano prints NOTHING for "%f" unless _printf_float is linked in.
override LDFLAGS += -u _printf_float

BOARD_STAMP := $(BUILD_DIR)/.board-$(BOARD)
ifeq ($(wildcard $(BOARD_STAMP)),)
_BOARD_GUARD := $(shell rm -f $(BUILD_DIR)/*.o $(BUILD_DIR)/*.d $(BUILD_DIR)/*.lst $(BUILD_DIR)/.board-* 2>/dev/null; mkdir -p $(BUILD_DIR); touch $(BOARD_STAMP))
endif

# The commit hash is a -D on every compile line and make does not track
# flags; retire the object that bakes it in whenever HEAD moves.
HASH_STAMP := $(BUILD_DIR)/.hash-$(RELAY_GIT_HASH)
ifeq ($(wildcard $(HASH_STAMP)),)
_HASH_GUARD := $(shell rm -f $(BUILD_DIR)/relay_alchemy.o $(BUILD_DIR)/relay_alchemy.d $(BUILD_DIR)/.hash-* 2>/dev/null; mkdir -p $(BUILD_DIR); touch $(HASH_STAMP))
endif

.PHONY: libdaisy
libdaisy:
	$(MAKE) -C $(LIBDAISY_DIR)

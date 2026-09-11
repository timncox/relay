# Relay on the Daisy Patch -- MIDI (TRS in, or USB on the Seed's micro-USB)
# to CV OUT 1 (pitch, 1 V/oct), CV OUT 2 (assignable) and GATE OUT.
#
#   make -C patch              build relay_patch.bin
#   make -C patch program-dfu  flash over USB (bootloader window)
#
# Requires libDaisy checked out and built at LIBDAISY_DIR (the same tree the
# other Patch ports use). No DaisySP.

TARGET = relay_patch

LIBDAISY_DIR ?= $(HOME)/tim-os/daisy-sdk/libDaisy

# Same reason as the other Patch ports: the module picker writes the chosen
# .bin from the SD card into the Daisy bootloader's QSPI app slot, so every
# app runs under the bootloader.
APP_TYPE ?= BOOT_SRAM
USE_FATFS = 1

CPP_SOURCES = relay_patch.cpp module_picker.cpp
C_SOURCES   = ../core/relay.c
C_INCLUDES += -I../core

RELAY_VERSION  := $(shell cat ../VERSION 2>/dev/null || echo 0.0.0)
RELAY_GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo dev)
C_DEFS += -DRELAY_VERSION=\"$(RELAY_VERSION)\" -DRELAY_GIT_HASH=\"$(RELAY_GIT_HASH)\"

OPT = -O2

# NOTE: no `-u _printf_float`. newlib-nano prints nothing for %f without it;
# the display formats volts as integers (millivolts) on purpose.

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

.DEFAULT_GOAL := all

# The commit hash is a -D on every compile line and make does not track
# flags; retire the object that bakes it in whenever HEAD moves.
HASH_STAMP := $(BUILD_DIR)/.hash-$(RELAY_GIT_HASH)
ifeq ($(wildcard $(HASH_STAMP)),)
_HASH_GUARD := $(shell rm -f $(BUILD_DIR)/relay_patch.o $(BUILD_DIR)/relay_patch.d $(BUILD_DIR)/.hash-* 2>/dev/null; mkdir -p $(BUILD_DIR); touch $(HASH_STAMP))
endif

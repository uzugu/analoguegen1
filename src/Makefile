# analoguegen1 — openfpgaOS + PC SDL2 build rules

ROOT        := $(abspath $(CURDIR)/..)
SDK_DIR     := $(ROOT)/third_party/openfpgaSDK/src/sdk
PEANUT_DIR  := $(ROOT)/third_party/Peanut-GB
APU_DIR     := $(PEANUT_DIR)/examples/sdl2/minigb_apu
BUILD_DIR   := $(ROOT)/build
OBJ_DIR     := $(BUILD_DIR)/obj

USE_SDK_CONTAINER := 0
OF_SDK_IN_CONTAINER := 1

SRCS := \
	main.c \
	core/gb_brain.c \
	core/gb_audio.c \
	core/gb_map.c \
	voxel/voxel_mesher.c \
	render/renderer.c \
	render/renderer3d.c \
	mods/mod_manager.c \
	mods/mod_voxel.c \
	mods/mod_moveinfo.c \
	mods/mod_followers.c \
	mods/mod_wilds.c \
	mods/mod_gen3box.c \
	$(APU_DIR)/minigb_apu.c

CFLAGS += -O3 -I$(CURDIR) -I$(CURDIR)/core -I$(CURDIR)/voxel -I$(CURDIR)/render -I$(CURDIR)/mods -I$(PEANUT_DIR) -I$(APU_DIR)
CFLAGS += -DENABLE_SOUND=1 -DAUDIO_SAMPLE_RATE=24000 -DMINIGB_APU_AUDIO_FORMAT_S16SYS
CFLAGS += -DENABLE_LCD=1 -DPEANUT_GB_12_COLOUR=0 -DPEANUT_GB_HIGH_LCD_ACCURACY=0

.DEFAULT_GOAL := pocket

include $(SDK_DIR)/sdk.mk

.PHONY: pocket pc clean

pocket: $(BUILD_DIR)/app.elf
	@$(SIZE) $< 2>/dev/null || true

pc: app_pc

clean: sdk-clean
	rm -rf $(BUILD_DIR)

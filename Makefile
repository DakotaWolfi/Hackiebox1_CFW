# CC3200 custom firmware
# SPDX-License-Identifier: GPL-3.0-or-later

-include config.local.mk

# Toolchain and TI SDK; defaults are the copies in the hackiebox repository.
ARM_GCC_DIR ?= ../hackiebox_cfw_ng_cfw/arm-sdk/10-2020-q4-major/bin
CC3200_SDK  ?= ../hackiebox_cfw_ng_cfw/ti-sdk/1.5.0/cc3200-sdk
CROSS       := $(ARM_GCC_DIR)/arm-none-eabi-

CFW_VERSION ?= 0.1.0
# 1: hibernate restart and off state by hibernation; 0: MCU reset.
CFW_HIBERNATE ?= 1
# Serial output: 1 normal operation, 2 adds the verbose diagnostics.
CFW_DEBUG ?= 1
# Volume step after a cold start on both paths (8); the
# hardware-test package builds with a lower one.
CFW_START_STEP ?= 8
# Modules (file names without .c) whose debug_verbose() output is on at
# level 1, e.g. CFW_VERBOSE="net cloud".
CFW_VERBOSE ?=
# 1: log frames copied in binary to UART0 between the text lines;
# 0 keeps the developer console plain text.
CFW_LOG_UART ?= 0
# Build number = commit time, so a build can be repeated. A
# build from a tree with uncommitted changes takes the current time instead,
# so it differs from the commit's build (the box refuses an equal one).
DIRTY       := $(shell git status --porcelain --untracked-files=no 2>/dev/null | head -c1)
BUILD_TIME  := $(if $(DIRTY),$(shell date +%s),$(shell git log -1 --format=%ct 2>/dev/null || date +%s))
# Exactly 7 characters: the build block field is 8 bytes with the NUL.
REVISION    := $(shell (git rev-parse HEAD 2>/dev/null || echo 0000000) | cut -c1-7)
BUILD_DATE  := $(shell LC_ALL=C date -u -d @$(BUILD_TIME) '+%d %b %H:%M')

BUILD := build
TARGET := $(BUILD)/cfw

# src/: code shared with the ESP32 build (esp32/); src/cc3200/: this
# platform's drivers, start-up and network processor.
SRC := $(wildcard src/*.c src/cc3200/*.c)
RTOS := third_party/freertos
FATFS := third_party/fatfs
# src/fatfs/ff.c is a patched copy of $(FATFS)/ff.c (FF_TB_PATCHES in
# src/ffconf.h); the original under third_party/ stays unmodified.
FATFS_SRC := src/fatfs/ff.c $(FATFS)/ffunicode.c
OPUS := third_party/opus-1.1
OPUS_CELT := bands celt celt_decoder cwrs entcode entdec entenc kiss_fft laplace mathops mdct modes pitch celt_lpc quant_bands rate vq
OPUS_SRC := $(addprefix $(OPUS)/celt/,$(addsuffix .c,$(OPUS_CELT))) $(OPUS)/src/opus.c $(OPUS)/src/opus_decoder.c third_party/opus_glue/silk_stub.c
# Optimisation per decoder part, to keep the image within the SRAM: the
# per-sample loops (FFT, MDCT, comb filter) -O3; the parts that run once per
# frame or band, or only for packet-loss concealment (pitch, LPC), -Os; the
# rest (band and PVQ decoding, the decoder frame) -O2.
OPUS_HOT := kiss_fft mdct celt
OPUS_SIZE := pitch celt_lpc laplace mathops rate quant_bands opus opus_decoder
# 1 (default): cwrs.c without its pulse-count tables (SMALL_FOOTPRINT), about
# 5 KB less SRAM for an O(N*K) row computation per decoded band.
CFW_OPUS_SMALL ?= 1
OPUS_SMALL := $(if $(filter 1,$(CFW_OPUS_SMALL)),cwrs)
OPUS_DEFS := -DFIXED_POINT -DDISABLE_FLOAT_API -DOPUS_ARM_INLINE_EDSP -DOPUS_BUILD -DVAR_ARRAYS -DHAVE_LRINTF
OPUS_INC := -I$(OPUS)/include -I$(OPUS)/celt -I$(OPUS)/silk -I$(OPUS)/silk/fixed -I$(OPUS)
RTOS_SRC := $(addprefix $(RTOS)/,tasks.c queue.c list.c timers.c) $(RTOS)/portable/ARM_CM3/port.c
SL := third_party/simplelink
SL_SRC := $(wildcard $(SL)/source/*.c) $(SL)/cc_pal.c
SDK_SRC := $(addprefix $(CC3200_SDK)/driverlib/,prcm.c cpu.c utils.c pin.c uart.c timer.c wdt.c interrupt.c gpio.c i2c.c systick.c i2s.c udma.c sdhost.c spi.c adc.c)

OBJ := $(SRC:%.c=$(BUILD)/%.o) $(RTOS_SRC:%.c=$(BUILD)/%.o) $(FATFS_SRC:%.c=$(BUILD)/%.o) $(SL_SRC:%.c=$(BUILD)/%.o) $(OPUS_SRC:%.c=$(BUILD)/%.o) $(patsubst $(CC3200_SDK)/%.c,$(BUILD)/sdk/%.o,$(SDK_SRC))

CFLAGS := -mcpu=cortex-m4 -mthumb -Os -g -std=gnu11 -Wall -Wextra \
  -ffunction-sections -fdata-sections -fno-common -fstack-usage \
  -Dgcc -DTARGET_IS_CC3200 \
  -DCFW_VERSION='"$(CFW_VERSION)"' -DCFW_BUILD_TIME=$(BUILD_TIME)u \
  -DCFW_REVISION='"$(REVISION)"' -DCFW_BUILD_DATE='"$(BUILD_DATE)"' \
  -DCFW_HIBERNATE=$(CFW_HIBERNATE) -DCFW_DEBUG=$(CFW_DEBUG) -DCFW_LOG_UART=$(CFW_LOG_UART) -DCFW_START_STEP=$(CFW_START_STEP) \
  -Isrc/cc3200 -Isrc -I$(SL)/include -I$(SL) -I$(SL)/source -I$(FATFS) -I$(OPUS)/include -I$(RTOS)/include -I$(RTOS)/portable/ARM_CM3 -I$(CC3200_SDK) -I$(CC3200_SDK)/inc -I$(CC3200_SDK)/driverlib
SDK_CFLAGS := $(filter-out -Wall -Wextra,$(CFLAGS)) -w
# Link-time optimisation of the own sources (src/, not the patched FatFs):
# about 3 KB less image. The start-up code and the allocator stubs stay
# outside, as only the libraries refer to them. The link keeps the stack
# usage of the optimised code (build/cfw.elf.ltrans*.su) for stackcheck.
LTO ?= -flto
NO_LTO := startup syscalls
LDFLAGS := -mcpu=cortex-m4 -mthumb -Os -g $(LTO) -fstack-usage -save-temps -nostartfiles -T ld/app.ld -Wl,--gc-sections \
  -Wl,-Map=$(TARGET).map --specs=nano.specs --specs=nosys.specs

all: $(TARGET)-slot.bin

$(TARGET).elf: $(OBJ) ld/app.ld
	$(CROSS)gcc $(LDFLAGS) $(OBJ) -o $@
	@mkdir -p $(BUILD)/lto && mv -f $(notdir $@).* $(notdir $(basename $(lastword $(OBJ)))).res $(BUILD)/lto/ 2>/dev/null || true
	$(CROSS)size $@

# Slot file: the loaded image plus the 64-byte digest text.
$(TARGET)-slot.bin: $(TARGET).elf tools/mkimage.py
	$(CROSS)objcopy -O binary $< $(TARGET).bin
	python3 tools/mkimage.py $(TARGET).bin $@

$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(CFLAGS) $(if $(filter $(NO_LTO),$(notdir $(basename $<))),,$(LTO)) $(if $(filter $(notdir $(basename $<)),$(CFW_VERBOSE)),-DCFW_VERBOSE_MODULE) -MMD -c $< -o $@

$(BUILD)/src/fatfs/%.o: src/fatfs/%.c
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(SDK_CFLAGS) -MMD -c $< -o $@

$(BUILD)/third_party/opus%.o: third_party/opus%.c
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(SDK_CFLAGS) $(if $(filter $(OPUS_HOT),$(notdir $(basename $<))),-O3,$(if $(filter $(OPUS_SIZE),$(notdir $(basename $<))),-Os,-O2)) $(if $(filter $(OPUS_SMALL),$(notdir $(basename $<))),-DSMALL_FOOTPRINT) $(OPUS_DEFS) $(OPUS_INC) -MMD -c $< -o $@

$(BUILD)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(SDK_CFLAGS) -MMD -c $< -o $@

$(BUILD)/sdk/%.o: $(CC3200_SDK)/%.c
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(SDK_CFLAGS) -c $< -o $@

# Worst-case stack of the activities from the .su files and the call graph.
stackcheck: $(TARGET).elf
	python3 tools/stackcheck.py $(TARGET).elf $(BUILD) x,y player.o:task cloud.o:task main.o:op_task net.o:task rtnl.o:task \
	  input.o:task tag.o:tag_task ledui.o:task sound.o:task sl_osi.o:spawn_task main | grep bytes

# Revvox code style (.clang-format) for the own sources.
CLANG_FORMAT ?= clang-format
FORMAT_SRC = $(filter-out src/ffconf.h,$(wildcard src/*.c src/*.h src/cc3200/*.c src/cc3200/*.h esp32/main/*.c esp32/main/*.h)) $(shell find tests/host esp32/tests -name '*.[ch]')
format:
	$(CLANG_FORMAT) -i $(FORMAT_SRC)

format-check:
	$(CLANG_FORMAT) --dry-run -Werror $(FORMAT_SRC)

clean:
	rm -rf $(BUILD)

-include $(OBJ:.o=.d)
.PHONY: all clean stackcheck format format-check

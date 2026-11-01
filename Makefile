# pv-vz90-dal. SPDX-License-Identifier: GPL-3.0-or-later
CROSS	?= arm-none-eabi-
# armv5t (no LDRD/STRD): DAL code is entered by OS code whose sp is not always 8-byte aligned (g4o)
# -ffixed-r9: r9 is the Palm OS globals register; DAL code never uses it, so OS r9 survives every DAL path (g4o)
CFLAGS	= -ffixed-r9 -march=armv5t -marm -mfloat-abi=soft -Os -g -Wall -Wextra -Werror -ffreestanding -nostdlib -fno-builtin
DAL_LOG ?= 1
DAL_SELFTEST ?= 0
# HALDrawFindIndexes behaviour: REFERENCE (default, as the reference VZ90 DAL) or SONY_NX70V (as Sony's NX70V DAL)
DAL_COLOR_MODE ?= REFERENCE
G4_DEFS += -DDAL_LOG=$(DAL_LOG) -DDAL_SELFTEST=$(DAL_SELFTEST) -DDAL_COLOR_MODE=DAL_COLOR_$(DAL_COLOR_MODE)
# the compiler command line of the g4 objects; build/g4/config.stamp is rewritten only when it changes, and every
# g4 object depends on it, so changing DAL_LOG, DAL_SELFTEST, DAL_COLOR_MODE or CFLAGS without "make clean"
# recompiles them (otherwise objects built with the previous option would be linked unchanged)
G4_CONFIG = $(CROSS)gcc $(CFLAGS) $(G4_DEFS)
OBJS	= build/start.o build/main.o build/probe.o build/probe_tables.o

all: build/g1.rom

build/%.o: src/%.S | build
	$(CROSS)gcc $(CFLAGS) -c $< -o $@
build/%.o: src/%.c src/pv_hw.h | build
	$(CROSS)gcc $(CFLAGS) -c $< -o $@
build/g1.elf: $(OBJS) src/link.ld
	$(CROSS)gcc $(CFLAGS) -T src/link.ld $(OBJS) -o $@ -lgcc
build/g1.bin: build/g1.elf
	$(CROSS)objcopy -O binary $< $@
build/g1.rom: build/g1.bin tools/mkimg.py
	python3 tools/mkimg.py $< $@
# local-only probe image: MODULE=<raw module code file the user owns>; never committed
build/probe.rom: build/g1.bin tools/mkimg.py
	python3 tools/mkimg.py $< $@ $(MODULE)
# g3pre: FreeRTOS (third_party/, MIT, unmodified) on PV, separate test image
# port variant: freertos-arm926-port (unmodified, needs -O0) or freertos-arm926-port-pv (vTickISR fix, any -O)
G3_PORT	?= third_party/freertos-arm926-port-pv
G3_PORT_OPT ?= -Os
G3_INC	= -Ig3pre -Ithird_party/freertos-kernel/include -I$(G3_PORT)
G3_TP	= tasks queue list timers heap_4
G3_OBJS	= build/g3/start_rtos.o build/g3/regcheck.o build/g3/main_rtos.o build/g3/pv_bsp.o build/g3/port.o build/g3/portISR.o \
	  $(patsubst %,build/g3/%.o,$(G3_TP))
G3_TPFLAGS = -ffixed-r9 -march=armv5t -marm -mfloat-abi=soft -Os -g -ffreestanding -nostdlib -fno-builtin -w
build/g3/%.o: g3pre/%.S | build/g3
	$(CROSS)gcc $(CFLAGS) -c $< -o $@
build/g3/%.o: g3pre/%.c g3pre/FreeRTOSConfig.h | build/g3
	$(CROSS)gcc $(CFLAGS) $(G3_INC) -c $< -o $@
build/g3/%.o: third_party/freertos-kernel/%.c g3pre/FreeRTOSConfig.h | build/g3
	$(CROSS)gcc $(G3_TPFLAGS) $(G3_INC) -c $< -o $@
# the unmodified port's vTickISR uses inline "BL" without declaring lr clobbered: build it with G3_PORT_OPT=-O0
build/g3/%.o: $(G3_PORT)/%.c g3pre/FreeRTOSConfig.h | build/g3
	$(CROSS)gcc $(G3_TPFLAGS) $(G3_PORT_OPT) $(G3_INC) -c $< -o $@
build/g3pre.elf: $(G3_OBJS) g3pre/link_rtos.ld
	$(CROSS)gcc $(CFLAGS) -T g3pre/link_rtos.ld $(G3_OBJS) -o $@ -lgcc
build/g3pre.bin: build/g3pre.elf
	$(CROSS)objcopy -O binary $< $@
build/g3pre.rom: build/g3pre.bin tools/mkimg.py
	python3 tools/mkimg.py $< $@
# g4pre: FreeRTOS + Boot probe (local-only image when MODULE is given; never committed)
G4_OBJS	= build/g3/start_rtos.o build/g3/pv_bsp.o build/g3/port.o build/g3/portISR.o $(patsubst %,build/g3/%.o,$(G3_TP)) \
	  build/g4/main_g4.o build/g4/kal_task.o build/g4/hal_memory.o build/g4/hal_display.o build/g4/hal_input.o build/g4/hal_time.o build/g4/kal_sync.o build/g4/kal_obj.o build/g4/hal_events.o build/g4/hal_misc.o build/g4/hal_flp.o build/g4/hal_draw.o build/g4/bmp_codec.o build/g4/hal_bootscreen.o build/g4/selftest_draw.o build/g4/ral.o build/g4/sony.o build/g4/sony_asm.o build/g4/probe.o build/g4/probe_tables.o
build/g4/%.o: g4pre/%.c g4pre/kal_task.h g4pre/hal_memory.h g4pre/hal_display.h g4pre/hal_input.h g4pre/hal_time.h g4pre/kal_sync.h g4pre/kal_obj.h g4pre/hal_events.h g4pre/hal_misc.h g4pre/hal_flp.h g4pre/hal_draw.h g4pre/bmp_codec.h g4pre/hal_bootscreen.h g4pre/selftest_draw.h g4pre/ral.h g4pre/sony.h g3pre/FreeRTOSConfig.h build/g4/config.stamp | build/g4
	$(CROSS)gcc $(CFLAGS) $(G4_DEFS) $(G3_INC) -Ig4pre -c $< -o $@
build/g4/%.o: src/%.c src/pv_hw.h build/g4/config.stamp | build/g4
	$(CROSS)gcc $(CFLAGS) $(G4_DEFS) -c $< -o $@
build/g4/%.o: g4pre/%.S build/g4/config.stamp | build/g4
	$(CROSS)gcc $(CFLAGS) -c $< -o $@
build/g4/%.o: src/%.S build/g4/config.stamp | build/g4
	$(CROSS)gcc $(CFLAGS) -c $< -o $@
G4B_OBJS = $(subst build/g4/selftest_draw.o,,$(subst build/g4/hal_bootscreen.o,,$(subst build/g4/bmp_codec.o,,$(subst build/g4/hal_draw.o,build/g4/hal_draw_none.o,$(G4_OBJS)))))
build/g4pre.elf: $(G4B_OBJS) g3pre/link_rtos.ld
	$(CROSS)gcc $(CFLAGS) -T g3pre/link_rtos.ld $(G4B_OBJS) -o $@ -lgcc
build/g4pre.bin: build/g4pre.elf
	$(CROSS)objcopy -O binary $< $@
build/g4pre.rom: build/g4pre.bin tools/mkimg.py
	python3 tools/mkimg.py $< $@ $(MODULE)
# full-ROM DAL: same objects linked to run in place at 0x10000230 (first ROM heap chunk); packed into a ROM image only
# locally, with the user's own ROM tooling (not part of this repository)
build/dal-fullrom.elf: $(G4_OBJS) g4pre/link_fullrom.ld
	$(CROSS)gcc $(CFLAGS) -T g4pre/link_fullrom.ld $(G4_OBJS) -o $@ -lgcc
build/dal-fullrom.bin: build/dal-fullrom.elf
	$(CROSS)objcopy -O binary $< $@
# DAL bundle for an emulator-side ROM loader: link address verified here, hashes and build ID recorded (INTERFACE §38)
build/dal-res/amdd4A38.bin: tools/mkdalres.py
	mkdir -p build/dal-res && python3 tools/mkdalres.py build/dal-res
build/vz90-dal.bundle: build/dal-fullrom.elf build/dal-fullrom.bin build/dal-res/amdd4A38.bin build/g4/config.stamp tools/mkbundle.py
	python3 tools/mkbundle.py build/dal-fullrom.elf build/dal-fullrom.bin build/dal-res/amdd4A38.bin build/dal-res/amdi4A38.bin build/g4/config.stamp $@
build/g4/config.stamp: FORCE | build/g4
	@printf '%s\n' '$(G4_CONFIG)' | cmp -s - $@ || printf '%s\n' '$(G4_CONFIG)' > $@
FORCE:
build/g4:
	mkdir -p build/g4
build/g3:
	mkdir -p build/g3
build:
	mkdir -p build
clean:
	rm -rf build
.PHONY: all clean FORCE

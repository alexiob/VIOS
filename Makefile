AGONDEV ?= $(CURDIR)/.tools/agondev
BUILD_ID ?= bringup-20261016-01
VIOS_INTERNAL_BAUD ?= 1152000
ifeq ($(filter $(VIOS_INTERNAL_BAUD),115200 384000 1152000),)
$(error VIOS_INTERNAL_BAUD must be 115200, 384000 or 1152000)
endif
EZCC := $(AGONDEV)/bin/ez80-none-elf-clang
EZAS := $(AGONDEV)/bin/ez80-none-elf-as
EZLD := $(AGONDEV)/bin/ez80-none-elf-ld
EZOBJCOPY := $(AGONDEV)/bin/ez80-none-elf-objcopy
EZNM := $(AGONDEV)/bin/ez80-none-elf-nm
XTENSA_CC ?= $(HOME)/.platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-gcc
ESP_CFLAGS := -std=c99 -Wall -Wextra -Werror -Os -mtext-section-literals -mlongcalls \
 -nostdlib -fno-builtin -ffunction-sections -fdata-sections -Wl,--gc-sections \
 -T tools/esp32-ram/probe.ld
CFLAGS := -target ez80-none-elf -mllvm -z80-gas-style -mllvm -z80-print-zero-offset \
 -std=c99 -ffreestanding -fno-builtin -fomit-frame-pointer -Oz -Wall -Wextra -Werror -Wa,-march=ez80+full \
 -nostdinc -Iinclude -I. -isystem $(AGONDEV)/include -DMONITOR_BUILD='"$(BUILD_ID)"'
C_SOURCES := kernel/monitor.c kernel/main.c drivers/agon-vdp/keyboard.c boards/agon-light-2/platform.c
ASM_SOURCES := arch/ez80/start.s arch/ez80/uart0.s
OBJECTS := $(patsubst %.c,build/%.o,$(C_SOURCES)) $(patsubst %.s,build/%.o,$(ASM_SOURCES))

.PHONY: all raw esp32-probes test clean
all: build/monitor.bin
esp32-probes: build/zdi-probe.bin build/zdi-loader.bin
build/zdi-probe.elf: tools/esp32-ram/zdi_probe.c tools/esp32-ram/probe.ld
	@mkdir -p build
	$(XTENSA_CC) $(ESP_CFLAGS) $< -lgcc -o $@
build/zdi-loader.elf: tools/esp32-ram/zdi_probe.c tools/esp32-ram/probe.ld
	@mkdir -p build
	$(XTENSA_CC) -DRAM_LOADER $(ESP_CFLAGS) $< -lgcc -o $@
build/zdi-probe.bin: build/zdi-probe.elf
	.venv/bin/esptool --chip esp32 elf2image $< --output $@
build/zdi-loader.bin: build/zdi-loader.elf
	.venv/bin/esptool --chip esp32 elf2image $< --output $@
build/zdi-instruction-probe.elf: tools/esp32-ram/zdi_probe.c tools/esp32-ram/probe.ld
	@mkdir -p build
	$(XTENSA_CC) -DRAM_LOADER -DINSTRUCTION_PROBEONLY $(ESP_CFLAGS) $< -lgcc -o $@
build/zdi-instruction-probe.bin: build/zdi-instruction-probe.elf
	.venv/bin/esptool --chip esp32 elf2image $< --output $@
RAW_C_SOURCES := kernel/monitor.c kernel/main.c boards/agon-light-2/platform_raw.c
RAW_ASM_SOURCES := arch/ez80/start.s arch/ez80/uart0.s arch/ez80/uart0_raw.s
RAW_OBJECTS := $(patsubst %.c,build/raw/%.o,$(RAW_C_SOURCES)) $(patsubst %.s,build/raw/%.o,$(RAW_ASM_SOURCES))
raw: build/monitor-raw.bin
build/raw/%.o: %.c
	@mkdir -p $(dir $@)
	$(EZCC) $(CFLAGS) -MMD -MP -c $< -o $@
build/raw/%.o: %.s
	@mkdir -p $(dir $@)
	$(EZAS) -march=ez80+full --defsym RAM_RESTART=1 $< -o $@
build/monitor-raw.elf: $(RAW_OBJECTS) boards/agon-light-2/ram.ld
	$(EZLD) -T boards/agon-light-2/ram.ld -Map build/monitor-raw.map -o $@ $(RAW_OBJECTS) -L$(AGONDEV)/lib -lagon
	$(EZNM) -u $@
build/monitor-raw.bin: build/monitor-raw.elf
	$(EZOBJCOPY) -O binary $< $@
	.venv/bin/python -c 'from pathlib import Path; from tools.serial_monitor import validate_image; import hashlib; b=Path("$@").read_bytes(); validate_image(b); print(len(b), "bytes", hashlib.sha256(b).hexdigest())'
build/%.o: %.c
	@mkdir -p $(dir $@)
	$(EZCC) $(CFLAGS) -MMD -MP -c $< -o $@
build/%.o: %.s
	@mkdir -p $(dir $@)
	$(EZAS) -march=ez80+full $< -o $@
build/monitor.elf: $(OBJECTS) boards/agon-light-2/ram.ld
	$(EZLD) -T boards/agon-light-2/ram.ld -Map build/monitor.map -o $@ $(OBJECTS) -L$(AGONDEV)/lib -lagon
	$(EZNM) -u $@
build/monitor.bin: build/monitor.elf
	$(EZOBJCOPY) -O binary $< $@
	.venv/bin/python -c 'from pathlib import Path; import hashlib; p=Path("$@"); b=p.read_bytes(); assert b[64:69]==b"MOS\x00\x01"; assert len(b)<0x6F000; print(len(b), "bytes", hashlib.sha256(b).hexdigest())'
test:
	.venv/bin/python -m unittest discover -s tests -v
clean:
	rm -rf build
-include $(OBJECTS:.o=.d)
-include $(RAW_OBJECTS:.o=.d)

VIOS_C_SOURCES := forth/agon_main.c forth/system.c forth/modules.c forth/vm_reference.c forth/vm_memory.c boards/agon-light-2/platform_raw.c bench/forth_bench.c
VIOS_ASM_SOURCES := arch/ez80/start.s arch/ez80/uart0.s arch/ez80/uart0_raw.s arch/ez80/timer.s
VIOS_OBJECTS := $(patsubst %.c,build/vios/%.o,$(VIOS_C_SOURCES)) $(patsubst %.s,build/vios/%.o,$(VIOS_ASM_SOURCES)) build/vios/engine-token.o build/vios/engine-direct.o
.PHONY: vios
vios: build/vios.bin
.PHONY: verify-forth-engine
verify-forth-engine:
	AGONDEV="$(AGONDEV)" PYTHON="$(CURDIR)/.venv/bin/python" tools/verification/forth-engine/check.sh
build/vios/%.o: %.c
	@mkdir -p $(dir $@)
	$(EZCC) $(CFLAGS) -MMD -MP -c $< -o $@
build/vios/%.o: %.s
	@mkdir -p $(dir $@)
	$(EZAS) -march=ez80+full --defsym RAM_RESTART=1 $< -o $@
build/vios/engine-token.o: arch/ez80/forth/engine.s
	@mkdir -p $(dir $@)
	$(EZAS) -march=ez80+full --defsym TOKEN_ENGINE=1 $< -o $@
build/vios/engine-direct.o: arch/ez80/forth/engine.s
	@mkdir -p $(dir $@)
	$(EZAS) -march=ez80+full $< -o $@
build/vios.elf: $(VIOS_OBJECTS) boards/agon-light-2/ram.ld
	$(EZLD) -T boards/agon-light-2/ram.ld -Map build/vios.map -o $@ $(VIOS_OBJECTS) -L$(AGONDEV)/lib -lagon
	$(EZNM) -u $@
build/vios.bin: build/vios.elf
	$(EZOBJCOPY) -O binary $< $@
	.venv/bin/python -c 'from pathlib import Path; from tools.serial_monitor import validate_image; import hashlib; b=Path("$@").read_bytes(); validate_image(b); print("VIOS",len(b),"bytes",hashlib.sha256(b).hexdigest())'
-include $(VIOS_OBJECTS:.o=.d)

STORAGE_C_SOURCES := boards/agon-light-2/storage.c drivers/fatfs/ff.c drivers/fatfs/diskio.c drivers/sd-spi/sd.c
STORAGE_OBJECTS := $(patsubst %.c,build/vios/%.o,$(STORAGE_C_SOURCES)) build/vios/arch/ez80/spi.o
STORAGE_PROBE_OBJECTS := build/vios/kernel/storage_probe.o $(STORAGE_OBJECTS) build/vios/boards/agon-light-2/platform_raw.o build/vios/arch/ez80/start.o build/vios/arch/ez80/timer.o build/vios/arch/ez80/uart0.o build/vios/arch/ez80/uart0_raw.o
.PHONY: storage-probe
storage-probe: build/storage-probe.bin
build/storage-probe.elf: $(STORAGE_PROBE_OBJECTS) boards/agon-light-2/ram.ld
	$(EZLD) -T boards/agon-light-2/ram.ld -Map build/storage-probe.map -o $@ $(STORAGE_PROBE_OBJECTS) -L$(AGONDEV)/lib -lagon
	$(EZNM) -u $@
build/storage-probe.bin: build/storage-probe.elf
	$(EZOBJCOPY) -O binary $< $@
-include $(STORAGE_OBJECTS:.o=.d)

# VIOS_GUI=1 (default): the workbench screen lives in the ESP display server
# (no CPU screen planes, ~22 KB less BSS) and the paired ESP firmware boots into
# the GUI; build both ends with the same VIOS_GUI. 0: the VDU text screen.
VIOS_GUI ?= 1
RESIDENT_SCREEN := $(if $(filter 1,$(VIOS_GUI)),drivers/agon-vdp/text_screen_vds.c,drivers/agon-vdp/text_screen.c)
RESIDENT_C_SOURCES := kernel/workbench_runtime.c kernel/workbench.c $(RESIDENT_SCREEN) kernel/resident_main.c forth/system.c forth/modules.c forth/vm_reference.c forth/vm_memory.c boards/agon-light-2/platform_resident.c drivers/agon-vdp/transport.c kernel/vios_runtime.c kernel/vios_services.c kernel/vios_audio.c kernel/vios_prelude.c kernel/vios_handles.c kernel/vios_ui.c kernel/vios_tools.c kernel/vios_modules.c kernel/vios_boot.c kernel/vios_irq.c bench/forth_bench.c $(STORAGE_C_SOURCES)
RESIDENT_UART_OBJECT := build/resident/arch/ez80/uart0_raw-$(VIOS_INTERNAL_BAUD).o
RESIDENT_OBJECTS := $(patsubst %.c,build/vios/%.o,$(RESIDENT_C_SOURCES)) $(patsubst %.s,build/vios/%.o,$(filter-out arch/ez80/uart0_raw.s,$(VIOS_ASM_SOURCES))) $(RESIDENT_UART_OBJECT) build/vios/arch/ez80/spi.o build/vios/arch/ez80/irq.o build/vios/engine-token.o build/vios/engine-direct.o
.PHONY: FORCE_INTERNAL_BAUD
FORCE_INTERNAL_BAUD:
# macOS's bundled Make compares timestamps at one-second resolution. Force the
# link/output chain when switching rates even if both builds share that second
# or the selected rate's object was already built by a focused test.
ifneq ($(shell cat build/vios-resident.baud 2>/dev/null),$(VIOS_INTERNAL_BAUD))
build/vios-resident.elf build/vios-resident.bin: FORCE_INTERNAL_BAUD
endif
ifneq ($(shell cat build/vios-resident.gui 2>/dev/null),$(VIOS_GUI))
build/vios-resident.elf build/vios-resident.bin: FORCE_INTERNAL_BAUD
endif
# Raw monitor/VIOS/storage-probe initializers retain their historical default.
$(RESIDENT_UART_OBJECT): arch/ez80/uart0_raw.s
	@mkdir -p $(dir $@)
	$(EZAS) -march=ez80+full --defsym VIOS_UART0_BAUD=$(VIOS_INTERNAL_BAUD) arch/ez80/uart0_raw.s -o $@
.PHONY: vios-resident
vios-resident: build/vios-resident.bin build/agon-stage0.bin build/agon-crc.bin build/agon-flash.bin
# eZ80 flash programmer run over ZDI by the ESP (MF); see arch/ez80/agon_flash.s.
build/agon-flash.o: arch/ez80/agon_flash.s
	@mkdir -p build
	$(EZAS) -march=ez80+full $< -o $@
build/agon-flash.bin: build/agon-flash.o
	$(EZOBJCOPY) --only-section .text -O binary $< $@
	.venv/bin/python -c 'from pathlib import Path;import hashlib;b=Path("$@").read_bytes();assert 0<len(b)<=256;print("Agon flash stage",len(b),"bytes",hashlib.sha256(b).hexdigest())'
# Untethered boot stub (/mos/viosboot.bin, a MOS moslet at 0x0B0000).
VIOS_BOOT_BURST ?= 32
VIOS_BOOT_TIMEOUT ?= 12
viosboot: build/viosboot.bin
build/viosboot_crc.inc: tools/crc32_planes.py
	@mkdir -p build
	.venv/bin/python tools/crc32_planes.py $@
build/viosboot.o: arch/ez80/viosboot.s build/viosboot_crc.inc
	$(EZAS) -march=ez80+full -I build --defsym VIOS_BOOT_BURST=$(VIOS_BOOT_BURST) --defsym VIOS_BOOT_TIMEOUT=$(VIOS_BOOT_TIMEOUT) $< -o $@
build/viosboot.elf: build/viosboot.o
	$(EZLD) -Ttext=0x0B0000 -e _viosboot -o $@ $<
	$(EZNM) -u $@
build/viosboot.bin: build/viosboot.elf
	$(EZOBJCOPY) --only-section .text -O binary $< $@
	.venv/bin/python -c 'from pathlib import Path;import hashlib;b=Path("$@").read_bytes();assert 0<len(b)<=0x8000 and b[64:69]==b"MOS\x00\x01";print("VIOS boot stub",len(b),"bytes",hashlib.sha256(b).hexdigest())'
build/agon-stage0.o: arch/ez80/agon_stage0.s
	@mkdir -p build
	$(EZAS) -march=ez80+full $< -o $@
	$(EZNM) -u $@
build/agon-stage0.bin: build/agon-stage0.o
	$(EZOBJCOPY) --only-section .text -O binary $< $@
	.venv/bin/python -c 'from pathlib import Path;import hashlib;b=Path("$@").read_bytes();assert 0<len(b)<=256;print("Agon stage0",len(b),"bytes",hashlib.sha256(b).hexdigest())'
build/agon-crc.o: arch/ez80/agon_crc.s
	@mkdir -p build
	$(EZAS) -march=ez80+full $< -o $@
	$(EZNM) -u $@
build/agon-crc.bin: build/agon-crc.o
	$(EZOBJCOPY) --only-section .text -O binary $< $@
	.venv/bin/python -c 'from pathlib import Path;import hashlib;b=Path("$@").read_bytes();assert 0<len(b)<=256;print("Agon CRC stage",len(b),"bytes",hashlib.sha256(b).hexdigest())'
build/vios-resident.elf: $(RESIDENT_OBJECTS) boards/agon-light-2/ram.ld
	$(EZLD) -T boards/agon-light-2/ram.ld -Map build/vios-resident.map -o $@ $(RESIDENT_OBJECTS) -L$(AGONDEV)/lib -lagon
	$(EZNM) -u $@
build/vios-resident.bin: build/vios-resident.elf
	$(EZOBJCOPY) -O binary build/vios-resident.elf $@
	.venv/bin/python tools/vios_stamp.py $@
	.venv/bin/python -c 'from pathlib import Path;from tools.serial_monitor import validate_image;import hashlib;b=Path("$@").read_bytes();validate_image(b);print("VIOS resident",len(b),"bytes",hashlib.sha256(b).hexdigest())'
	@printf '%s\n' '$(VIOS_INTERNAL_BAUD)' > build/vios-resident.baud
	@printf '%s\n' '$(VIOS_GUI)' > build/vios-resident.gui
-include $(RESIDENT_OBJECTS:.o=.d)

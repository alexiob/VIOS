AGONDEV ?= $(CURDIR)/.tools/agondev
BUILD_ID ?= bringup-20261016-01
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

VIOS_C_SOURCES := forth/agon_main.c forth/system.c forth/vm_reference.c forth/vm_memory.c boards/agon-light-2/platform_raw.c bench/forth_bench.c
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

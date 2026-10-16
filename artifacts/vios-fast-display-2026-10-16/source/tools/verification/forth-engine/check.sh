#!/bin/sh
# Host-only assembly/link/disassembly probes. Does not access serial hardware.
set -eu
TASK_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd -P)
cd "$TASK_ROOT"
TOOL="${AGONDEV:-.tools/agondev}/bin/ez80-none-elf"
SOURCE=tools/verification/forth-engine
OUT=build/verification/forth-engine
mkdir -p "$OUT"
"${TOOL}-as" -march=ez80+full -o "$OUT/direct.o" arch/ez80/forth/engine.s
"${TOOL}-as" -march=ez80+full --defsym TOKEN_ENGINE=1 -o "$OUT/token.o" arch/ez80/forth/engine.s
"${TOOL}-as" -march=ez80+full -o "$OUT/globals.o" "$SOURCE/globals.s"
for kind in token direct; do
 "${TOOL}-ld" -Ttext=0x10000 -Tbss=0x30000 -e "_fvm_${kind}_run" -o "$OUT/$kind.elf" "$OUT/$kind.o" "$OUT/globals.o"
 "${TOOL}-objdump" -dr "$OUT/$kind.o" > "$OUT/$kind.dis"
 "${TOOL}-objdump" -d "$OUT/$kind.elf" > "$OUT/$kind-linked.dis"
 "${TOOL}-objcopy" -O binary -j .text "$OUT/$kind.elf" "$OUT/$kind.bin"
 "${TOOL}-nm" -n "$OUT/$kind.elf" > "$OUT/$kind.sym"
done
"${TOOL}-size" "$OUT/direct.o" "$OUT/token.o"
"${PYTHON:-python3}" "$SOURCE/probe.py" "$OUT"

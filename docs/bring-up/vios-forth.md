# VIOS resident Forth milestone

On 2026-10-16, VIOS compiled and executed Forth on the physical Agon Light 2 using native 24-bit cells and three interchangeable engines. The target defaults to the assembly **DIRECT** engine. Assembly **TOKEN** and the standard-C **C** reference remain in the same image for differential checks. This is an Agon-first implementation: it uses eZ80 native addresses and ADL operation; future CPU ports can provide their own cell/address profiles.

## Build and enter

The existing [RAM bring-up runbook](ram-console.md) describes the two GPIO-to-ZDI jumper links, serial port, installed toolchains and Python dependencies. The Makefile expects AgonDev under `.tools/agondev` and the provisioned PlatformIO Xtensa compiler; `AGONDEV` and `XTENSA_CC` can override them. No toolchain installation happens during Make.

```sh
make vios esp32-probes test
```

This produces `build/vios.bin` plus the ESP32 RAM helpers. `make all` still builds the earlier monitor; use `make vios` for Forth. The [artifact manifest](../../artifacts/vios-2026-10-16/manifest.json) records measured binary sizes and hashes for the preserved milestone artifacts.

For normal headless work, use the [persistent MCP/TCP harness](vios-harness.md). It keeps one USB serial connection open, exposes evaluation and inspection tools, and only touches hardware after an explicit `deploy` call. Example server launch:

```sh
.venv/bin/python tools/vios_harness.py --port /dev/cu.usbserial-22430
```

For a standalone upload, language checks across all three engines, and timer benchmarks:

```sh
.venv/bin/python tools/vios_hardware.py --log docs/bring-up/my-vios-check.jsonl
```

That command establishes a new RAM session. Run only one board-owning process at a time. Deployment identifies the board, checks external RAM mapping, writes bounded CRC-checked blocks, reads every block back, and checks the complete payload digest before execution at `0x040000`. BSS resides below the reserved native stack at `0x0AF000..0x0AFFFF`.

The ESP32 helper and VIOS both execute from RAM. Existing MOS initialization currently supplies the external RAM mapping. The helper temporarily replaces the ESP32 VDP application with a USB-to-UART0 bridge at 115200 baud. A reset or power cycle removes this session and boots installed firmware. Original processor firmware backups are preserved; VIOS deployment has no flash-write operation. See the bring-up runbook for the earlier, separately approved VDP firmware update and backup details.

## Resident development

The prompt accepts colon definitions, numbers, variables/constants, conditionals, `BEGIN` loops, `DO`/`LOOP`, execution tokens and explicit deferred services. Definitions and stacks persist while the RAM session remains alive. For example:

```forth
: SQUARE DUP * ;
7 SQUARE . CR
SEE SQUARE
VARIABLE X
123 X ! X @ . CR
```

`WORDS`, `.S`, `SEE` and `RESOURCES` expose dictionary names, stack values, retained source, word addresses, arena usage and stack high-water depths. `ENGINE C`, `ENGINE TOKEN` and `ENGINE DIRECT` select execution without recompiling definitions. `HEX`, `DECIMAL` and `BASE` control numeric conversion. Definition source is retained as normalized tokens, rather than a byte-for-byte copy of an original source file.

Ordinary compiled calls bind to the word that existed at compile time. Redefining that name affects subsequent lookup and compilation; existing callers keep their original binding. Use `DEFER` and `IS` when existing callers must follow a live service change:

```forth
: ONE 11 ;
: TWO 22 ;
DEFER SERVICE
' ONE IS SERVICE
: CLIENT SERVICE . ;
CLIENT CR
' TWO IS SERVICE
CLIENT CR
```

The existing `CLIENT` prints 11, then 22. `SEE CLIENT` retains its source. Incomplete definitions stay unpublished, and errors roll them back rather than leaving a partially visible word. Runtime errors clear VM stacks; `ABORT` also discards an unfinished definition. The host harness reports pending execution without injecting keyboard bytes. For UART-consuming `KEY` programs, use its explicit `console_input` and `await_result` tools, as described in the harness runbook.

## Execution contract and capacities

The shared contract is `include/forth/vm.h`. On eZ80, a cell is three bytes; arithmetic wraps at 24 bits, signed comparisons use bit 23, and true is `0xFFFFFF`. Memory words operate on trusted native addresses. The host C reference instead uses registered emulated memory regions.

Token code stores one-byte opcodes with three-byte operands. Direct code stores three-byte primitive or colon addresses. A colon entry begins with a native four-byte `CALL DOCOL`; DOCOL moves its continuation to the bounded Forth return stack and enters the thread. Forth recursion therefore does not consume recursive C frames. Both assembly engines cache IP and the data-stack pointer, share checked primitive bodies, and return explicit service traps for console I/O, dynamic execution/deferred services and complex arithmetic. Multiplication, division, remainder and shifts currently use the shared C trap handler.

The compiler emits both arenas even when DIRECT is selected. This retains easy engine comparison at a memory cost:

| Resource | Fixed capacity |
| --- | ---: |
| Dictionary entries, including built-ins | 128 |
| Word name | 31 bytes |
| Token code arena | 16 KiB |
| Direct code arena | 32 KiB |
| Data arena | 8 KiB |
| Retained source arena | 8 KiB |
| Compiler control entries | 64 |
| Data stack / return stack | 256 cells each |
| Nested loop frames | 32 limit/index pairs |
| REPL source line | 255 printable ASCII bytes |

Dictionary lookup uses 32 hash buckets. Checked stacks, compiler resource errors and a dispatch budget provide recoverable limits; native memory access remains a trusted systems feature. Data/return peaks update on every successful push, including transient depths inside a run. The same checks and profiling run in both assembly engines. They are part of the measured profile, which is not an optimized release build.

## Evidence and remaining work

The [verified hardware summary](vios-framed-verified.summary.json) and [serial transcript](vios-framed-verified.jsonl) preserve byte-level deployment/readback and language checks across C, TOKEN and DIRECT. The [benchmark report](../benchmarks/README.md) describes repeated timer measurements and their limits. Final preserved artifact identity belongs to the manifest; earlier evidence files intentionally retain their own tested build identities.

The resident compiler and native engines are implemented and hardware-tested. Graphics/audio through VDP, SD/filesystem persistence, automatic source loading, a complete cold-boot initializer, wider OS services and CPU ports remain future work. RAM definitions/source disappear on reset. The host provides MCP/TCP control; the target has neither a JSON runtime nor an IP stack, and native USB/IP or UDP control is not implemented.

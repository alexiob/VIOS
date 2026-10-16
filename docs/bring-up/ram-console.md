# Verified RAM bring-up

2026-10-16. The physical Agon Light 2 passed CPU execution and USB serial control using a RAM-only ESP32 helper and a freestanding C eZ80 monitor. This is the first bring-up milestone, not an independently bootable OS or a Forth implementation.

## Preserved firmware

| Processor | Original backup | Size | SHA-256 |
| --- | --- | --- | --- |
| ESP32 | `backups/esp32/original-2026-10-16.bin` | 4,194,304 bytes | `91693875ae044e50eb10d02dd41c6efc528609bdd9b012aab4576b929c535e1c` |
| eZ80 | `backups/ez80/original-2026-10-16.bin` | 131,072 bytes | `e396bfff189dfcd6c064f81349f4af9c80f4033c9015eb6ea12f870679eb6569` |

Paths here are relative to the workspace root. The ESP32 backup matches a full-image target MD5. The eZ80 backup consists of two independent complete reads, stored as `.bin` and `.verify.bin`; they match byte for byte, and each 256-byte serial response passed its CRC32 check. The CPU register snapshots match before and after those memory reads. Readout metadata and raw serial transcripts accompany the images. The exact read-only ESP32 helper used for the eZ80 backup is preserved as `backups/ez80/zdi-read-probe.bin` and `.c`.

The eZ80 identifies as product `0007`, revision `AA`. Its flash contains Agon Quark MOS, but does not match the downloaded vendor MOS 1.03, 1.04-rc1 or 1.04 binary prefixes. The exact version remains unconfirmed. Do not infer corruption from that mismatch. The original ESP32 application contains manufacturing GPIO diagnostics. Its approved replacement with Olimex VDP 1.04 was the only processor flash change made; the subsequent probe and loader execute from RAM.

## Physical connections

Disconnect USB and any battery before changing wires. Keep UART_DIS open. Both debug endpoints are on the same board, so no extra ground or power link is needed.

| GPIO header | Signal | ZDI header |
| --- | --- | --- |
| Pin 9 | ESP32 GPIO26 / debug clock | Pin 4 |
| Pin 8 | ESP32 GPIO27 / debug data | Pin 6 |

With four female-to-male wires and a breadboard, join GPIO9 and ZDI4 using A1/B1, and GPIO8 and ZDI6 using A3/B3. Each numbered A–E strip joins **one lead from each header**. Never join both GPIO leads to each other or both ZDI leads to each other. Use the connector diagrams in `docs/research/sources/agon-recovery/media/` to establish pin-1 orientation. ZDI pin 5 is ground.

## Build and deploy

Host dependencies currently provisioned: Python virtual environment with `tools/requirements.txt`; Agondev v0.22 at `.tools/agondev`; Xtensa GCC 11.2.0 at the PlatformIO package path used by `XTENSA_CC` in the Makefile. Override `AGONDEV` or `XTENSA_CC` for another host. Toolchain installation is not automated by Make.

```sh
make raw esp32-probes test
.venv/bin/python tools/zdi_monitor.py
```

The deployment tool checks the connected ESP32 MAC against the identified board, resets into its ROM serial loader and loads `build/zdi-loader.bin` into ESP32 RAM. It keeps that serial connection open: closing/reopening the port can pulse modem lines and reset this board. The helper identifies and halts the CPU, disables interrupts for the RAM run, and checks the existing CS0 external RAM mapping. This bootstrap currently relies on the installed MOS having initialized that mapping; it does not configure the whole board from reset.

The host validates the MOS-format ADL image header and payload bounds. The target rejects RAM writes outside `040000..0AEFFF`, caps each block at 256 bytes, and checks its CRC before writing. Every written block is independently read back; the host compares all bytes and the complete image SHA-256 before execution at `040000`. The stack occupies `0AF000..0AFFFF`; startup clears BSS and owns the C runtime. No MOS calls occur in the running monitor.

The ESP32 then bridges the onboard USB serial connection to eZ80 UART0 at 115200 baud, 8N1. This build uses raw characters and configures its own UART; it does not use VDP keyboard packets. The portable monitor core is shared with the separate VDP-based build (`make all`). `make all` produces `build/monitor.bin`, whose MOS/VDP loading route has not been demonstrated. The working raw build is `build/monitor-raw.bin`.

The deployment command runs the real-hardware assertions and records results. All three Ctrl-] bytes in sequence are reserved by the temporary bridge to halt the CPU and return to the helper's debug command loop. A physical reset/power cycle removes both RAM programs and boots the installed firmware again. Port closure may also reset the board; do not assume a RAM session survives reconnecting a terminal. Re-running the deployment command re-establishes it.

## Hardware evidence

`docs/bring-up/monitor-hardware-green.jsonl` preserves timestamped transmitted/received bytes, including the payload transfer and command results. Its `.summary.json` records the matching RAM readback digest, CPU identity and memory mapping, boot banner, line editing and restart proof. The tested raw image is 1,249 bytes with SHA-256 `53cdb53e8f7c0461a0f9c0a302d525c86a7dc68f5a22b067e45a532c055487a6`.

Observed console responses include:

```text
agon-monitor bringup-20261016-01
agon> ping
pong
agon> info
Agon Light 2 / eZ80 ADL / raw UART0 115200 / freestanding C
build: bringup-20261016-01
agon> echo hardware-control-ok
hardware-control-ok
```

Hardware assertions cover help, ping, identity, echo (including text resembling the prompt), overlong-line discard, backspace, CRLF, and `reboot` followed by another successful ping. In the raw build, `reboot` waits for the UART transmitter to empty and restarts the RAM image. It is not a board power reset or return to MOS. The initial missing-newline failure and the corrected run are both retained. Host behavior checks also pass under C99 warnings-as-errors and ASan/UBSan.

## Repeat a flash backup

```sh
make esp32-probes
.venv/bin/python tools/zdi_backup.py --output backups/ez80/new-read.bin
```

Choose a fresh output name; existing data files are refused. The tool resets the board and uses the read-only RAM probe, checks CPU identity, halts it, confirms flash mapping, reads the complete flash twice and compares both images. It leaves the CPU halted. Re-run the monitor deployment or reset the board afterward. Do not use this command with the RAM-loader image in place of the default read-only probe.

## Next architecture work

The Forth investigation is in `docs/research/forth-os-proposal.md`: a portable C token VM as the initial reference engine, resident interpreter/compiler, source-first persistence, and explicit cell/address profiles for eZ80 and future 6502 systems. That design has not yet been implemented. A cold-boot initializer, SD driver, and OS services will need separate hardware verification beyond this console milestone.

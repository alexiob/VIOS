# Proposed first hardware milestone

Status: proposal for user review, before implementation. Date: 2026-10-16.

## Outcome

Compile a small C program for the eZ80, deploy it from this Mac to the physical Agon Light 2, run it in RAM, and issue commands through `/dev/cu.usbserial-22430`, with a saved transcript proving responses. The microSD is installed; there is no keyboard or display. The captured ESP32 application currently emits GPIO diagnostics, so normal VDP/MOS loading remains unverified.

## Recommended approach

Use MOS as a temporary bootstrap/loader and retain the ESP32 as the board's I/O coprocessor. The RAM payload will provide its own startup, stack, BSS initialization and direct polled eZ80 UART0 driver; it will not call MOS services while running. This proves bare-metal execution on the eZ80 after a firmware bootstrap. It is not yet an independent cold-boot OS.

This limits the first milestone to loading, CPU execution and a working console. Scheduling, filesystems, namespaces, networking and replacing eZ80 flash follow only once hardware access and recovery are reliable.

## Steps

1. **Identify and preserve the existing installation.** Keep the boot log. Read and save a full ESP32 flash backup and its chip/flash metadata through the ROM loader, then inspect the installed image. Establish whether its GPIO diagnostics are a test mode or an unrelated application. Confirm eZ80/MOS status using the available route. Review a concrete replacement image and recovery procedure with the user before any required flash replacement; do not erase unknown firmware to investigate it.
2. **Establish headless loading.** With working MOS/VDP, inspect/backup the installed SD and boot script, enable serial console at boot, and make `hexload vdp` available. Existing card contents determine whether this needs a one-time move to a host card reader. If normal firmware cannot be reached, present the exact recovery prerequisite: compatible firmware restore and, if eZ80 recovery/debug is needed, two jumper wires to ZDI. Do not assume the current USB connection is an eZ80 memory loader.
3. **Build the minimal target.** Pin the macOS arm64 Agondev toolchain. Compile portable C with a small ADL assembly entry and I/O wrappers, plus a dedicated linker script. Keep the payload and stack below `0x0B0000`. Start with interrupts disabled, fixed buffers and no allocator; inspect the map/disassembly for unintended MOS/library dependencies.
4. **Deploy and control it.** Use one host serial session for receiver readiness, bounded RAM upload and execution. Validate addresses and checksums, retain the negotiated CPU/VDP UART configuration, and parse VDP key-event frames into characters. Keep upload and console operations in a repeatable host command.
5. **Demonstrate and record.** Print a unique build banner; run `help`, `ping`, `info`, and `echo <text>` with exact expected replies. Test blank/unknown input, backspace, CR/LF and an overlong line without memory corruption. Provide a controlled `reboot` path back to the firmware after verifying its entry assumptions, then reload and repeat. Preserve the binary hash, memory map and raw serial transcript. If recovery needs a physical reset, document that honestly.

## Proposed implementation boundaries

| Location | Responsibility |
| --- | --- |
| `kernel/monitor.c` | Portable fixed-buffer command loop, banner and commands |
| `include/platform/console.h` | Minimal console input/output contract |
| `include/platform/system.h` | Platform identity and restart contract |
| `arch/ez80/` | ADL startup, I/O instructions, CPU interrupt state, ABI adapters |
| `boards/agon-light-2/` | Board bindings, UART/VDP setup, payload linker map, loader entry |
| `drivers/agon-vdp/` | Framed keyboard events and text/console protocol |
| `lib/` | Only portable freestanding helpers actually needed |
| `tools/` | Host serial ownership, upload, run and transcript capture |
| `tests/` | Meaningful host checks of command parsing, VDP framing and uploader address bounds |

No assembly, register constants or MOS headers in the portable monitor. Future architecture/board ports implement the same small contracts. Driver reuse must not turn the core into a collection of board preprocessor branches.

## Alternatives considered

- **MOS application calling firmware APIs:** fastest initial hello-world and easy return, but does not demonstrate an OS-owned runtime. Useful only as a diagnostic if loader/ABI issues need isolation.
- **ESP32/ZDI RAM loader:** can control the eZ80 without an SD or MOS, but needs two physical jumper wires and compatible ESP32 firmware. Prefer it if normal firmware/bootstrap is unavailable or later independent kernel debugging requires it.
- **Immediate replacement of eZ80 flash:** closer to final cold boot, but adds flash/recovery work before proving the UART and C runtime. Defer until RAM bring-up passes.

## Pass criteria and limits

Success requires actual eZ80 execution, a unique banner, deterministic responses over USB, a safe bounded memory layout and a repeatable load/run/recovery cycle. Host compilation, ESP32 diagnostics, serial transmission alone, or emulator output do not count as a pass.

No target code has been written or deployed at this proposal stage. Firmware identity, access to the installed SD and the headless bootstrap are the outstanding prerequisites. OS semantics beyond this monitor remain design work for later milestones.

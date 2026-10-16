# VIOS resident system

The integrated physical-board acceptance passed: 137 checkpoints cover the transport, three engines, VDP replies, native SD module persistence, live replacement and true A/B source restarts. See [the acceptance report](vios-resident-accepted-run3.json) and [the progress ledger](vios-resident-progress.md). Recovery fault tests and host/native checks have separate reports below.

## Build and attach

```sh
make test verify-forth-engine
make vios-resident VIOS_INTERNAL_BAUD=1152000
VIOS_INTERNAL_BAUD=1152000 ~/.platformio/penv/bin/pio run -d firmware/esp32-vios -e esp32-vios
.venv/bin/python tools/vios_harness.py --backend resident --tcp 127.0.0.1:8765
```

Use the MCP `connect` tool to attach to installed resident firmware. `connect` uploads nothing. `reboot` restarts the supervisor and reconnects through the same serial handle. Explicit `reboot(force=True)` (MCP `{"force":true}`, CLI `:recover`) abandons pending or uncertain execution and loses unsaved RAM. `deploy` waits for bootstrap, then explicitly replaces CPU RAM with the selected image and verifies its readback; it is a recovery/development operation. `--backend ram` retains the earlier temporary RAM bridge.

Only one process owns `/dev/cu.usbserial-22430`. A harness process keeps it open across clients and serializes operations. Opening the driver can reset the board electrically; initial attach permits 120 seconds. A newly attached session must be at an idle interpreter; use explicit force recovery for an unknown program already consuming `KEY` input.

## Bootstrap and transport

The ESP32 application contains the pinned VDP, a boot supervisor, the exact VIOS CPU image and a 46-byte Agon-specific native bootstrap. It discovers enabled on-chip SRAM, verifies the bootstrap there, runs it to configure external SRAM and checks native register readbacks plus a completion marker. Only then does it write and verify the main image at `040000`. The kernel uses no MOS calls. Original MOS remains in eZ80 flash.

The bootstrap uses ordinary eZ80 instructions for hardware configuration. Instruction-store experiments produced unexpected register/operand behavior on this board; those experiments are preserved in the diagnostic logs, and their LD/OUT injection path is excluded from production startup. Runtime debug mapping reports the validated native bootstrap result.

USB uses 115200 baud, 8N1. The current CPU/VDP pair defaults to 1152000; matched 384000 and 115200 builds are supported. Build both ends sequentially at the same rate. COBS/CRC frames separate console, debug and status traffic. Source requests are assembled and CRC-checked in full before forwarding to the CPU; the CPU independently checks source CRC before execution. Resident source lines are limited to **235 ASCII bytes**. Missing acknowledgements are ambiguous and are never retried automatically. After output corruption the host refuses to report a result as valid. See the [fast-display qualification and USB pacing investigation](../benchmarks/fast-display-2026-10-16.md).

Both UART2 flow-control directions are enabled. Native eZ80 code pauses RTS and drains up to 32 FIFO bytes before C parsing; input opens only at explicit receive windows. This fixes the measured continuous-burst overrun without host pacing. Fifty full-length source lines passed on hardware; `VDP-INFO` reported zero dropped/overrun observations afterward. The compiler regression for binary VDP output executes actual linked target code.

VDP drawing, text, keyboard, audio and reply packets retain their native command path. Console output is explicit and is not automatically copied to VGA. Legacy terminal mode and blocking SHIFT paging are disabled in this profile. PS/2 keyboard remains available; mouse initialization is disabled because GPIO26/27 are reserved for the permanent ZDI links. See [the firmware protocol](../../firmware/esp32-vios/README.md).

The 48 host tools include 29 core tools for evaluation, source/module inspection, resources, stacks, benchmarks, bounded debug access, persistence and boot selection, plus 19 workbench tools. MCP/JSON/TCP execute on the Mac; there is no JSON parser or MCP server in the eZ80 VM. Loopback TCP uses the documented NDJSON transport, not MCP Streamable HTTP. CH340 is a serial USB device; this profile does not implement native USB Ethernet, a board TCP/IP stack, Wi-Fi or UDP mutation commands.

## Malleable services and source modules

Ordinary Forth execution uses native direct threading by default, with token threading and the C reference retained. Explicit device words pay a native service trap; ordinary VM opcodes do not perform device abstraction dispatch. `CONSOLE-EMIT`, `CONSOLE-KEY`, `CLOCK-MS`, `FS-READ`, `FS-WRITE` and `DISPLAY-WRITE` are deferred bindings that Forth modules can replace.

Applications must call those deferred names to observe rebinding. Replacing `CONSOLE-EMIT` does not retarget the primitive `EMIT` or the kernel's recovery console; replacing `FS-READ` does not redirect the package loader's native storage operations. These explicit boundaries retain a dependable recovery path and keep ordinary primitive execution efficient.

The host workflow is `module_stage` → `module_activate` → `module_save`. Staging and activation compile/test through the C reference with a managed-data overlay. Failed compilation, dependency checks or tests leave the active dictionary and data intact. Irreversible device operations are disallowed in transactions. Tests must leave exactly one true value. Dependencies pin module names and source CRCs.

`module_source` returns exact source, CRC32 and a host-computed SHA256. Packages contain checked metadata, dependencies, source and format/cell-width markers. Saves use a verified temporary file and rename into an immutable content path beneath `/VIOS`. Existing different content is never silently replaced. `load` supports bounded nested source includes; `module_load` validates and activates a package whose dependencies are already active.

Current limits are eight module names, two retained generations per name, one pending stage, four dependencies per module, 4096 source bytes per module and a 32 KiB module-source pool. Rollback reclaims only the latest activation checkpoint and refuses intervening dictionary growth. This is source persistence, not a saved heap, suspended computation, general garbage collector or arbitrary unload facility.

## A/B source startup

`boot_trial` saves the active dependency-ordered module set into the inactive source-bundle slot. Reboot consumes the pending attempt durably before loading it. Module tests, a VM arithmetic check, a VDP response and storage synchronization form the current boot health gate; successful health confirms the trial. An unconfirmed attempt falls back on the next boot. `boot_status`, `boot_confirm` and `boot_revert` expose the selection explicitly; revert takes effect at the next boot.

Two CRC-checked generation records journal confirmed/previous/candidate state. The old fallback identity is invalidated before reusing its storage. Failed confirmation writes restore the committed in-memory selection. Package files remain immutable and can be shared by source slots.

Both slots share one FAT32 filesystem and SD card. This handles rejected updates; it does not provide media-failure isolation or make FAT power-fail atomic. This profile does **not** establish automatic ESP32 firmware A/B rollback. Its app0 update preserves the existing bootloader, partition table, OTA selection and app1.

## Recovery and evidence

An independent ESP32 task owns USB/control/ZDI. It starts before graphics initialization with restricted `B`/`Z` control, then receives full control ownership after bootstrap. The VDP task alone writes CPU reply/input packets. Eight bounded console records and whole-frame USB TX locks prevent parser/control byte interleaving. Source input is rejected before CPU readiness. A buffered graphics loop can block normal Forth output, but the separate control task can inspect, halt and explicitly restart the system.

The supervisor's 30-second boot deadline restarts the whole ESP32 after each of the first two failed attempts, clearing a stuck parser before loading the embedded CPU image. The third failed attempt leaves the CPU halted with USB debug available. A validated RTC no-init counter survives software resets; a healthy ready message or intentional `Z` clears it. An intentionally paused CPU suspends the deadline. A halted CPU cannot execute a Forth CLI; supervisor debug is the recovery route in that state. Healthy running programs have no runtime watchdog here. The planned hybrid workbench must retain the ordinary CLI independently of its panes.

The complete pre-resident ESP32 backup is `backups/esp32/pre-vios-resident-2026-10-16.bin`. The extracted app0 restore image is `backups/esp32/pre-vios-resident-app0-2026-10-16.bin`, 0x140000 bytes, SHA256 `4b3bce4e667b2023289a0347fd27b9fd4bad61be8d746c1707a536a13cbc9376`. With the harness stopped, the application-only restore command is:

```sh
.venv/bin/esptool --chip esp32 --port /dev/cu.usbserial-22430 --baud 115200 \
  --after hard-reset write-flash 0x10000 \
  backups/esp32/pre-vios-resident-app0-2026-10-16.bin
```

The explicit acceptance driver attaches without loading a ROM helper, records every checkpoint, verifies 50 maximum-sized source requests, three engines, debug readback, VDP replies, module replacement and three true supervisor restarts:

```sh
.venv/bin/python tools/vios_resident_acceptance.py \
  --phase core --phase modules --phase boot \
  --log docs/bring-up/resident-acceptance-run1.json
```

Boot acceptance requires a clean, unconfirmed SD selection and refuses to replace an existing confirmed user bundle. Protocol replies prove screen readback/audio acknowledgement, not physical legibility or audible sound without an attached display/listener. See [the selected hybrid interface design](../research/vios-interaction-environment.md) for subsequent UI work.

The accepted SD startup is slot A, with demonstration modules `LIVE1I`, `LIVE1P` and `LIVE1D`. `LIVE1-C` returns 11 and `LIVE1-D` returns 12. They are persisted examples of a deferred service and its provider/consumer, not required kernel internals. The test also confirmed v2 values 22/23 in slot B before reverting to A.

[Recovery fault acceptance](vios-resident-recovery-run1.json) passed 25 checkpoints. It deliberately blocked `KEY`, then executed an infinite buffered VDP jump. During the graphics loop, USB boot/identity queries, CPU halt/register inspection and RAM header readback remained usable; force `Z` restored Forth. A separate boot-deadline test observed an automatic full ESP restart after 30.1 seconds, a retained failure count of one, and successful reattachment without sending `Z` or a CPU image. Every case preserved confirmed SD slot A. The three-failure terminal limit is covered by native fixture tests; three consecutive failures were not induced on hardware.

[Whole-board power-cycle acceptance](vios-resident-power-cycle.json) also passed. The listener observed USB removal/reappearance after the user's power disconnect, attached without image upload in 35.969 seconds, and verified slot A, persisted values 11/12, DIRECT resources and mode 1 with zero receive drops. This tests physical reset separately from the earlier ESP software-restart cases.

Measured footprint: CPU payload 72,292 bytes, BSS 190,082 bytes and a separate 4 KiB stack; the conservative linker region leaves 192,282 bytes before the stack. ESP32 app0 image is 630,544 bytes, with 37,948 bytes of static RAM (runtime heaps, task stacks, framebuffer and DMA allocations are additional). The host MCP/JSON implementation has no target parser footprint. See the [resident benchmark](../benchmarks/resident-2026-10-16.md) for timing and the final artifact manifest for hashes.

The [accepted artifact manifest](../../artifacts/vios-resident-2026-10-16/manifest.json) identifies the exact binaries and source snapshot. Its `sd-startup/VIOS` directory contains CRC-verified host copies of the five demonstration packages, two bundles and two journals; [backup evidence](vios-resident-sd-startup-backup.json) records their hashes. The [live session record](vios-resident-live-session.json) records the running TCP harness. Use the [shared MCP stdio configuration](vios-harness.md#start-and-attach) when the CLI and an LLM should share that one owner.

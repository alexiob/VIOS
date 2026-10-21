# VIOS development handoff for Claude

Prepared **2026-10-18**, Europe/Rome. This document transfers the project intent,
implementation checkpoint, hardware practices and recommended continuation work.
Read it before changing code or opening the serial device. Existing user
instructions take precedence; the roadmap below is a recommendation, not a claim
that its unimplemented designs have already been approved.

## Update (2026-10-21)

The complete, code-verified reference is now the
[VIOS manual](docs/manual/VIOS-MANUAL.md); where this handoff disagrees with it,
the manual and the code win. Main changes since this handoff was written:

- **Untethered boot.** MOS 3.0.2 was written to the eZ80 flash on 2026-10-19
  (owner-approved; see `docs/superpowers/specs/2026-10-19-vios-untethered-boot.md`
  §16.7). MOS runs the SD stub `viosboot`, which loads the resident embedded in
  the ESP firmware (or `/vios/vios.bin`). The ZDI wire is optional (dev boots,
  debug tools). Normal installs never write the eZ80 flash.
- **ESP firmware A/B.** Install with `tools/vios_firmware_ab.py` (backs up,
  writes the inactive slot on trial, the firmware confirms itself or falls back).
  Raw `esptool write-flash 0x10000` is no longer the procedure.
- **Display.** The board boots into the GUI desktop (mode 0, 640×480,
  16 colours) with a Console window; F1 toggles console/editor; Ctrl+Alt+F1
  switches GUI ↔ full-screen text console. Lines are edited on the ESP.
- **Tasks** are preempted by a 1 kHz timer tick. **Harness**: 72 MCP tools.
- **Serial port**: currently `/dev/cu.usbserial-210`; the suffix changes, so
  check `ls /dev/cu.usbserial-*` and pass `--port` (tool defaults still name
  `-224130`).
- **`:recover` / `reboot(force=true)`** on an untethered board restarts only
  the ESP; the resident does not re-send health and the harness cannot
  reattach until a board reset, power cycle or harness restart.
- **Sizes**: the main-51 resident is about 186 KB with BSS ending at
  `0xAAB15`; check `build/vios-resident.map` rather than numbers here.

## 1. Start here

- Repository: <https://github.com/alexiob/VIOS>, private; branch `main`.
- Baseline commit: `439c5e02f204c518cbe932d4be35164d8e39f095`, initial VIOS
  implementation, research, backups and qualification evidence. This handoff is
  a later documentation addition; inspect current Git status before working.
- Local checkout: `/Users/alex/Documents/Projects/iob.dev/repos/agon-light-2`.
- Last accepted firmware/hardware measurements: **2026-10-16**, not a fresh
  observation of the board today.
- A read-only host check on **2026-10-18** found the usual
  `/dev/cu.usbserial-22430` absent and no listener on TCP 8765. No board was
  opened, reset or interrogated while preparing this handoff. Recheck availability;
  ask the user to reconnect hardware if necessary. Old process IDs are obsolete.

Read these in order:

1. [README](README.md).
2. [Latest fast-display implementation, measurements and limits](docs/benchmarks/fast-display-2026-10-16.md).
3. [Automated acceptance with the explicit manual-keyboard limit](docs/bring-up/vios-fast-display-acceptance.json).
4. [Resident system](docs/bring-up/vios-resident-system.md),
   [harness](docs/bring-up/vios-harness.md) and
   [workbench](docs/bring-up/vios-workbench.md) runbooks.
5. [Hardware reference](docs/research/hardware.md),
   [Forth specification](docs/superpowers/specs/2026-10-16-vios-forth.md), and
   [resident specification](docs/superpowers/specs/2026-10-16-vios-resident-system.md).
6. [Interaction design](docs/research/vios-interaction-environment.md) and
   [original Forth research](docs/research/forth-os-proposal.md), treating proposed
   features and old observations as historical context.

Source and the newest measured evidence resolve conflicts in older prose. For
example, early research recommends a C token VM first and describes an absent
keyboard; the implemented default is native DIRECT, and the user subsequently
attached VGA and a keyboard. Some older workbench memory totals also predate the
latest renderer. Do not repeat the initial bring-up as if it were unfinished.

## 2. User intent and settled decisions

**VIOS** is the OS and project name.

The intended result is a small, malleable, self-contained operating/development
environment inspired by Unix/Plan 9 and Smalltalk/Genera, built around resident
Forth. It must serve both humans and LLM/harness clients, including headless use.

The user selected:

- **Agon Light 2 first.** Exploit its native eZ80 instructions, memory, graphics
  coprocessor, storage and input. Optimize on the board, not by desktop intuition.
- A native Forth implementation with the **standard-C reference retained for
  testing**, plus small assembly/hardware adapters. DIRECT won the measured
  initial comparison; TOKEN remains available.
- Hardware/architecture boundaries that allow a later MOS 6502 or other port
  **without imposing generic dispatch or lowest-common-denominator restrictions
  on ordinary Agon execution**. No 6502 port exists yet.
- A **hybrid workbench: text first, graphics where useful**, with integrated
  browsing, editing and inspection. Preserve the plain CLI and headless controls.
- Inspectability and controllability as core OS capabilities, not an afterthought.
  Keep expensive MCP/JSON/LLM integration on the host where appropriate.
- Reliable recovery, firmware backups and benchmark evidence alongside changes.

Present a concise implementation plan before substantial new coding. Proceed
autonomously within the agreed scope; ask when physical action or a material
unresolved design choice is needed. Avoid repeated approval requests for routine
reversible work. Earlier authorization covered serial takeover and backed-up,
application-only ESP updates, not arbitrary eZ80 flashing, repartitioning another
card, publishing services, or rewiring pins without discussion.

## 3. What is actually implemented

| Area | Current implementation |
| --- | --- |
| Bare-metal startup | Untethered: MOS 3.0.2 runs the SD stub `viosboot`, which loads the ESP-embedded image at `0x040000`. Dev boot (ZDI wire): the ESP32 supervisor uses a verified 46-byte native stage0 to establish CPU RAM and load the image. The runtime makes no MOS calls. |
| Forth | Resident interpreter/compiler, native 24-bit DIRECT and TOKEN engines, C reference, bounded stacks, definition/source inspection, deferred bindings and resource counters. |
| Live modules | Stage/test/activate checked source with managed-data transaction overlay, pinned dependency CRCs, two retained generations, constrained rollback and exact source retrieval. |
| Storage | Native SD SPI/block path, FatFs/FAT32, checked module packages, source loading, verified saves and source-based A/B startup. |
| Host integration | One resident USB owner; human CLI and 72 MCP tools share serialized access over loopback NDJSON or MCP stdio. Binary COBS/CRC frames multiplex console/control/events over serial. |
| Recovery | Independent ESP USB/ZDI task; CPU register/memory inspection; explicit forced reboot during pending input or a stuck VDP; boot watchdog and bounded failed-boot handling. |
| Workbench | Module/word browser, exact-source draft editor, live inspector, guarded editing/staging/activation/save, one undo snapshot, semantic keyboard events and headless equivalents. |
| Rendering | Retained panes/source index, sparse glyph/style changes, native UART writer, compressed commands, ESP buffered viewport scrolls, full-width source mode and framebuffer-fenced diagnostics. |
| Console | GUI desktop at boot with a Console window (F1 toggles editor, Ctrl+Alt+F1 GUI ↔ full-screen text console), hidden harness envelopes, line editor with history on the ESP, physical keys isolated from host frames. |
| Tasks (Oct 18) | Four background Forth tasks swapped at trap boundaries, preempted by a 1 kHz timer tick (4 ms slices) and by budget exhaustion (probe-verified exact resume), `PAUSE`/`MS`, per-task output rings or console attachment, UART-safe admission gate, measured 1024-instruction slice, `AUTORUN` boot task. |
| Handles (Oct 18) | Non-blocking `OPEN/CLOSE/READ/WRITE/PIPE`, Forth `READ-WAIT/WRITE-ALL`, SD streams, `/dev/null|cons|tasks`, per-task ownership cleanup. |
| Inspection (Oct 18–19) | `LAST-ERROR` pre-unwind capture (word+offset, frames, stack), `TRACE` single-stepping on all engines, `snapshot`, `help`, `complete`, `module_graph`, `develop`, `resync`. |

**This is an OS foundation, not the full intended OS.** The compiler/outer
interpreter, compositor and substantial policy currently live in C. It is not
yet a self-hosted Forth compiler/world, a multitasking kernel, a Plan 9 namespace
system, a POSIX implementation, or a networked board OS. Source persistence does
not preserve a running heap, suspended stacks, open handles or arbitrary RAM.

### Malleability semantics to preserve

- Native ordinary calls stay fast. Defining a new word with the same name does
  not magically retarget already compiled callers. Explicit `DEFER`/`IS` bindings
  provide replaceable service boundaries.
- Services include `CONSOLE-EMIT`, `CONSOLE-KEY`, `CLOCK-MS`, `FS-READ`,
  `FS-WRITE`, `DISPLAY-WRITE`, and the workbench's `WB-PAINT` hook. Core recovery
  paths do not all pass through user-replaceable bindings.
- Transactions validate through the C reference and a managed-data overlay.
  Irreversible device operations are rejected there. A native callback declared
  pure must honor the documented access/side-effect contract; this is trusted
  systems code, not a security sandbox.
- Module rollback only reclaims the latest activation checkpoint and refuses
  intervening dictionary growth. It is not arbitrary unload or general GC.
- `WB-PAINT` replacement retains observer state isolation and a 4096-instruction
  budget. Only the exact original default native painter gets the fast path;
  name shadowing or deferred rebinding must never bypass the observer.
- Compilation/partial CLI input and application execution take priority over idle
  painting. Rendering must not steal application `KEY` input or change DS/BASE.

## 4. Hardware and resource envelope

| Item | Established facts / working configuration |
| --- | --- |
| CPU | eZ80F92, ADL mode, 18.432 MHz; observed ID `0007`, revision `AA`. No MMU or user/kernel memory protection. |
| CPU memory | 512 KiB external SRAM, 128 KiB on-chip flash, 8 KiB on-chip SRAM. Current payload is confined to `0x040000..0x0AFFFF`; do not treat the wider 24-bit address space as populated RAM. |
| Current image | main-51: about 186 KB CPU binary, BSS end `0xAAB15`, about 17 KB below stack bottom `0xAF000`; separate 4 KiB native stack ends at `0xB0000`. Check `build/vios-resident.map` for the current build. This gap is not a promise of an implemented allocator. |
| ESP32 | ESP32-PICO-D4, 4 MiB flash, 8 MiB PSRAM. Its graphics/asset memory is not mapped into eZ80 RAM. Accepted app0 image is 669568 bytes. |
| Display | Pinned VDP 1.04 / vdp-gl; the ESP boots into the GUI compositor in mode 0 (640×480, 16 colours, 60 Hz). Text is rasterized into the ESP framebuffer. |
| Serial | Host USB-C → CH340 → ESP UART0 at **115200 8N1**; separate internal CPU/VDP UART at **1152000**, with matched 384000 also tested and 115200 supported. Keep RTS/CTS. |
| Storage | eZ80-owned microSD via SPI. The previously prepared FAT32 card contains VIOS source startup; do not reformat or alter its slot selection for ordinary development. |
| Input | User attached VGA and a keyboard. The USB-A-shaped keyboard socket requires a PS/2-capable keyboard, not arbitrary USB HID. Mouse initialization is disabled because its default GPIO pins are used by ZDI. |

Native eZ80 `fcell` uses the target's 24-bit unsigned int; the host reference uses
`uint32_t` with explicit 24-bit semantics. Native memory addresses and host
registered-region adapters are implemented in `forth/vm_memory.c`. Do not replace
this with the earlier research proposal's memory model without an explicit
design/migration. Opaque execution tokens, cell-width assumptions and wire/file
encodings need particular care when adding a future architecture.

Current configurable/finite limits include:

- 512 dictionary entries and 128 hash buckets; these are different limits.
- 256 data-stack cells, 256 return-stack cells and 32 loop frames.
- Eight module names, two retained generations each, one pending stage, four
  dependencies/module, 4096 source bytes/module and a 32 KiB module-source pool.
- One 4096-byte editor draft and one source undo snapshot. Two packed screen
  planes consume 19200 bytes; no second ESP character grid was added.
- Resident source lines: **235 ASCII bytes** after allowing the request envelope.
  Workbench patches: **80 bytes**, guarded by revision. Large source requests
  are host-chunked according to their tool contracts, not sent as an oversized
  single console line. Host tool schemas are authoritative.
- Physical key queue: 32 events. Idle painter: at most 128 display bytes/batch.

Inspect the linker/map and all separate arena capacities before expanding these
limits. Do not trade away tens of KiB casually on this target.

### Physical recovery wiring

These links were required for the accepted October 16 resident boot and recovery.
Since untethered boot they are optional: needed for dev boots and the ZDI
debug tools, not for normal use:

| Board GPIO header | Signal | ZDI header |
| --- | --- | --- |
| Pin 9 | ESP GPIO26, debug clock | Pin 4 |
| Pin 8 | ESP GPIO27, debug data | Pin 6 |

Keep `UART_DIS1` / `UART_D` **open**. The user made the two links with four
female-to-male wires and a breadboard. Two leads from different headers must
share each numbered connected strip; do not short the two signals together.
Disconnect USB and any battery before physical wiring changes. Confirm connector
orientation from [the verified wiring runbook](docs/bring-up/ram-console.md), not
from a remembered photograph or an assumed board revision. Do not enable a
mouse on GPIO26/27 while these links are needed.

## 5. Source map

| Path | Responsibility / relevant boundary |
| --- | --- |
| `forth/system.c`, `include/forth/` | Dictionary, compiler/interpreter, execution API, inspection and transaction interfaces. |
| `forth/vm_reference.c`, `forth/vm_memory.c` | Executable C semantics and native/host memory adapters. Preserve these for differential tests. |
| `forth/modules.c`, `kernel/vios_modules.c` | Live module lifecycle and persistent packages. |
| `arch/ez80/forth/engine.s` | DIRECT/TOKEN native inner engines. Read `tools/verification/forth-engine/ABI.md` before touching registers or frame layouts. |
| `arch/ez80/` | Entry, stage0, UART, SPI and timer assembly. eZ80 I/O ports need `IN0`/`OUT0`, not ordinary memory dereferences. |
| `boards/agon-light-2/` | Resident/raw platform adapters, storage binding and `ram.ld`. |
| `include/platform/` | Narrow console/input/storage/system/timer/VDP contracts. |
| `drivers/sd-spi/`, `drivers/fatfs/` | SD block transport and vendored FAT implementation. Preserve provenance. |
| `kernel/vios_services.c`, `kernel/vios_runtime.c`, `kernel/vios_boot.c` | Forth service bindings, runtime integration and SD source-slot boot policy. |
| `kernel/workbench.c` | Portable live editor/browser/inspector model and incremental composition. |
| `kernel/workbench_runtime.c` | Commands, input routing, hook isolation, idle painting, repaint and fence behavior. |
| `drivers/agon-vdp/text_screen.c`, `drivers/agon-vdp/transport.c` | Desired/painted cells, scroll queue, command encoding and bounded packet/input parsing. |
| `firmware/esp32-vios/` | Pinned VDP derivative, USB mux, independent supervisor/ZDI bootstrap, paired-image embedding and manifests. |
| `tools/vios_harness.py`, `tools/vios_resident_*.py`, `tools/vios_cli.py` | MCP/TCP owner, resident board/session state, CLI. `vios_board.py` also supports historical raw paths. |
| `tests/`, `tools/verification/forth-engine/` | Host/sanitizer/target-disassembly tests and native ABI/probe verification. |
| `docs/`, `backups/`, `artifacts/` | Research, experiments, acceptance records, original device images and immutable releases. |

Historical monitor/raw bridge code remains useful for recovery research, but the
production continuation target is **`vios-resident` + `esp32-vios`**. Do not
accidentally deploy a temporary probe instead of the full VDP resident image.

## 6. Build and verify without touching hardware

On this Mac the ignored local dependencies were provisioned as:

- Python environment: `.venv/`; packages pinned in `tools/requirements.txt`
  (`esptool==5.1.0`, `pyserial==3.5`).
- AgonDev v0.22: `.tools/agondev/`; Make accepts `AGONDEV` overrides.
- PlatformIO: `~/.platformio/penv/bin/pio`. The project pins espressif32 6.3.0,
  Arduino 2.0.9 through that platform, and ESP32Time 2.0.0. The normal VDP
  toolchain is separate from some historical RAM-helper toolchain notes.

A fresh clone does not contain those environments or ignored `build/` outputs.
Check/install the pinned dependencies from the project references; do not replace
them with a guessed latest SDK during another feature change.

Run from the repository root:

```sh
git status --short --branch
make test
make verify-forth-engine
.venv/bin/python tools/freeze_fast_display.py --verify-only

# After tests, build the two firmware ends sequentially at the same rate.
make vios-resident VIOS_INTERNAL_BAUD=1152000   # also builds build/agon-flash.bin, needed by the ESP embed step
make viosboot                                    # SD boot stub
VIOS_INTERNAL_BAUD=1152000 ~/.platformio/penv/bin/pio run -d firmware/esp32-vios -e esp32-vios
```

The host test count has grown well past the 184 tests of October 18; inspect
the `make test` output rather than comparing with a fixed number. On October 16
linked native verification had 146 probes. These are baseline results, not proof that a future tree or
machine has passed. Inspect each new run and preserve failures.

**Ordering matters:** host native tests can relink the CPU at the default rate.
Do not run tests concurrently with a paired firmware build. The `.baud` sidecar
and embed guard reject CPU/ESP mismatches; do not bypass them. For 384000, use
384000 in both build commands. Historical raw binaries default to 115200.

Current outputs are `build/vios-resident.{bin,elf,map,baud,gui}`,
`build/agon-stage0.bin`, `build/agon-crc.bin`, `build/agon-flash.bin`,
`build/viosboot.bin`,
`firmware/esp32-vios/.pio/build/esp32-vios/firmware.bin`, and
`firmware/esp32-vios/build-manifest.json`. Normal AgonDev CRT/libc examples may
call MOS; keep the freestanding startup/linker and audit linked helper routines.

## 7. Connect Claude, CLI and hardware without competing owners

First inspect device presence, its current owner and port 8765. Such host checks
do not require opening the board:

```sh
ls /dev/cu.usbserial-*          # currently /dev/cu.usbserial-210
lsof /dev/cu.usbserial-210
lsof -nP -iTCP:8765 -sTCP:LISTEN
```

If the board is absent, ask the user to connect it with the SD card in (ZDI links only if a dev boot or ZDI debug is needed).
The serial suffix can change. If another process owns it, identify that process
and its state rather than starting another owner. **Opening/closing this serial
driver can reset the ESP electrically.** Save live state before ownership changes.

If no owner exists, start one persistent resident harness:

```sh
.venv/bin/python tools/vios_harness.py --backend resident --port /dev/cu.usbserial-210 --tcp 127.0.0.1:8765
```

MCP construction/tool listing does not open hardware. Explicit `connect` attaches
without uploading a CPU image; startup may need up to 120 seconds. Once attached,
inspect `boot_status`, `modules`, `wb_status`, `wb_source`, `stacks`, `resources`
and `VDP-INFO` before mutation. Save current sources/state, not just the old JSON.

A CLI connects to that owner:

```sh
.venv/bin/python tools/vios_cli.py
```

For Claude's standard MCP stdio configuration, the tested bridge to the **same**
already running TCP owner is:

```json
{
  "mcpServers": {
    "vios": {
      "command": "/usr/bin/nc",
      "args": ["127.0.0.1", "8765"]
    }
  }
}
```

Alternatively, the Python harness without `--tcp` directly owns the serial port
for MCP stdio; stop the TCP owner first if choosing that arrangement. Never run
both owners. The loopback transport is newline-delimited JSON-RPC, protocol
`2025-11-25`, **not** Streamable HTTP. It is unauthenticated and only binds
127.0.0.1. Do not expose it externally as part of a convenience change.

Useful tool families:

- Inspect: `snapshot` (take one before any mutation/reset), `words`, `inspect_word`, `resources`, `stacks`, `modules`,
  `module_source`, `wb_status`, `wb_source`, `wb_view`, `wb_render_stats`.
- Mutate guarded state: `module_stage` → `module_activate` → `module_save`, or
  revision-checked `wb_patch` → `wb_stage` → `wb_activate` → `wb_save`.
- Execute: `evaluate`; when pending, use `await_result` and deliberate
  `console_input` for `KEY`. A timeout is not permission to send the same
  mutation again. Unknown/CRC-failed outcomes remain uncertain and fail closed.
- Debug/recover: `debug_registers`, `memory_read`, `resume`, explicit `reboot`.
  `reboot(force=true)` / CLI `:recover` abandons RAM state; on an untethered
  board it restarts only the ESP and the harness cannot reattach until a board
  reset, power cycle or harness restart. `deploy` and the ZDI debug tools need
  the wire; ordinary attachment does not need `deploy`.

Do not send raw ASCII terminal commands or old `SET CONSOLE`/hexload recipes into
the current framed USB mux. See the harness code/schema for exact contracts.
Native USB Ethernet, board TCP/UDP, Wi-Fi services and mutating UDP RPC do not
exist; the present TCP connection terminates on the Mac.

## 8. Recovery, deployment and preservation

Before any destructive benchmark/reset/update, retain exact active module
sources/dependencies, dirty draft, cursor/focus/selection/open state, engine,
BASE, stack state and boot selection. A module list alone cannot restore source.
The October 16 [preservation record](docs/bring-up/vios-fast-display-preserved.json)
and [restoration driver](tools/vios_fast_display_state.py) demonstrate the
specific saved-state workflow; do not overwrite today's work with that old
snapshot. Ad hoc definitions/stacks are not automatically persisted.

The last restored checkpoint had four active modules (`LIVE1I`, `LIVE1P`,
`LIVE1D`, `WBM1P`), an exact dirty 1969-byte `WBM1P` draft, BASE 10, DIRECT and
empty DS. Slot A was confirmed, CLEAN, generation 7. Original closed/browser
state was verified before intentionally reopening editor focus. These are
historical facts to compare with, not values to enforce over newer user work.

### Keep the independent escape route

- The USB/ZDI task remains reachable even if a buffered VDP command loops.
  Preserve that separation and complete-frame locking.
- The boot supervisor has a 30-second deadline, up to two automatic ESP restarts,
  then a halted/debuggable CPU after the third failed attempt. It is a **boot**
  watchdog, not a runtime watchdog for every healthy application.
- Healthy normal CLI, bounded managed errors and independent supervisor reset
  are different recovery levels. Do not promise Forth `ABORT` can escape an
  arbitrary native or graphics wedge.
- Historical `GO` and `RESUME` acknowledgement text contains `115200` as a frozen
  USB/protocol compatibility marker. Actual internal baud is in the build
  manifest. Changing those strings already caused a recovery regression once.
- Use the accepted native stage0. Earlier instruction-store LD/OUT injection
  experiments behaved unexpectedly on this board and are not the production
  bootstrap to revive by guesswork.

### Firmware writes

Application-only updates write **one ESP app slot (`app0` or `app1`), capacity `0x140000`**, with `tools/vios_firmware_ab.py`.
Stop the sole serial owner first, preserve current RAM work, identify the board,
back up the actual installed app0/image and check its hashes before updating.
Verify the paired manifest, image size and esptool's flash hash verification.
Then establish a single persistent owner and recheck startup/health.

After those prerequisites, the normal new-build install (it backs up `otadata`
and the target slot, writes the inactive slot on trial, and the firmware
confirms itself or falls back) is:

```sh
.venv/bin/python tools/vios_firmware_ab.py --port /dev/cu.usbserial-210 \
    install firmware/esp32-vios/.pio/build/esp32-vios/firmware.bin --backup-dir backups/ab-$(date +%Y-%m-%d)
```

Never mix CPU/ESP rates or use a whole-device erase/write as a shortcut. The ESP
bootloader and partition table are never written. The eZ80 flash was
written once, with owner approval, to install MOS 3.0.2 (2026-10-19);
changes to it, the bootloader, partitions or SD boot selection are a separate
recovery/design decision.

The latest immutable release is
[`artifacts/vios-fast-display-2026-10-16/`](artifacts/vios-fast-display-2026-10-16/RESTORE.md).
Its manifest SHA-256 is
`fc24b80355d5ac276ce9c7d345d8959760e2a5e1fe760205740102e434020acc`.
It includes both matched rates, source/evidence archives and a working previous
app0 restore payload. The latter is
`restore/pre-vios-fast-display-app0-2026-10-16.bin`, SHA-256
`e0ed0d20b2438e26e47404a152e9aeeeb98268f7c52113fcedec6854a8c845ae`.
It embeds its own older matching CPU image. Follow
[RESTORE.md](artifacts/vios-fast-display-2026-10-16/RESTORE.md); do not pair it with
the newer CPU binary.

Original full ESP/eZ80 backups and their metadata remain in `backups/`;
the verified original CPU flash version is not fully identified. Preserve all
existing releases and backups. Never regenerate an old release directory in
place. `freeze_fast_display.py` intentionally refuses an existing destination;
use a new milestone destination and suitable evidence requirements for new work.

Source A/B slots are dependency-ordered source bundles on one FAT32 card. They
handle rejected source updates; they do not isolate media failure or make FAT
power-fail atomic. ESP firmware A/B is separate (`tools/vios_firmware_ab.py`). See
[`kernel/vios_boot.c`](kernel/vios_boot.c) before extending the scheme.

## 9. Measured performance and remaining qualification

Latest plain-source display-only medians, October 16; USB reporting excluded:

| Operation | CPU composition ms | Fenced output ms at 384k | Fenced output ms at 1.152M |
| --- | ---: | ---: | ---: |
| Full repaint after composition | 125.715 separately | 232 | 202 |
| Unchanged | 1.278 | 3 | 3 |
| Character edit | 7.851 | 9 | 9 |
| Newline edit | 12.486 | 16 | 14.5 |
| Source scroll | 5.995 | 13 | 11 |
| Full-width source scroll | 6.786 | 10.5 | 10 |

Fenced output includes a cheap second compose and an ordered framebuffer query.
Thus source scroll is about **17 ms composition + fenced output**, not 11 ms
end-to-end keyboard latency. Earlier source-scroll painting was 52.5 ms/307
bytes; the latest eight-sample median is 57 bytes including cold cache misses.
Six warm samples median 40.5 bytes. Full-paint scene contents differ somewhat
from older runs; do not attribute every gain to baud alone. Full composition
still costs about 126 ms and is a real remaining CPU cost.

Other key constraints:

- The screen model is authoritative on the eZ80. `CHAR@` is a diagnostic pixel
  reconstruction, not the editor's source of truth.
- VDP buffers `0xFA00` and `0xFA01` are reserved for cached scroll scripts.
  Cache miss clears then writes a 19-byte payload; transaction costs 39 bytes,
  warm call six. Pinned 1.04 scrolling needs explicit eight-pixel movement.
- Scroll acceptance shifts both shadows/caches and repairs cursor styles.
  Dirty/coalesced/unsupported changes must fall back to repaint. Preserve the
  blank bottom-right cell, which avoids automatic whole-screen scrolling.
- `WB-ZOOM 0|1` / Ctrl+Space changes layout; `WB-REPAINT` reconstructs without an
  ordinary mode switch; `WB-REDRAW` retains full mode-reset recovery.
- `WB-COMPOSE` uses a 16-bit 288 kHz timer: modulo wrap at about 227.56 ms.
  Native output counters and host round trips measure different intervals.

### Open issues, not completed acceptance

1. **Physical keyboard smoke test remains pending.** User previously confirmed
   display/keyboard operation, but the latest 180-second stress window saw no
   physical cursor/zoom input and no operator reply. The report's failed
   assertion means the manual precondition was unmet, not an established device
   defect. Ask the user for arrows/held repeats, Tab and Ctrl+Space while CLI and
   rendering traffic run; record visual correctness, responsiveness and drops.
2. **USB bulk-output loss has a tested mitigation, unresolved physical cause.**
   Unpaced firmware twice lost encoded bytes before host decoding. Console
   frames now drain TX and yield a scheduler tick under the existing mutex;
   control/event frames have no extra delay. Thirty full-screen reads at each
   tested rate passed after pacing, plus benchmark/control traffic. Do not
   remove pacing or call the ESP UART/CH340/host-driver root cause proven.
   A scheduler tick is not a guaranteed full millisecond of silence.
3. **Long-duration mixed-load and power-failure qualification is incomplete.**
   Twenty-five final recovery checkpoints and earlier cold-start tests passed;
   these are not exhaustive soak or storage power-cut tests. Check the precise
   evidence scope before claiming stronger reliability.
4. **Memory reclamation and dynamic capacities need a design.** Finite arenas,
   retained generations and checkpoint rollback are deliberate. Larger source
   trees, tasks, inspectors or an image mechanism must budget RAM and define
   reference lifetime, not just increase constants.

The independent review and raw failures are retained in
[the review](docs/bring-up/vios-fast-display-review.md) and
[execution ledger](docs/bring-up/vios-fast-display-progress.md). Keep negative
evidence; never silently retry a CRC-failed mutation and present it as success.

## 10. Recommended path toward the full system

These stages extend the chosen direction. Validate interfaces and cost before
implementing them; do not treat this as permission to code all stages in one
unreviewed rewrite. Deliver a small working vertical slice at each step.

| Stage | Intended work | Completion evidence |
| --- | --- | --- |
| **0. Re-establish the baseline** | Reconnect/identify hardware, retain current user state, run host/native checks, qualify physical input, extend USB mixed-load soak and repeat cold start with the accepted pair. | Exact state restoration, framebuffer/reference agreement, no drop growth, usable CLI/MCP and recoverable forced faults. Investigate failures before more speed changes. |
| **1. Daily live-development workflow** | Complete a shared semantic command/presentation layer for keyboard, CLI and MCP; add completion/help, evaluation workspace, module/dependency inspection and explicit edit→test→activate→save feedback. Extend structured snapshots/events with bounded target work. | The same operations and errors work locally/headlessly with revision/generation checks; offline snapshots identify what is live, dirty, staged and persisted. No second divergent UI model. |
| **2. Task and I/O foundation** | Design cooperative tasks/safe yield points, per-task VM/compiler/error/I/O context, bounded cancellation and a console/control priority policy. Refactor global VM state deliberately; current transactions are synchronous and not task-safe. | Two useful tasks make progress while input/control remains responsive; explicit stack/context ABI, memory budgets and cancellation/recovery tests. Preserve fast single-task execution. |
| **3. Unix/Plan 9 service core** | Add a small VFS/handle contract, file/device endpoints, streams/pipes and local namespaces atop existing block/FAT support. Decide sharing/ownership and blocking semantics with the task design. | Shell/tool pipelines and inspectable device/control endpoints on Agon; short I/O, EOF, errors, cancellation and handle cleanup tested. No claimed POSIX or 9P compatibility without implementing/testing it. |
| **4. Rich inspectable tools** | Capture errors before destructive unwind, add typed lightweight descriptors `(kind, identity, generation)`, source maps and a checked stepping/debug path; grow graphics inspectors only where useful. | Inspect a failed computation without corrupting the fast engine; safe restart options only when an operation supplies them. Measure source-map and UI RAM cost. |
| **5. More Forth and self-contained development** | Move suitable OS policy/tools into Forth incrementally; build an on-device compile/test/package/rebuild workflow and eventually a metacompiler with a protected rescue base. Define durable workspace/configuration state separately from heap images. | Edit, test, save and reboot into an application from local SD without a Mac. Demonstrate a reproducible rebuild before calling the system self-hosted. |
| **6. Networking** | Start with host-side serial transport/services as appropriate; evaluate an ESP32 networking broker with explicit framing, quotas and recovery independence if board networking is needed. | Measured RAM/CPU/UART costs and simultaneous VGA/input/control traffic. CH340 remains serial: a host TCP proxy is not native USB Ethernet. Do not add unauthenticated remote mutation by accident. |
| **7. Robust distribution and another architecture** | Extend source updates and crash/power-loss tests; separately design firmware A/B if justified. Then choose a concrete 6502-family board/profile and implement its startup, drivers and native engine. | Recoverable interrupted updates and documented boot health. Shared portable tests pass on the new cell/memory profile; measure actual target performance. NMOS 6502 and 65C02 are distinct targets. |

The Plan 9 inspiration is a coherent file/service/namespace model; the
Smalltalk/Genera inspiration is navigating and changing a live system through
consistent tools. Neither requires copying a large desktop object VM, CLIM or
an emulator onto the Agon. Prefer small explicit contracts and compile-time or
native specializations where they preserve performance.

## 11. Validation and release discipline for each milestone

- Keep the C reference and native differential/ABI tests. Add meaningful
  regression tests for state, byte encoding, rollback and failure behavior;
  do not replace target measurements with a host simulator's instruction count.
- Benchmark on Agon, **display only during timed rendering**. Separate compose,
  native send, framebuffer completion and host latency; use exact fixtures and
  record cold/warm effects. Keep custom paint hooks valid.
- Re-run keyboard/CLI/pending-`KEY`/MCP traffic with painting and compare drop
  counters. A successful command write alone is not proof of display correctness
  or physical keyboard responsiveness.
- The fast-display benchmark replaces an unsaved disposable draft. The controls
  driver defines a temporary RAM word. Recovery scripts deliberately lose RAM.
  Read their docstrings and preserve live state before running them.
- For example, after attachment and preservation:

  ```sh
  .venv/bin/python tools/vios_fast_display_bench.py --baud 1152000 --samples 7 --output build/new-display-benchmark.json
  .venv/bin/python tools/vios_fast_display_controls.py --baud 1152000 --output build/new-control-acceptance.json
  ```

- Full fault recovery has its own serial owner. Stop the shared harness before:

  ```sh
  .venv/bin/python tools/vios_resident_recovery.py --exercise-resets --watchdog --log build/new-recovery-acceptance.json
  ```

- Preserve today's state and restore it after all resets. Describe any deliberate
  final UI state change; do not claim an entire RAM world survived a source-only
  restoration. Save accepted evidence under a new durable path before cleanup.
- Do not mutate existing `artifacts/` snapshots, their manifests or historical
  results. `.gitattributes` preserves original bytes; check archive/source hashes.
  `.gitignore` excludes environments/build outputs and the old active serial log.
- Finish with a concise account of behavior, size/timing costs, tests, remaining
  limits, installed image hashes and recovery route. Inspect working changes
  before committing; do not force-push or silently change repository visibility.

## 12. Suggested first message to Claude

> Read `CLAUDE_HANDOFF.md` and the linked latest acceptance/runbooks. Continue
> development of VIOS as an Agon-first, malleable Forth OS with native performance,
> C-reference tests, hybrid workbench, always-accessible CLI/LLM control and
> independent recovery. Check current Git/host state without opening serial
> blindly. Distinguish the October 16 measurements from today's hardware state.
> Present a short plan for baseline qualification and the next small full-system
> milestone before coding. Preserve live user work and frozen releases. Ask me
> for any needed physical board/SD/keyboard action, then carry the agreed
> implementation through testing and actual Agon measurements.

## 13. Progress 2026-10-18/19 (Claude session)

Board on `/dev/cu.usbserial-224130` (suffix changed from `-22430`). Every
flashed app0 and its manifest/ELF is in `backups/app0-2026-10-18/`; the
original Oct 16 installed image was read back first (byte-identical to
`artifacts/vios-fast-display-2026-10-16/firmware-accepted.bin`). Only app0 at
`0x10000` was written; SD slots, eZ80 flash, bootloader and partitions are
untouched. The Oct 16 RAM work (WBM1P and its 1969-byte draft) is restored
from `docs/bring-up/vios-fast-display-preserved.json` with
`tools/vios_fast_display_state.py` after each flash; it is RAM-only.

Stage status against section 10:

- **0** Baseline re-run; app0 backed up; keyboard confirmed by the user in
  the workbench (arrows, typing, Tab, Ctrl+Space). Long soak, cold-start and
  power-cut qualification still not done.
- **1** Done for this slice: snapshot, resync, complete/help (CLI Tab),
  module graph, develop cycle, local VGA console sharing the REPL.
- **2** Done (slice 1): tasks, gate, slice sweep, acceptance. Not done:
  per-task compiler/BASE context, locks, task-safe module transactions.
- **3** Done (slices 1-2): handles, pipes, /dev endpoints, cleanup, per-task
  stdin/stdout redirection with backpressure, `PIPELINE`, host SD file
  service (`DIR`, `HANDLE-PUT/GET`, `sd_list/put/get`, CLI `:ls :put :get`).
  Not done: shell syntax beyond `PIPELINE`, namespaces/mounts.
- **4** Done (slices 1-3): pre-unwind capture, TRACE, compile-time source
  maps (640 entries), host `describe` typed descriptors, `events`. Not done:
  restart options, graphics inspectors.
- **5** Started: Forth prelude, compiler data/string words, `J LEAVE +LOOP
  ?DO UNLOOP FILL MOVE U. DUMP`, native `TYPE`, AUTORUN (reboot path
  host-tested through real boot code; `apps/hello-autorun.fth`).
  On-device AUTORUN boot needs a boot-slot module (user approval for SD),
  metacompiler/self-hosted rebuild not started.
- **6, 7** Not started (host TCP owner remains the only network path).

RAM budget (Oct 19): CPU image ~144 KB, BSS end `0xA5E48`, ~37 KB below
the stack. New resident features need explicit budgets; candidates to
reclaim are the 5 KiB source-map pool and the 4.6 KiB console shadow.
See `docs/USER-GUIDE.md` and `docs/bring-up/vios-session-2026-10-19.md`.

Hardware lessons worth keeping:

- USB loss (open issue #2) reproduced on 2026-10-19: after 31.9 min of mixed
  load, 3 bytes vanished inside one 1-character console frame between ESP
  and Mac. Echo coalescing (one frame per streamed line) cut frame count and
  halved REPL latency (35.9 ms); `resync` now re-arms framing at a quiet
  prompt. Root cause (ESP UART/CH340/host driver) remains unproven.

- Never run bounded C work (task slices, console interpretation) while the
  REPL has re-asserted RTS: the 16-byte UART FIFO overruns and host frames
  are lost. Pause RX first (`platform_console_quiet` gate, drain after the
  keyboard poll).
- VDP 1.04 `CHAR@` matches the *current* text colours; set `3 COLOUR 128
  COLOUR` before diagnostic readback (the console cursor leaves them inverse).
- Recovery without reboot works: ZDI `memory_write` of task/console state,
  completing a partial line with `console_input 0d`, then `resync`. Symbol
  addresses must come from the exact flashed ELF (kept in backups); the
  compiler may narrow static ints to one byte.
- `tools/vios_keyboard_probe.py`, `vios_console_accept.py`,
  `vios_tasks_accept.py`, `vios_handles_accept.py`, `vios_core_words_accept.py`,
  `vios_slice_sweep.py` are the on-board acceptance drivers.

Pending user decisions: merge `refs/heads/claude/frosty-faraday-fa0037`
(BOOT-MANIFEST, real `persisted` in snapshots, +928 B); allow a test module
in an SD boot slot to demonstrate AUTORUN after reboot; physical checks of
the console cursor, editing keys and history.

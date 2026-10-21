# VIOS / Agon Light 2

VIOS is an Agon-first, malleable Forth operating-system foundation. It runs on the physical Agon Light 2 with native 24-bit cells, eZ80 assembly direct/token engines and a standard-C semantic reference. The default is **DIRECT**. The compiler emits both native representations and retains definition source for inspection and live development.

For continued development, start with the [Claude handoff](CLAUDE_HANDOFF.md):
current implementation, hardware/recovery workflow, known limits and the roadmap
toward the full system.

New here? Read the [VIOS manual](docs/manual/VIOS-MANUAL.md) (build, install, set up and use VIOS; every word; examples). The [user guide](docs/USER-GUIDE.md) is a short quick-start.

## Current state (2026-10-21)

- The board boots **untethered**: MOS (3.0.2, flashed once on 2026-10-19) runs the
  SD boot stub `viosboot`, which loads the resident embedded in the ESP firmware
  (or the SD copy). The ZDI wire is optional and only needed for dev boots and
  low-level recovery. Normal installs never write the eZ80 flash.
- The screen is the **GUI desktop** (mode 0, 640×480, 16 colours) with a
  *Console* window holding the REPL. **F1** toggles console/editor;
  **Ctrl+Alt+F1** switches between the GUI and a full-screen text console.
- ESP firmware is installed A/B with `tools/vios_firmware_ab.py` (backup, trial,
  self-confirm or automatic fallback to the previous slot). See the manual's
  [Installing](docs/manual/VIOS-MANUAL.md#installing) chapter.
- The harness registers 72 MCP tools. Tasks are preempted by a 1 kHz tick.

The dated sections below are history; where they disagree with the manual, the
manual and the code win.

## October 18–19 additions

All verified on the physical board unless noted; evidence in `docs/bring-up/*-2026-10-1[89]*`.

- **Local console on VGA.** Boot opens a VGA REPL console (home view); **F1**
  toggles it with the workbench editor. The local line editor supports
  Left/Right/Home/End, Backspace, Delete, Esc and Up/Down history; a cursor
  blinks; output produced while the editor is shown is kept. Physical keys
  are isolated from host command frames. See the [console spec](docs/superpowers/specs/2026-10-19-vios-console.md).
- **Background tasks.** `SPAWN`, `SPAWN-CON`, `PAUSE`, `MS`, `TASK-KILL`,
  `TASK-STATE`, `TASKS`, `TASK-OUT`, `TASK-SLICE`: four time-sliced Forth
  tasks preempted at instruction boundaries without native-engine changes
  ([task spec](docs/superpowers/specs/2026-10-18-vios-tasks.md)). A word named
  `AUTORUN` starts as a console task at boot.
- **Handles, devices and pipes.** `OPEN`/`CLOSE`/`READ`/`WRITE`/`PIPE`,
  `READ-WAIT`/`WRITE-ALL`, SD file streams, `/dev/null`, `/dev/cons`,
  `/dev/tasks`, per-task cleanup, `HANDLES`
  ([handle spec](docs/superpowers/specs/2026-10-18-vios-handles.md)).
- **User devices and startup.** `S" name" ' word DEVICE` serves `/dev/name`
  from a Forth word; `/VIOS/USER.FTH` runs at boot when present.
- **Inspection.** `LAST-ERROR` keeps the failing word, offset, caller frames
  and stack captured before unwind; `TRACE word n` single-steps on any engine.
- **Core words.** A Forth prelude adds `> 0< ROT NIP TUCK ABS MAX MIN TYPE
  SPACES WITHIN …`; the compiler adds `." CHAR [CHAR] CREATE ALLOT HERE , C,`.
- **Host tools.** `snapshot`, `resync`, `complete`, `help`, `module_graph`,
  `develop`, `tasks`, `handles`, `last_error`, `trace`; CLI `:snapshot`,
  `:resync`, `:help`, `:complete` (and Tab), `:graph`, `:why`, `:trace`.

The resident system adds concurrent framed USB control, native SD/FAT32, replaceable Forth service bindings, transactional source modules and A/B source startup. The [physical-board acceptance](docs/bring-up/vios-resident-accepted-run3.json) passed 137 checkpoints, including real module save/reload and v1 → v2 → v1 startup. [Recovery acceptance](docs/bring-up/vios-resident-recovery-run1.json) passed pending-input, infinite graphics-loop and automatic watchdog-reset cases. The [resident runbook](docs/bring-up/vios-resident-system.md) records limits and recovery details.

The native engine measurements show direct threading taking **11–14% less time than token threading** across five measured workloads. The October 16 workbench CPU image was 111,301 bytes with 255,060 bytes of BSS; the October 19 image was about 136 KB with BSS ending at `0xA1668`. The main-51 resident is about 186 KB; see `build/vios-resident.map` for the current layout. The dictionary capacity is 512 configurable entries. See the [workbench/VM measurements](docs/benchmarks/workbench-2026-10-16.md), [incremental composition measurements](docs/benchmarks/incremental-2026-10-16.md) and [fast-display measurements](docs/benchmarks/fast-display-2026-10-16.md). These measurements describe this implementation and these workloads.

Build with the provisioned AgonDev and PlatformIO toolchains:

```sh
make test verify-forth-engine
make vios-resident VIOS_INTERNAL_BAUD=1152000   # also builds build/agon-flash.bin, which the ESP embed step needs
make viosboot                                    # SD boot stub build/viosboot.bin
VIOS_INTERNAL_BAUD=1152000 ~/.platformio/penv/bin/pio run -d firmware/esp32-vios -e esp32-vios
```

Build the CPU and ESP sequentially with the same internal baud rate; the embed
script rejects a mismatch. USB stays at 115200. A matched 384000 pair is also
supported. Historical raw-monitor builds retain their 115200 default.

Install with `tools/vios_firmware_ab.py` and `tools/vios_sd_install.py` (see the manual's [Installing](docs/manual/VIOS-MANUAL.md#installing) chapter). For the installed resident firmware, start one persistent host harness. The serial suffix changes with the USB socket, so look it up and always pass `--port`:

```sh
ls /dev/cu.usbserial-*
.venv/bin/python tools/vios_harness.py --backend resident --port /dev/cu.usbserial-210 --tcp 127.0.0.1:8765
```

Call its `connect` tool to attach without uploading an image. For MCP stdio, omit `--tcp`; the [harness runbook](docs/bring-up/vios-harness.md) includes client configuration; the harness registers 72 tools, including 19 workbench tools (see the [manual's tool reference](docs/manual/VIOS-MANUAL.md#mcp-tool-reference)). MCP/JSON/TCP run on the host, and a compact binary protocol runs over USB serial. The CH340 does not provide a native USB network interface.

A human CLI attaches to the existing TCP harness:

```sh
.venv/bin/python tools/vios_cli.py
```

Forth lines, inspection, explicit KEY input and recovery are available headlessly. `:recover` intentionally discards unsaved RAM state and restarts the resident supervisor. On an untethered board (no ZDI wire) it restarts only the ESP; the harness then cannot reattach until a board reset, power cycle or harness restart. One process owns the serial port; do not start another harness while one is running. See [CLI usage](docs/bring-up/vios-cli.md).

The ESP firmware embeds the CPU image; MOS's boot stub loads it untethered, or the ESP loads it over ZDI when the wire is fitted. The eZ80 flash holds MOS 3.0.2 (written once on 2026-10-19; normal installs never write it). The ESP32 bootloader and partition table are never written; firmware goes into one app slot at a time with `tools/vios_firmware_ab.py`, which keeps the previous slot as the fallback. The original firmware backups remain available. The earlier temporary RAM bridge is retained as `--backend ram`; its [historical runbook](docs/bring-up/vios-harness-ram-profile.md) and [wiring prerequisites](docs/bring-up/ram-console.md) remain separate.

The [hybrid text workbench](docs/bring-up/vios-workbench.md) provides a browser, exact-source editor and live inspector on an 80×60-cell screen (under the GUI, a full-screen window). The CLI supports `:wb open`, `:wb view` and `:wb close`; keyboard and LLM tools share the same guarded operations. Retained panes, indexed source lines and sparse character/style runs replace the full-frame rebuild on ordinary updates. A 60-byte dirty-row index keeps the measured 32-byte edit render near 4.2 ms. Composition and transmission are measured separately. The plain CLI and independent supervisor recovery remain available.

Integrated viewport scrolling now moves existing pixels on the ESP and paints
only exposed/damaged cells. Native bounded UART output, per-batch command
compression and two cached scroll scripts reduce a measured source scroll from
52.5 to 11 ms through framebuffer completion, with CPU composition measured
separately at 6 ms. **Ctrl+Space** or `WB-ZOOM 1` opens the full-width editor;
`WB-ZOOM 0` restores it. `WB-REPAINT` reconstructs without an ordinary mode
switch; `WB-REDRAW` retains full display recovery. See the
[benchmark methods, limits and USB integrity investigation](docs/benchmarks/fast-display-2026-10-16.md).

- [Resident system and limits](docs/bring-up/vios-resident-system.md)
- [Text workbench runbook](docs/bring-up/vios-workbench.md)
- [Rendering benchmarks and limits](docs/benchmarks/workbench-2026-10-16.md)
- [Incremental composition benchmarks](docs/benchmarks/incremental-2026-10-16.md)
- [Source scrolling and newline benchmarks](docs/benchmarks/source-rows-2026-10-16.md)
- [Fast transport, viewport scrolling and full-width editor](docs/benchmarks/fast-display-2026-10-16.md)
- [Resident implementation/evidence ledger](docs/bring-up/vios-resident-progress.md)
- [Original Forth milestone](docs/bring-up/vios-forth.md)
- [Hardware reference](docs/research/hardware.md)
- [Original eZ80 backup metadata](backups/ez80/original-2026-10-16.json)

Source, hardware research and evidence are versioned in Git. Immutable releases under `artifacts/` preserve the source and evidence snapshots made during hardware qualification, including snapshots from before the initial Git commit. Archived references retain their notices and licenses. ESP32 ZDI protocol code derives from the MIT-licensed Agon recovery project; see `tools/esp32-ram/NOTICE.txt`. Vendored VDP and vdp-gl retain their own licenses and pinned source identities.

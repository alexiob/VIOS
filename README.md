# VIOS / Agon Light 2

VIOS is an Agon-first, malleable Forth operating-system foundation. It runs on the physical Agon Light 2 with native 24-bit cells, eZ80 assembly direct/token engines and a standard-C semantic reference. The default is **DIRECT**. The compiler emits both native representations and retains definition source for inspection and live development.

For continued development, start with the [Claude handoff](CLAUDE_HANDOFF.md):
current implementation, hardware/recovery workflow, known limits and the roadmap
toward the full system.

New here? Read the [VIOS manual](docs/manual/VIOS-MANUAL.md) (build, install, set up and use VIOS; every word; examples). The older [user guide](docs/USER-GUIDE.md) is a short summary.

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

, concurrent framed USB control, native SD/FAT32, replaceable Forth service bindings, transactional source modules and A/B source startup. The [physical-board acceptance](docs/bring-up/vios-resident-accepted-run3.json) passed 137 checkpoints, including real module save/reload and v1 → v2 → v1 startup. [Recovery acceptance](docs/bring-up/vios-resident-recovery-run1.json) passed pending-input, infinite graphics-loop and automatic watchdog-reset cases. The [resident runbook](docs/bring-up/vios-resident-system.md) records limits and recovery details.

The native engine measurements show direct threading taking **11–14% less time than token threading** across five measured workloads. The October 16 workbench CPU image was 111,301 bytes with 255,060 bytes of BSS; the October 19 image is about 136 KB with BSS ending at `0xA1668` (about 55 KB below the 4 KiB stack). The dictionary capacity is 512 configurable entries. See the [workbench/VM measurements](docs/benchmarks/workbench-2026-10-16.md), [incremental composition measurements](docs/benchmarks/incremental-2026-10-16.md) and [fast-display measurements](docs/benchmarks/fast-display-2026-10-16.md). These measurements describe this implementation and these workloads.

Build with the provisioned AgonDev and PlatformIO toolchains:

```sh
make test verify-forth-engine
make vios-resident VIOS_INTERNAL_BAUD=1152000
VIOS_INTERNAL_BAUD=1152000 ~/.platformio/penv/bin/pio run -d firmware/esp32-vios -e esp32-vios
```

Build the CPU and ESP sequentially with the same internal baud rate; the embed
script rejects a mismatch. USB stays at 115200. A matched 384000 pair is also
supported. Historical raw-monitor builds retain their 115200 default.

For the installed resident firmware, start one persistent host harness:

```sh
.venv/bin/python tools/vios_harness.py --backend resident --tcp 127.0.0.1:8765
```

Call its `connect` tool to attach without uploading an image. For MCP stdio, omit `--tcp`; the [harness runbook](docs/bring-up/vios-harness.md) includes client configuration for 65 tools, including 19 workbench tools. MCP/JSON/TCP run on the host, and a compact binary protocol runs over USB serial. The CH340 does not provide a native USB network interface.

A human CLI attaches to the existing TCP harness:

```sh
.venv/bin/python tools/vios_cli.py
```

Forth lines, inspection, explicit KEY input and recovery are available headlessly. `:recover` intentionally discards unsaved RAM state and restarts the resident supervisor. One process owns the serial port; do not start another harness while one is running. See [CLI usage](docs/bring-up/vios-cli.md).

ESP32 app0 embeds the CPU RAM image and verifies it through ZDI at startup. Original eZ80 flash remains untouched. The ESP32 bootloader/partition table/app1 were preserved, and the original firmware backups remain available. The [resident runbook](docs/bring-up/vios-resident-system.md) includes the application-only restore command. The earlier temporary RAM bridge is retained as `--backend ram`; its [historical runbook](docs/bring-up/vios-harness-ram-profile.md) and [wiring prerequisites](docs/bring-up/ram-console.md) remain separate.

The [hybrid text workbench](docs/bring-up/vios-workbench.md) provides a browser, exact-source editor and live inspector in mode 1 (80×60 cells). The CLI supports `:wb open`, `:wb view` and `:wb close`; keyboard and LLM tools share the same guarded operations. Retained panes, indexed source lines and sparse character/style runs replace the full-frame rebuild on ordinary updates. A 60-byte dirty-row index keeps the measured 32-byte edit render near 4.2 ms. Composition and transmission are measured separately. The plain CLI and independent supervisor recovery remain available.

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

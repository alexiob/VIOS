# Changelog

All notable changes to VIOS are listed here. Versions follow semantic versioning; the version lives in the VERSION file (see docs/RELEASING.md).

## [Unreleased]

## [0.8.0] - 2026-10-21

First tagged release. VIOS turns the Agon Light 2 into a self-contained Forth workstation: a resident Forth system on the eZ80 that runs without MOS once booted, and a display server on the ESP32 with a windowed, Smalltalk-style GUI.

### System

- A bare-metal resident Forth on the eZ80 with 24-bit cells; it drives its own UART, timer, SPI, SD card and FAT32 and makes no MOS calls after start.
- Three engines run the same compiled words: `DIRECT` (native direct-threaded, the default), `TOKEN` (native token-threaded) and `C` (the portable reference); `ENGINE` switches between them without recompiling.
- Four background tasks beside the console: `SPAWN`, `SPAWN-CON`, `PAUSE`, `MS`, `FRAME-WAIT`, `TASK-KILL`, `TASK-STATE`, `TASKS`, `TASK-OUT`, `TASK-SLICE`, each with its own stacks, `BASE` and 128-byte output ring.
- Tasks are preempted by a 1 kHz tick at safe points (4 ms slices); `PREEMPT` and `PREEMPT-STATS` control and show it, `CRITICAL(` … `)CRITICAL` protect shared updates, and Ctrl+C interrupts a running command within about 50 ms.
- Source modules are staged, tested on the reference engine, activated and rolled back as units (`MODULE-BEGIN`, `MODULE-STAGE`, `MODULE-ACTIVATE`, `MODULE-ROLLBACK`, `MODULE-DISCARD`), with two generations kept and dependencies pinned by source CRC-32.
- `SAVE` writes content-addressed, CRC-checked module packages to the SD card, and `MODULE-LOAD` loads them back.
- Two boot slots of modules with automatic trial and confirm: `BOOT-TRIAL`, `BOOT-STATUS`, `BOOT-MANIFEST`, `BOOT-CONFIRM`, `BOOT-REVERT`; a trial that fails its health check falls back to the confirmed slot.
- `/VIOS/USER.FTH` runs at every boot, and a word named `AUTORUN` starts as a console task after it.

### Forth

- A Forth prelude widens the core word set (`ROT`, `NIP`, `TUCK`, `MAX`, `MIN`, `WITHIN`, `SPACES`, `/MOD`, `*/`, `BOUNDS`, `ERASE`, `DUMP` …), plus native `TYPE`, `FILL`, `MOVE` and `U.`.
- Counted loops with `?DO`, `+LOOP`, `LEAVE`, `UNLOOP` and `J`; strings and characters with `."`, `S"`, `CHAR` and `[CHAR]`.
- `DEFER` and `IS` for late binding, and six replaceable service bindings: `CONSOLE-EMIT`, `CONSOLE-KEY`, `CLOCK-MS`, `FS-READ`, `FS-WRITE`, `DISPLAY-WRITE`.
- Every definition keeps its source, so `SEE` shows exactly what was typed.
- `LAST-ERROR` reports the last failure with the failing word, offset, source token, the top 8 stack cells and up to 16 caller frames, captured before the stacks are cleared.
- `TRACE word n` single-steps a word on any engine and shows the source token of each step.
- `WORDS`, `RESOURCES`, `.S` and `BENCH` (micro-benchmarks on all three engines) for looking at the running system.

### Compiler

- Extensible compiler: `IMMEDIATE`, `POSTPONE`, `[` `]`, `LITERAL`, `STATE`, `COMPILE,`, `FIND`, `EVALUATE`, `CREATE` … `DOES>` with `>BODY`, `MARKER`.
- `CREATE`, `ALLOT`, `,`, `C,`, `HERE`, `VARIABLE`, `CONSTANT` and `DEFER` work inside definitions, so defining words work.
- Compiling is about 35% faster (340 → 220 µs per token on the eZ80): keywords are looked up in a hashed table.
- Word headers shrink from 55 to 25 bytes: names live in a shared name pool (about 9 KB of RAM back). The first boot after upgrading rebuilds the boot cache once.
- Words that change the dictionary are refused in background tasks, inspector runs and module tests; `MARKER` refuses to remove module words.
- Source maps hold 1,792 entries in 6-byte form (was 640); `RESOURCES` reports `map-entries N/M` and the browser status line shows the map's fill.

### GUI and display server

- The ESP32 runs a 640×480, 16-colour window compositor from power-on, before the eZ80 has loaded; the console is a window on the desktop, and Ctrl+Alt+F1 switches to a full-screen text console and back.
- Frames are composed at every vertical blank in about 3 ms of each 16.7 ms frame; large repaints are composed off-screen and shown at once.
- Windows with titles, shadows, focus, zoom, modal dialogs and keyboard move and resize (F5, F6, Ctrl+F5, Ctrl+W), driven from Forth with `UI-WINDOW`, `UI-SURFACE`, `UI-TYPE`, `UI-FOCUS`, `UI-CLOSE` and batched with `UI-BEGIN` … `UI-COMMIT`.
- Two looks, switchable at run time with `UI-STYLE`: Smalltalk (world menu) and Turbo Vision (menu bar and status line).
- Commands with accelerators, menus with submenus (`UI-COMMAND`, `UI-MENU`, `UI-POPUP`), and a command palette on Ctrl+K.
- Widgets drawn and edited on the ESP: label, button, check box, radio button, input field, list and table (tree and pager modes), progress bar, tabs and multi-line text views, laid out by boxes; events are pulled with `UI-NEXT` and `UI-EVENT`.
- Canvases: retained display lists of lines, rectangles, bars, ovals, discs, stipples, text and icons (`UI-CANVAS`, `UI-PEN`, `UI-LINE`, `UI-BAR`, `UI-PRINT`, `UI-ICON` …); single items are moved, recoloured or erased (`UI-NUDGE`, `UI-INK`, `UI-ERASE`) and only the changed part is redrawn.
- Up to 8 canvases of up to 640×480 with 255 items each, eight built-in 16×16 icons, 24 user bitmaps, and a `CANVAS` event naming the item under a click.
- Smooth animation on the ESP: `UI-ANIMATE` tweens windows, sprites and canvases with linear, in-out, out and spring easing, up to 16 at once.
- Classic VDU drawing from Forth (`CLS`, `COLOUR`, `MODE`, `PLOT`, `CHAR@`, `PIXEL@`, `VDP-INFO`); `UI-RAW` hands the screen to it and takes it back.
- Sprites (`UI-SPRITE`, `UI-SPRITE-AT`), a built-in arrow pointer, and Ctrl+F12 to drive the pointer from the keyboard.
- GUI tools: a Browser (Ctrl+Shift+B) to read and edit modules and accept source live, a Workspace (Ctrl+Shift+W) with do it, print it and inspect it, an Inspector (Ctrl+Shift+I) for stack, memory, tasks, handles and system, and a Manager (Ctrl+Shift+M) for boot slots and packages behind a confirmation dialog.
- The text workbench (F1): a full-screen module browser, inspector and source editor with stage and activate keys, still available beside the GUI tools.
- Console lines are edited on the ESP (cursor keys, Home, End, Delete, Esc and a four-line history), so typing stays responsive however busy the eZ80 is.

### Devices and files

- Handles on SD files, pipes and devices: `OPEN`, `CLOSE`, `READ`, `WRITE`, `READ-WAIT`, `WRITE-ALL`, `PIPE`, `PIPELINE`, `HANDLES`; a task's input and output can be redirected, and a killed task's handles are closed.
- Built-in devices `/dev/null`, `/dev/cons` and `/dev/tasks`, and up to four user devices served by Forth words with `DEVICE`, Plan 9 style.
- `/dev/win`: every window and widget as text files (`/dev/win/new`, `/dev/win/N/ctl`, `/dev/win/N/text`, `/dev/win/N/event`), served by the ESP, so a UI can be built and driven with `OPEN`, `READ` and `WRITE`.
- `/dev/win/N/event` streams a node's events as text lines that a task can wait on with `READ-WAIT`.
- `/dev/ui/tree` and `UI-SNAPSHOT` print Forth source that rebuilds the current windows and widgets.
- SD card on FAT32: `DIR`, `LOAD`, `INCLUDE` (4 deep), `FILE-READ`, `FILE-WRITE` and `FILE-REMOVE`; larger files are read and written as streams through handles.
- `/host/` paths reach files on the Mac while the harness runs, so a program edited on the Mac runs with `LOAD /host/app.fth`.

### Tools and host harness

- `tools/vios_harness.py` owns the USB port and serves the board as 72 MCP tools and as JSON-RPC on loopback TCP `127.0.0.1:8765`.
- `tools/vios_cli.py`, a terminal console on the harness with Tab completion and commands such as `:help`, `:why`, `:trace`, `:graph`, `:ls`, `:put`, `:get`, `:snapshot` and `:recover`.
- `develop` stages, activates and optionally saves a module in one call; `snapshot` records what is live, dirty, staged and persisted.
- USB link layer: the ESP numbers every frame, keeps the last 24 and resends lost frames on request; `esp_stats` shows `link_requests`, `link_resent` and `link_gone`.
- ESP-side probes that work while the eZ80 is busy: `esp_stats`, `esp_console`, `esp_gui` (key presses and framebuffer pixels), `esp_inject`, `esp_audio`.
- `tools/vios_board_suite.py` runs the on-board acceptance checks (GUI, GUI tools, console, audio, core words, tasks, pipeline, handles, tools) with an optional soak.
- `tools/vios_release.py` packages a release: ESP firmware, SD card tree, install tools, this changelog's section and SHA-256 sums.

### Audio

- A three-voice synthesizer on the ESP (square, triangle, sawtooth, noise) mixed at 16384 samples per second in the sound interrupt, out of the 3.5 mm jack and the buzzer (with its `BUZ_EN1` jumper closed).
- `TONE`, `TUNE`, `PLAY`, `STOP`, `VOLUME` and `SOUND?`; tunes are stored as text in 8 slots of 255 bytes and played on the ESP without the CPU.
- Tune notation with notes, rests, octaves, lengths, tempo, volume, waveform, articulation, loops (`*`) and chaining between slots (`&n`); bad tunes are refused with the slot and offset of the error.

### Boot, firmware and recovery

- Boot and console show `VIOS v0.8` and `(c) 2026 Alessandro Iob`.
- Untethered boot: MOS 3.0.2 runs the `viosboot` stub from the SD card, which streams the resident from the ESP firmware at 1,152,000 baud with per-block CRCs, falling back to `/vios/vios.bin` on the card.
- `viosboot sd` and `viosboot force` choose the image order or ignore the unfinished-boot counter; after three unfinished boots the stub stays at the MOS prompt.
- The boot-module cache restores the confirmed slot's compiled modules instead of compiling them: 857 ms instead of 2,168 ms; `BOOT-TIMES` shows where boot time went.
- ESP firmware A/B: `tools/vios_firmware_ab.py` installs into the slot that is not running, backs up what it replaces, and the new firmware confirms itself only when the eZ80 reports healthy, otherwise it rolls back.
- `tools/vios_sd_install.py` prepares a card from the Mac and `tools/vios_sd_refresh.py` updates the SD copy through the running board.
- The ESP answers MOS 3.0.2's boot-time VDP queries as VDP 2.16.0 does.
- Dev boot over the optional ZDI wire: the ESP loads and checks the resident itself, can halt and inspect the eZ80, and a 30-second boot watchdog restarts a failed boot.
- If no SD card or no boot stub is found, the console window says so instead of staying blank.

### Documentation

- The VIOS Manual (`docs/manual/VIOS-MANUAL.md`): architecture, building, installing, the console, the Forth language, modules, tasks, devices, files, the GUI and its tools, audio, the harness, troubleshooting, example programs and a word reference.
- Manual screenshots and animations come from the real CPU and display-server code running on the host (`tools/vios_manual.py media`).
- A user guide (`docs/USER-GUIDE.md`) and design specs under `docs/superpowers/specs/`.

### Known limits

- The canvas tween and the 16-tween + 20-sprite gate are tested on the host only.
- A long session of redefinitions eventually fills the dictionary (words, source, map); `MARKER` or a restart frees it.
- The power-cut tests of `SAVE` and `BOOT-TRIAL` are pending.
- Fixed capacities: 512 dictionary entries, 8 modules of up to 4096 bytes, 4 tasks, 8 handles, 4 pipes and 4 user devices.
- Not ANS Forth: no `CATCH`/`THROW` (errors clear the stacks), no double-cell words, and `PICK`, `ROLL`, `CASE` and `VALUE` are not provided; control structures are parser keywords, so `FIND` returns 0 for them.
- No memory protection, no garbage collector and no module unload; only source persists across a restart (modules and `USER.FTH`), not the running heap.
- No networking on the board; `/host/` works only while the harness runs on the Mac.
- PS/2 keyboard only; the PS/2 mouse is off (Ctrl+F12 turns the keyboard into a pointer).
- The VDP loop gap reaches 14–16 ms under GUI load, against a 4 ms target.
- The USB link (CH340) occasionally drops bytes; the link layer recovers the frames, but the cause is not known.
- Untethered boot is qualified with MOS 3.0.2 only.
- The boot watchdog's give-up after three failed boots is tested on the host, not induced on the board.

# VIOS Resident System Implementation Plan

> Use executing-plans with bounded parallel ownership. Track evidence in docs/bring-up/vios-resident-progress.md.

**Goal:** Concurrent VDP/USB control, native SD source persistence, replaceable services and reproducible module activation on the actual Agon.
**Architecture:** ESP32 boot/transport supervisor around a pinned VDP; native eZ80 Forth service trap; freestanding SD/FAT32; source-based transactional modules; host MCP control.
**Tech Stack:** C99, eZ80 assembly, pinned upstream Arduino ESP32 VDP, Python MCP/TCP tools.
**Spec:** docs/superpowers/specs/2026-10-16-vios-resident-system.md

## Global constraints
- Preserve original backups and previous artifacts; no eZ80 flash programming.
- Single hardware owner: parent. Workers do not open serial or flash.
- No Git repository/worktree/commit gates. User approved implementation; ask only for genuinely missing physical actions.
- Fixed capacities, 32-bit SD sectors, native24bit Forth; no per-op generic device abstraction.
- Red/green tests and actual-board evidence for each subsystem.

## Review focus
- Interrupted/CRC-bad frames resynchronize without turning graphic data into controls.
- Failed stage/activation leaves all existing definitions, DEFER bindings and managed data intact.
- SD large-sector arithmetic works beyond24bit and timeout handling survives timer wraps.
- Startup failure retains recovery route and never changes CPU flash.
- Partial source save/invalid startup cannot destroy the known-good boot source.

### Task1: Shared VDP and control transport
- [x] Firmware worker: pin/fetch full compatible VDP source/dependencies; build reproducibly.
- [x] Firmware worker: shared C COBS/CRC codec with malformed/fragmented/oversized frame tests.
- [x] Firmware worker: VDP-context console extension, USB control task and embedded VIOS ZDI bootstrap; app0 bounds.
- [x] Parent: host COBS serial adapter, board UART console/VDP demux with buffering and native service hooks; fake transport and actual-board tests.

### Task2: SD/FAT32
- [x] Storage worker: SD SPI driver/register glue, explicit timeouts and card capacity; licensed FatFs/native integer-width audit.
- [x] Storage worker: storage_init/read_file/write_file/rename/remove/mkdir/sync/error with host FAT disk-image tests and failed-write cases.
- [x] Parent: native target build and real-card mount/read/write/readback while preserving prior files.

### Task3: Transactional modules
- [x] Module worker: native registration, compiler snapshots, C-only managed-data transaction; pure native guard.
- [x] Module worker: stage/activate/discard/strict LIFO rollback, retained sources/CRC/dependency inventory; regressions for failed definitions/data/IS/test.
- [x] Parent: outer LOAD/INCLUDE and module commands, versioned SD persistence and known-good startup selection; host module tools and exact-source/hash/test operations.

### Task4: Integration and acceptance
- [x] Parent: console/file/clock/VDP native services with replaceable deferred Forth bindings and inspectable service inventory.
- [x] Parent: prepare/review firmware image and rollback command, preserve current app0, deploy/verify through USB.
- [x] Parent: actual VDP replies plus graphics/audio commands, SD module save/load, activation/rebinding/rollback.
- [x] Parent: reset/cold-start with no host upload; verify persisted module and headless harness reconnect.
- [x] Independent review, full host/native probes and hardware regressions, footprint/latency benchmark, runbooks and new artifact manifest.

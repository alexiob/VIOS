# VIOS Text Workbench Implementation Plan

> Use executing-plans with bounded parallel ownership for independent API/model/renderer units. The user explicitly requested implementation of the selected design; do not add approval handoffs. No Git/worktree/commit steps apply to this non-Git workspace.

**Goal:** Live browser/editor/inspector text panes, preserved CLI/LLM control, and measured Agon rendering.
**Architecture:** Structured Forth inspection + portable bounded model/editor + dirty-span VDU backend, serialized at REPL idle boundaries. All buffers static; <=48 KiB additional CPU BSS; <=128 bytes per idle render batch.
**Tech stack:** C99, existing eZ80 native engines, pinned VDP 1.04, Python host tools.
**Spec:** docs/superpowers/specs/2026-10-16-vios-text-workbench.md

## Constraints and review focus
- Preserve artifacts/vios-resident-2026-10-16 and original backups; parent alone opens serial/flashes app0; no eZ80 flash writes.
- Native opcode hot path unchanged; no JSON/networking on target; no heap.
- UI inspection and paint observers preserve application stacks/BASE/engine/error state, including failures.
- Stale catalog/editor identities, dirty draft replacement, source/control-byte bounds and failed updates must fail safely.
- No local keyboard bytes leak into tagged CLI lines; floods have bounded queues/drop counts.
- No repaint monopolizes the normal CLI; raw VDP stalls retain tested supervisor recovery.

### Task 1: Structured VM inspection and bounded observer (API owner)
- [x] RED→GREEN tests for paginated metadata/source, variable/deferred inspection, catalog invalidation after commit/rollback and no invalidation from failed staging.
- [x] Add include/forth/inspect.h and forth/system.c functions; no generic opcode instrumentation. Add nonreentrant bounded observer with temporary stack and exact state restoration.

### Task 2: Portable model/editor (model owner)
- [x] RED→GREEN tests for module/word browsing, 4096-byte draft, atomic revision-checked patch, one undo, navigation/focus, staged/activated state and error preservation.
- [x] Add include/vios/workbench.h and kernel/workbench.c; consume structured Forth metadata and existing module APIs. Keep draft buffers outside VM data arena. Compose into the text screen API.

### Task 3: Dirty text screen and keyboard demux (display owner)
- [x] RED→GREEN tests for unchanged/full/partial spans, 128-byte batches, sanitization, per-cell style and cursor placement.
- [x] Add include/vios/text_screen.h and drivers/agon-vdp/text_screen.c; desired/painted packed cells and deterministic command log.
- [x] Add bounded raw-key queue/routing to VDP transport and platform input API. Test old console behavior while disabled, event packets while enabled and overflow handling.

### Task 4: REPL/controller/host integration (parent)
- [x] RED→GREEN tests for idle/CLI priority, WB commands, lossless draft protocol, state guards and Forth paint rebinding.
- [x] Add kernel/workbench_runtime.c, idle hook and build wiring; register WB native/deferred paint words. Add MCP/CLI headless workbench operations with strict bounds. The USB CLI remains the output/evaluation workspace in this milestone.
- [x] Full host/native checks; reviewed RAM deployment; actual text/graphics readback, edit-stage-activate-save workflow and concurrency/recovery acceptance.

### Task 5: Benchmark/release (parent and independent review)
- [x] Measure full/empty/edit/selection/scroll render bytes/times, framebuffer/model readback and native VM impact; verify CPU/ESP32 footprint.
- [x] Fix important review findings with regressions; preserve old app0 backup and update only app0 after acceptance; validate resident startup/CLI/recovery.
- [x] Runbooks, live harness, new immutable artifact manifest/source/evidence archive; qualify physical display/keyboard observations.

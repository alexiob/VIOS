# VIOS fast display implementation plan

> **For agentic workers:** use test-driven development and execute the owned tasks below; never access the physical serial port from a worker.

**Goal:** deliver faster display transport, integrated pane scrolling, fewer VDU commands and a faster native painter, with measured Agon results and responsive headless/local control.

**Architecture:** the eZ80 retains the authoritative workbench and desired/painted screens. The Agon renderer queues semantic viewport scrolls and uses existing ESP32 buffered VDU commands for repeated operations. Hardware-specific code stays in the board/architecture/VDP layers; the compositor sees a bounded optional scroll operation with an ordinary repaint fallback. A full-width editor mode exploits the VDP's scanline scrolling path.

**Spec and authorization:** the user's explicit instruction to implement all recommendations in `docs/benchmarks/render-display-only-2026-10-16.md`, benchmark on the Agon and preserve recovery/control. This plan implements that already-reviewed design; no additional approval gate is needed.

## Global constraints

- eZ80 18.432 MHz, resident RAM image, pinned VDP 1.04, mode 1: 80×60/four colours.
- Keep USB serial at 115200. Internal UART targets 384000 and 1152000, coordinated at both ends with RTS/CTS intact.
- Do not write eZ80 flash or alter SD boot selections. Back up/readback-verify current ESP app0 before approved application-only updates.
- Preserve the current draft, live module source, BASE and original workbench state before RAM replacement/reset.
- One physical serial owner. Parent alone deploys, changes firmware, resets or benchmarks hardware.
- Keep idle output bounded to 128 bytes; command caching and scroll commands must obey that budget and preserve control servicing.
- Existing Forth-replaceable paint hook keeps observer isolation and its execution budget. Only the exact immutable default native painter gets a direct path.
- Preserve frozen releases under `artifacts/`; create a new release rather than modify them.

## Task 1 — Native transport (worker-owned)

**Files:** `arch/ez80/uart0.s`, `arch/ez80/uart0_raw.s`, `arch/ez80/uart0.h`, `boards/agon-light-2/platform_resident.c`, `Makefile`, transport build configuration, ESP baud configuration and bridge rate diagnostics; associated UART/platform tests.

**Interfaces:** keep `platform_vdp_send(const void *, size_t)` and all RX/pause/resume contracts. Add a bounded `unsigned ez80_uart0_tx_burst(const unsigned char *, unsigned)` that writes only while CTS and TX-ready permit and returns progress; C services RX between bursts/waits. Historical raw monitor builds retain their 115200 default. Resident and ESP builds share a reproducible selectable rate, default 1152000.

- [x] Add failing native-machine/FIFO tests for partial/zero progress, CTS, TX readiness, register/ABI preservation and overrun accounting.
- [x] Implement the bounded native writer and remove redundant per-byte C RX collection.
- [x] Add behavioral rate-initializer tests for 115200/384000/1152000, including cold clocks and inherited DLAB.
- [x] Coordinate build rates and retain rate-independent USB/ZDI supervisor recovery.
- [x] Run UART/platform/ESP bridge tests and report RED→GREEN evidence. Do not build/deploy ESP firmware or touch hardware in the worker.

## Task 2 — Renderer operations and command compression (worker-owned)

**Files:** `drivers/agon-vdp/text_screen.c`, `include/vios/text_screen.h`, `tests/text_screen_native.c`, `tests/test_text_screen.py` and renderer-specific tests.

**Interfaces:** add `int vios_text_scroll(unsigned x, unsigned y, unsigned width, unsigned height, int rows)`. Positive rows move contents up, negative down; vacated cells become fg3/bg0 spaces. Return 1 only when the region can safely be scrolled; return 0 without mutation on invalid geometry, region dirt, unsupported displacement or a prior queued scroll. Dirt outside the region is allowed. Accepted scrolls shift desired and painted shadows consistently and queue the VDP operation before subsequent glyph output. `vios_text_pending()` includes queued operations; reset cancels them. Keep existing stats structure layout.

- [x] Add failing framebuffer-model tests for both scroll directions, pending edits, coalesced composition, cursor styles, clipping, reserved bottom-right and tiny budgets.
- [x] Implement the scroll queue and at most two tiny ESP buffered-command cache entries, with explicit buffer replacement and invalidation. Use standard pinned VDP commands; no duplicate 9600-byte ESP screen map is needed.
- [x] Add failing byte-cost tests for sparse spans and same-style short gaps while checking the final framebuffer independently.
- [x] Compress colours/cursor positions within a batch and merge profitable gaps, without depending on raw VDU state left by another caller. Track painted clean-gap cells correctly.
- [x] Verify native target pending/scan bounds and sanitizer framebuffer equivalence. Report commands/tests/results; no hardware.

## Task 3 — Compositor integration and full-width editor (worker-owned)

**Files:** `kernel/workbench.c`, `include/vios/workbench.h`, `tests/workbench_native.c`, `tests/test_workbench.py` and compositor-specific tests. Parent owns runtime command parsing and hook optimization.

**Interfaces:** consume Task 2's optional `vios_text_scroll(...)`. Add `void vios_workbench_zoom(int enabled)` and `int vios_workbench_zoomed(void)`; normal source geometry is x26,width54,y18,height38; zoom is x0,width80,y18,height38. Keep the existing status structure/protocol unchanged. Parent exposes `WB-ZOOM 0|1` and `WB-ZOOM?`; use Ctrl+Space as the local toggle. Zoom invalidates composition but does not reset the VDP mode, source or selection.

- [x] Add failing equivalence tests for one/multiple source-row movement, insert/delete newline, horizontal movement, styles, full invalidation and operation refusal.
- [x] Request semantic scrolls when source rows can be reused, shift row/cursor caches consistently, and project only exposed/damaged/cursor rows. Retain ordinary repaint fallback for ambiguous edits or dirty regions.
- [x] Add failing zoom tests for 80-column projection, movement, toggle restoration, keyboard shortcut and reserved screen boundaries.
- [x] Implement zoom with bounded memory; keep the browser/inspector usable in the remaining upper region and restore full layout on exit.
- [x] Run portable/sanitized compositor tests. Do not edit renderer/runtime files or access hardware.

## Task 4 — Runtime, integration and verification (parent-owned)

**Files:** `kernel/workbench_runtime.c`, runtime and platform test fixtures, new hardware benchmark/control driver, documentation/artifact metadata.

- [x] Add a failing runtime test that distinguishes exact immutable default painter from a shadowed native name and a replaced deferred target.
- [x] Preserve exact binding semantics while calling the trusted default directly; custom hooks still use `forth_observe`.
- [x] Expose zoom commands and local toggle; separate repaint invalidation from expensive mode reinitialization where safe. Preserve explicit recovery from paint errors/raw display damage.
- [x] Run the entire suite plus linked-engine verification. Review all three worker deliverables and get an independent whole-change review.
- [ ] Preserve current live state and verified old artifacts. Build matching 384000 images, back up ESP app0, update only app0, and verify resident startup, keyboard/CLI/MCP/control and display-only benchmarks. Repeat at 1152000; keep the best rate that passes reliability checks.
- [x] Benchmark precomposed painting, composition, real scroll/edit/zoom cases and cold/warm command-cache cost. Record a framebuffer fence and distinguish native vs observer vs host timing.
- [ ] Stress maximal CLI lines, pending KEY and local keyboard routing while painting; require no receive-drop growth and correct framebuffer glyph/style output. Exercise supervisor recovery and confirmed SD boot preservation.
- [x] Restore user's live draft/state, preserve firmware hashes/footprint/results, document measurements and create a new immutable release with a restore route.

The two hardware/control checkboxes above remain partially open only for a
physical keyboard observation: no operator input/reply arrived in the
three-minute window. Both matched rates, framebuffer checks, source edits,
CLI/MCP/pending KEY, zero-drop stress and 25 recovery fault checkpoints passed.
After exact restoration, the editor is intentionally open for this pending
manual check. See the acceptance JSON for the explicit automated/manual scope.

## Review focus

1. A new edit arriving after a scroll is queued or partly painted must converge to exactly the desired frame without moving unrelated panes.
2. Old cursor highlight, CRLF/tabs, newline insertion/deletion and zoom transitions must agree with a full reference reconstruction.
3. Cached VDP buffers must be replaced rather than appended, fit output budgets, and remain recoverable if the VDP parser is wedged.
4. Higher baud and larger native bursts must not overrun the finite CPU FIFO or starve keyboard/USB control; resets and startup must use matching rates.
5. Default-painter shortcuts must not bypass replaced hooks, name shadowing, BASE/stack isolation, or error recovery.

## Execution ledger

See `docs/bring-up/vios-fast-display-progress.md`. The repository has an unborn main branch and all project files are untracked; work proceeds in place with existing frozen releases and explicit source backups rather than creating unrelated Git history. Worker ownership prevents overlapping edits. Parent owns hardware integration and accepts interface changes explicitly.

# VIOS Forth Implementation Plan

> **For agentic workers:** Use executing-plans and independent workers for the explicitly assigned files. Track evidence here and in docs/bring-up/vios-forth-progress.md.

**Goal:** Resident Agon Forth, C testing reference, actual token/direct hardware benchmark.
**Architecture:** Shared semantic/primitive contract; native 24-bit target engines; dual-emitting resident compiler; fixed memory regions and explicit live service bindings. CPU/board specifics remain outside portable language code.
**Tech Stack:** Standard C99, Agondev eZ80 ADL assembly, ESP32 RAM/ZDI loader, Python host tests.
**Spec:** docs/superpowers/specs/2026-10-16-vios-forth.md

## Global constraints
- No processor flash programming. Preserve existing backups and monitor.
- Cells are 24 bits; address/engine layout is architecture-specific.
- Native and reference semantics must agree; bounds failures recover.
- Timing uses target hardware, excludes UART, and verifies workload results.
- No Git repository: evidence ledger replaces commit/worktree bookkeeping.

## Review focus
- Rollback after compiler/dictionary exhaustion; incomplete words stay hidden.
- Signed 24-bit arithmetic, shifting, multiplication and division boundaries.
- Return/loop stack separation, nested calls and balanced explicit return-stack use.
- Live deferred bindings change existing callers; direct bindings remain stable.
- Timer wrap/calibration and equivalent work/results across engine representations.

### Task 1: VM reference and correctness
- [x] Implement reference globals/stack/memory/arithmetic and execution in forth/vm_reference.c and forth/vm_memory.c; owner reference worker.
- [x] Native C99 ASan/UBSan tests in tests/forth_vm_native.c and tests/test_forth_vm.py: boundaries, stack errors, calls, branches, loops, memory, budget termination.

### Task 2: Native engines
- [x] Implement arch/ez80/forth/engine.s, assembled twice for token and direct dispatch; owner engine worker.
- [x] Match include/forth/vm.h globals and traps; return to C only for explicit slow primitives. Expose direct primitive-address table and DOCOL.
- [x] Build and inspect target assembly. Parent verifies matching workloads on board.

### Task 3: Resident compiler and tools
- [x] Implement forth/system.c and include/forth/system.h; owner parent.
- [x] Emit dual dictionaries, native DOCOL headers, runtime literals/variables/control flow; preserve source and atomic definition publication.
- [x] Tests define/run words, nested flow/loops, deferred rebinding, source/SEE, compiler rollback and resource errors against host reference.

### Task 4: Board timing and large BSS
- [x] Add board timer counter/measurement wrappers and correct target startup for BSS larger than64KiB; owner platform worker.
- [x] Confirm timer registers and prescaler using local Zilog primary source; document wrap/measurement units. Do not touch serial hardware from workers.

### Task 5: Hardware integration and results
- [x] Parent builds VIOS RAM payload, deploys/readbacks through existing RAM loader and checks REPL commands for C, token and direct engines.
- [x] Run repeated known-result workloads, measure timer ticks and code/stack/dictionary usage, preserve raw serial evidence/binary checksums and measured recommendation.
- [x] Independent final code review; fix material issues with regressions; update README/runbook.

### Added scope: headless LLM/harness integration
- [x] Tagged ASCII completion protocol, input for pending KEY, fixed-radix word/resource metadata.
- [x] Persistent MCP stdio and loopback TCP tools, CRC/readback RAM access and CPU debug resume.
- [x] Real-board MCP/TCP integration and nine-sample request latency measurements; retained logs.
- [x] Independent review, protocol regressions and UART FIFO resume fix verified on hardware.

Evidence: docs/bring-up/vios-framed-verified.* and vios-harness-{tcp,stdio}-hardware.json; benchmark summary docs/benchmarks/vios-profiled.json. Final artifacts and checks recorded in artifacts/vios-2026-10-16/manifest.json.

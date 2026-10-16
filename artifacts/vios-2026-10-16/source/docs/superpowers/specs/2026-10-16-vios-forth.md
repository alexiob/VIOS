# VIOS Agon-first Forth milestone

User approved implementation and hardware benchmarking on 2026-10-16. Project/OS name: VIOS. Build a resident 24-bit Forth for Agon Light 2 with standard-C semantic reference, eZ80 assembly token and direct-threaded engines, thin board drivers and source-oriented live development. Do not weaken Agon features to match future CPUs.

The shared VM contract is include/forth/vm.h. Three-byte cells on target, masked 24-bit reference values on host. Token code uses one-byte opcodes and three-byte operands. Direct code uses three-byte native primitive addresses, and colon definitions begin with a native CALL to DOCOL, making calls to colon words one threaded cell. Both use identical arithmetic, branching, stacks and primitive semantics. VM globals are native 24-bit cells on target. No Forth call consumes recursive C frames. Native engine calls return traps for console and explicit dynamic-service operations; ordinary arithmetic/calls remain inside assembly.

Resident compiler emits both representations, hashes names, preserves source and supports numbers, colon definitions, variables/constants, conditionals, BEGIN loops, DO/LOOP, execution tokens, explicit deferred services, WORDS/.S/SEE/resources and recoverable language/resource errors. Aborted definitions must not become visible. Deferred service changes affect compiled callers explicitly using the deferred entry; ordinary calls are bound directly. Initial dictionaries/arenas have fixed documented capacities. Host reference uses registered emulated address regions; target memory words use native addressing. This is a trusted systems language, not a security sandbox.

Hardware benchmarks use an eZ80 timer and exclude serial output from timed regions. Compare identical arithmetic/call/branch/loop/memory workloads with known results. Record repeated samples, code bytes, compile time and stack high-water information. No claim that one format universally wins from a narrow benchmark. Reuse RAM-only serial deployment, verify readback, preserve original firmware and monitor recovery. The ESP32 RAM bridge temporarily replaces VDP; graphics/audio integration, SD persistence, cold boot and full self-hosting follow later.

Standard C provides compiler/reference/support; architecture assembly is allowed wherever it makes the target efficient. No Git repository exists; keep the existing workspace and evidence ledger, do not require commits/worktrees or additional approval loops for the explicitly approved milestone.

LLM and harness integration is a core requirement. Keep machine-readable request
framing and inspection available headlessly. MCP stdio and a loopback TCP JSON-RPC
bridge live on the Mac; the Agon's USB interface is a serial bridge, so native USB
Ethernet/IP is not assumed. VIOS accepts tagged source lines and emits completion
records after execution, preserving BASE, stacks and multiline compilation.
Pending KEY input can be supplied explicitly without contaminating the input
stream with synchronization commands. Expose source/dictionary/stack/resource
inspection and verified RAM/debug operations. Measure target image/BSS cost and
actual end-to-end hardware request latency, keeping JSON/network costs off the
resource-constrained CPU. Full VDP, device/filesystem services, persistence and
self-hosted development remain subsequent OS work.

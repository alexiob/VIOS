# A Forth foundation for the Agon OS

Historical research proposal. The implemented VIOS milestone follows the later
[Agon-first specification](../superpowers/specs/2026-10-16-vios-forth.md): native
eZ80 direct threading is the default, token threading remains selectable, and
the C VM is retained for testing. See the [hardware results](../benchmarks/README.md).

Research proposal, 2026-10-16. No VM or OS code is implemented by this document. Hardware facts and the approved first deployment remain in [hardware.md](hardware.md) and [bring-up-proposal.md](bring-up-proposal.md).

## Recommendation

Build a resident Forth system as the OS's interactive language, compiler and primary implementation environment. Keep a small freestanding C machine layer, minimal architecture assembly and independent recovery monitor beneath it. Put most evolving OS policy in Forth once the VM works.

Start with a **token VM implemented in standard C**, running directly on the eZ80 after the existing RAM bootstrap. Preserve that implementation as the executable specification and desktop test engine. Add an assembly VM engine or native-code compiler only when target measurements justify one. This gives us a small, understandable foundation without requiring a new optimizing compiler before we can experiment with the OS.

Here, “native Forth” means a resident interpreter/compiler executing on the board with direct access to its machine services. Whether compiled Forth definitions are tokens, threaded addresses or machine instructions is a separate decision. A resident token compiler is still a real compiler; it just targets our VM.

The portability promise should be **portable Forth source and stable OS interfaces**. CPU startup, interrupt entry, VM execution engines, memory/banking adapters and board drivers still need ports. Compiled dictionaries are generally target artifacts. A Forth core makes a small porting boundary possible; it does not make the hardware boundary disappear.

## Why this fits, and what it costs

| Benefit | Corresponding cost or limit |
| --- | --- |
| Inspect hardware and change behavior from the serial prompt; redefine services without rebuilding the entire image. | An unrestricted prompt can corrupt the running system. Reliable recovery and a protected base dictionary are essential. |
| Keep the interpreter, compiler, tools and application language in one resident environment. | We must build a usable editor, source loader, diagnostics and rebuild process before “self-contained” means daily development without the Mac. |
| Express higher layers in compact, factored Forth with a small machine interface. | Small source size does not guarantee small RAM use or fast execution. Dispatch, dictionary names, stacks and debug metadata consume memory. |
| Bring portable services to a second architecture by reusing source. | Code that assumes cell width, physical addresses, dictionary layout or return-stack internals will break across ports. |
| Prototype files, devices, pipes and namespaces interactively. | Forth supplies language machinery, not a filesystem, process model, network stack or Plan 9 semantics. We still design those. |
| Keep a desktop reference VM for deterministic tests and debugging. | Host tests cannot establish interrupt correctness, UART timing, SD behavior or generated target ABI correctness. |

This is attractive for an exploratory, inspectable personal OS on small machines. It is a less comfortable fit if the priority becomes compatibility with existing Unix binaries, strong isolation between untrusted programs, a large C application ecosystem or predictable optimized performance without VM tuning. The Agon eZ80 has no MMU/privilege boundary; changing language does not provide one.

## Implementation choices

| Approach | Advantages | Costs | Assessment |
| --- | --- | --- | --- |
| Standard-C token VM with small assembly/hardware adapters | Same semantics on host and target; easy tracing, bounds checks and differential tests; clear machine-independent instruction format. | More dispatch and representation overhead; C compiler quality matters on eZ80 and particularly 6502. | **Recommended first implementation and permanent reference.** |
| Per-CPU threaded Forth engine | Can exploit eZ80 registers and instruction set; tiny and fast inner interpreter is feasible; high-level Forth can remain shared. | Every port needs assembly work and ABI validation; compiled code is architecture-specific; debugging engine/compiler errors is harder. | Good measured optimization after the reference works. An assembly token engine is also possible without changing the token compiler. |
| Adapt an existing Forth | Working compiler, tools and test corpus arrive sooner. | Existing word semantics, cell model, dependencies and licensing may shape the whole OS; removing MOS/host assumptions is real engineering. | Evaluate against a written core contract; do not assume an available Forth is already a bare-metal OS kernel. |

### Threading and compilation vocabulary

In **indirect threading**, a compiled word refers to a code field that refers to its implementation. **Direct threading** removes that extra lookup for primitive code addresses. **Subroutine threading** emits machine calls/returns. **Native compilation** may additionally inline and optimize machine instructions. **Token threading** uses instruction identifiers resolved by a dispatcher; bytecode is a token representation with byte-sized instructions. Ordinary ISO C can implement a `switch` dispatcher, whereas labels-as-values/computed `goto` is a compiler extension. These distinctions follow [Anton Ertl's implementation guide](https://www.complang.tuwien.ac.at/forth/threaded-code.html).

Our proposed first format is byte-oriented tokens with explicit literal, branch, call and return operations. Calls refer to dictionary identifiers/offsets, not serialized C function pointers. Fix primitive identifiers for a given image ABI. Do not use an enum's in-memory representation as the file format. Keep execution tokens opaque at the language boundary so another engine can implement them differently.

There is no universal fastest threading choice. In particular, desktop branch-prediction results in implementation papers are not eZ80/6502 measurements. Benchmark arithmetic, calls, dictionary lookup, console handling and block I/O on the actual board before choosing assembly specialization or a machine-code compiler.

## Existing implementations worth studying

| Project | Verified characteristics | Relevance and limits |
| --- | --- | --- |
| [Agon Forth](https://github.com/lennart-benschop/agon-forth) | Has 16-bit Z80 and 24-bit ADL versions. Its [24-bit documentation](https://raw.githubusercontent.com/lennart-benschop/agon-forth/main/forth24/README.md) describes a direct-threaded engine, cached top of stack, assembler, save-system support and a metacompiler that can run on Agon. It requires MOS. | Strong local feasibility reference and potential prototype. Its runtime/stack layout and I/O are not our independent kernel. The repository [license](https://github.com/lennart-benschop/agon-forth/blob/master/LICENSE) is GPLv3; adopting code introduces copyleft considerations for distributed derivatives. Review file-level notices and intended project licensing before reuse. |
| [pForth](https://github.com/philburk/pforth) | ANSI-C implementation aimed at 32/64-bit platforms; can run with character I/O without an OS and export dictionaries as C source. | Useful portable-core candidate and bootstrap example. Neither 24-bit eZ80 nor 16-bit 6502 suitability is established by its portability claim. Audit pointer/cell assumptions and target size first. Its current [license text](https://github.com/philburk/pforth/blob/master/license.txt) grants use, copying, modification and distribution with or without a fee. |
| [Embed Forth](https://github.com/howerj/embed) | Small C99 VM with Forth metacompiler, self-rebuilding image and MIT-licensed VM/image/metacompiler. Its [header](https://github.com/howerj/embed/blob/master/embed.h) fixes 16-bit cells and 32,768 cells of core. | Excellent reference for a minimal, reproducible VM bootstrap. The fixed address/cell model limits a direct adoption on Agon; extending it is a format/compiler change, not just increasing a buffer. |
| [Tali Forth 2](https://github.com/SamCoVT/TaliForth2) | Bare-metal subroutine-threaded ANS-like Forth specifically for **65C02**. Source has platform separation and tests. Its [copying notice](https://github.com/SamCoVT/TaliForth2/blob/master-64tass/COPYING.txt) places it in the public domain and points to individual routine provenance. | Valuable 6502-family implementation and testing reference. It uses CMOS instructions: it is not an unchanged port to the original NMOS MOS Technology 6502. Choose the actual future processor/board first. |

These are research candidates, not vendored dependencies. Before adopting one, pin an upstream commit, preserve provenance and license files, run its tests, inspect generated target code and enumerate all firmware/host calls. No implementation has yet been proven suitable or measured on this board for our requirements.

**A quick Agon Forth proof is useful without adopting its architecture.** Once working MOS/VDP is established, load its existing binary into RAM and exercise serial input, defining words, its assembler and a self-rebuild on a backed-up development card. That would demonstrate the interaction model cheaply. It would not demonstrate an independent kernel: MOS still supplies services and owns reserved memory/stack conventions. Keep this experiment separate from our bare-metal monitor evidence. A permanent fork would need an audit and replacement of MOS entry/exit, console/storage calls, memory layout and recovery behavior, plus the licensing decision. Running the existing binary as a comparative test is a smaller commitment than copying its implementation into the OS.

**A C VM on a 6502 is a reference-first strategy.** A small implementation may be viable, but there is no benchmark here proving the proposed 24-bit-style checked engine would fit or feel responsive on a particular 6502 machine. The 16-bit profile avoids unnecessary width, yet compiled C dispatch, pointer/index manipulation, software stacks and checks can still be expensive. Treat a desktop C engine as mandatory for testing and an actual 6502 C engine as a measured prototype. Expect an assembly token/threaded engine to be a likely production requirement for a constrained 6502 while reusing the language, compiler and OS source. Tali demonstrates a CPU-specific approach is feasible; it does not prove our chosen VM's performance. Record ROM/RAM, stack budgets, serial throughput and compiler speed before promising parity between ports.

## Cell widths and memory model

The [Forth standard](https://forth-standard.org/standard/usage) permits implementation-defined cells of at least 16 bits; addresses and execution tokens fit a cell. It does not standardize dictionary layout or compiled representation. Its [portability guide](https://forth-standard.org/standard/port) explains the need to avoid assumptions about implementation internals. We should follow the standard where practical and publish an explicit supported subset, extensions and environmental limits rather than claim full conformance.

| Logical cell | Agon implications | 6502 implications |
| --- | --- | --- |
| 16 bits | Small operands/stacks, but one flat address cannot cover the working 448 KiB payload RAM. Requires a restricted arena or extra address/bank mechanisms. | Natural first profile for an ordinary 64 KiB machine; still competes with ROM, I/O, stacks and application RAM. |
| 24 bits | Matches eZ80 ADL addressing and can represent the whole Agon arena. Unusual width needs explicit arithmetic and storage rules in portable C. | More byte operations and RAM than 16 bits; a banked physical address space still requires board support. |
| 32 bits | Simplifies conventional C types and interchange with desktop systems; wider arithmetic has a cost on this CPU. | Four-byte arithmetic and stacks are expensive on an 8-bit CPU. Does not create additional physical memory. |

**Proposed baseline:** a 24-bit logical-cell Agon profile, a 16-bit profile for a future ordinary 6502, and optional 32-bit profiles for larger targets. Run the same high-level source tests under 16 and 24 bits from the start. Do not require identical compiled images across these profiles.

For the standard-C reference, represent a 24-bit logical value in an unsigned C type of at least 32 bits and explicitly mask arithmetic results. Pack dictionary/data-space cells as three bytes through load/store helpers. C working stack elements may initially occupy four bytes: logical width is not a promise about C `sizeof`. An optimized eZ80 backend may use native 24-bit storage/registers behind the same contract. Measure the reference implementation before approving that backend.

Define two's-complement logical values, signed comparisons, division rounding, shifts and overflow deliberately. Avoid C signed overflow, invalid shift counts, implementation-dependent signed right shifts and implicit narrowing. For double-cell multiplication/division, use bounded multiword algorithms where necessary; do not require fast 64-bit arithmetic. Publish supported numeric ranges.

Use **byte-addressed VM arena offsets** for Forth data addresses, explicit byte encodings and opaque execution tokens. `@`/`!` access the allowed VM data arena, not arbitrary C pointers. This makes a desktop engine possible without pretending host pointers fit 16/24 bits. Hardware access goes through board words or narrow machine primitives. A privileged raw-memory/debug extension can exist, clearly architecture-specific, outside the portable language contract.

Portable Forth uses `CELL+`, `CELLS`, character/byte accessors and named OS interfaces; it does not assume that a cell is two/four bytes or that an execution token is a data address. File offsets and wire fields get explicit widths independent of cells. On a 16-bit port, large offsets may use double cells or dedicated handles. Near/far pointers and banking, if needed, belong in a separately specified board/address adapter.

## Small initial core and code boundaries

The first Forth milestone is a single-task serial REPL that can compile and execute definitions in RAM. Include bounded data and return stacks, integer/bit operations, checked memory access, literal/call/branch/exit operations, dictionary search, numbers, `: ;`, basic conditionals/loops and console I/O. Add `WORDS`, `.S`, a simple definition decoder, abort and a stable way back to the recovery monitor. Move token parsing and the outer interpreter/compiler into Forth as bootstrap stages permit.

Aim for a small primitive set, not a predetermined magic number. Do not make floating point, an optimizer, native code generation, dynamic linking, preemption, networking or complete ANS/POSIX compatibility prerequisites for this milestone. A handful of file/device calls should appear only after the standalone REPL is tested.

| Area | Responsibility |
| --- | --- |
| `vm/`, `include/vm/` | Standard-C reference execution, explicit value/address semantics and checked VM state. No board headers. |
| `forth/core/`, `forth/tools/` | Portable language, compiler, diagnostics, bootstrap and tests. |
| `forth/os/` | Device/filesystem policy, shell words, scheduler policy and later namespaces. |
| `arch/ez80/`, later `arch/6502/` | Entry/exit, interrupt/context ABI, I/O instructions and optional optimized VM engine. |
| `boards/agon-light-2/`, later a named 6502 board | Memory/linker map, board wiring, clock and boot/recovery bindings. |
| `drivers/` | UART, VDP framing, SPI/block drivers with narrow machine contracts. |
| `platform/host/` and `tests/` | Desktop I/O adapter, deterministic VM runs and cross-profile checks. |

Keep the C recovery monitor useful even if the Forth dictionary or compiler becomes corrupt. Initially a reset/reload may be the only reliable escape from a completely wedged engine; promise a software escape only after testing it on hardware.

## Stacks, interrupts, tasks and malleability

The reference engine should use explicit bounded data and return stacks, plus a separate compiler control stack and native C stack. Never implement every Forth call by recursive C calls: deep Forth nesting must not consume an unbounded C stack. Forth return-stack use must follow the [standard's balancing restrictions](https://forth-standard.org/standard/usage). [CATCH](https://forth-standard.org/standard/exception/CATCH)/`THROW` are useful language-level recovery semantics, but they cannot undo an invalid hardware write or guarantee recovery after arbitrary native corruption.

Begin with one task and polled I/O. Later add cooperative scheduling at explicit safe points. Each task needs its own data/return stacks, instruction state, exception state, input buffers, numeric formatting state, current directory and namespace references. Keep dictionary compilation serialized. Publish a new definition only after it is complete; an aborted definition should not leave a half-linked entry.

Do not execute the interpreter/compiler from an interrupt handler. Architecture ISRs should acknowledge hardware and enqueue bounded events; normal task context processes them. Protect shared queue updates with a target-appropriate interrupt/atomic protocol: `volatile` alone is insufficient. Preemption would require a separately reviewed context-save ABI and reentrancy design.

Redefining a word should shadow it for future lookup. Existing compiled calls need not change automatically; live patching requires explicit indirection. First support replacing stopped services, not arbitrary hot replacement of executing code. `FORGET`/dictionary reclamation must not free definitions still referenced by tasks or other words. Prefer a fixed base dictionary and a resettable development region initially.

Bounds checks, stack guards, call traces, word names and resource quotas improve diagnostics. An unrestricted low-level Forth is not a security sandbox, even with those checks. Keep raw hardware/native-code facilities out of any later restricted application profile.

## Storage, images and self-hosting

Source text is the portable, version-controlled authority. A saved dictionary is a cache/build artifact with a declared target profile. Even token images may embed cell-sized literals, offsets or layout assumptions; tokens alone do not make an image architecture-independent.

When persistence is introduced, give images a magic value, format version, VM ABI, cell width, byte encoding, image lengths, entry identifier and integrity check. Never save native C structs, pointers, active tasks, open file handles or live peripheral state as a rebootable image. Reinitialize runtime resources at boot. Native machine-code images additionally identify architecture, board bindings and relocation requirements.

Start SD work read-only. Load Forth sources from text files through a narrow file interface; retaining a FAT-compatible boot/development area makes host recovery practical. Persistent updates come after card backup and tested recovery. Avoid treating a raw RAM dump as a durable image format. Later image/source writes need an interrupted-write recovery design; do not assume an SD/FAT rename is power-loss atomic.

Use staged self-hosting claims:

1. **Resident development:** define and compile words over serial with no host compiler for those words.
2. **Resident application builds:** edit/read source and rebuild services using the board's Forth system and SD.
3. **Reproducible Forth bootstrap:** regenerate the language/compiler dictionary from source and compare deterministic results with host builds. [Embed's metacompiler](https://github.com/howerj/embed) demonstrates this style of fixed-point check.
4. **Machine-layer self-hosting:** add a target assembler/metacompiler capable of regenerating machine primitives and boot images, while retaining an externally recoverable seed.

The first three do not automatically rebuild a C VM binary on the board. Fully rebuilding that machine layer requires either a resident C toolchain or a Forth assembler/metacompiler replacement. That is a later, explicit milestone.

## Unix/Plan 9 ideas that fit

Build a small shared-address-space system with Forth tasks and file-like objects. Start with a console, RAM files and basic open/read/write/close operations. Then add pipes and per-task path/namespace tables. Forth wordlists organize language names; filesystem namespaces organize resource paths. They are different mechanisms.

Plan 9's [9P specification](https://9p.io/magic/man2html/5/intro) describes message-based access to named resources, a useful future boundary for local device servers and remote resources. Start by making local file/device operations coherent; only then introduce a bounded 9P subset with explicit protocol version, framing, tags, identifiers and resource limits. A serial 9P experiment can precede IP networking. ESP32 wireless hardware does not provide an eZ80 network protocol automatically.

This is Plan 9/Unix-inspired design, not a promise to run Plan 9 programs, POSIX binaries, `fork`, an MMU process model or a full network stack. Naming the desired semantics before naming compatibility will keep the small machine honest.

## Proposed stages and evidence

1. **Finish the approved hardware groundwork.** Identify and back up firmware, establish headless serial loading, deploy the small C monitor, capture successful commands and demonstrate recovery. This work is still necessary under a Forth architecture and gives the VM a proven platform. No Forth requirement should force premature flash replacement.
2. **Specify the VM contract.** Record the supported Forth subset, 24-/16-bit profiles, primitive semantics, tokens, address spaces, stack limits, error behavior and platform callbacks. Select original implementation versus reuse only after a short source/footprint evaluation and a licensing decision.
3. **Build and test the host reference.** Bootstrap the small compiler; exercise arithmetic boundaries, nested calls/control flow, malformed input, bounded stacks and dictionary exhaustion. Run the applicable standard-word tests, plus project tests under both cell profiles. A subset must not be labeled full standard conformance.
4. **Deploy a RAM-only Forth payload.** Reuse the monitor's startup/console/loader. Demonstrate arithmetic, a new definition, loops, `.S`, diagnostics and restart over the real USB path. Record code/data/stack sizes, remaining arena, timings, build hashes and the full transcript. Agon VDP keyboard packets still require the board driver; `KEY` cannot assume raw ASCII on UART0.
5. **Make development persistent.** Add read-only SD source loading, a simple editor/source workflow, deterministic dictionary rebuilds and validated image loading. Add writes only with verified backup/recovery and interrupted-write handling. Keep a known-good boot image and serial escape path.
6. **Grow the small OS.** Add cooperative tasks, bounded event queues, devices/RAM filesystem, pipes and namespaces in separate increments. Measure contention and define ownership before sharing a writable dictionary among tasks.
7. **Prove the portability claim.** Choose a specific NMOS 6502 or 65C02 board, memory map and serial/block interface. Bring up its machine layer and 16-bit VM, run the same core and selected OS source tests, then measure whether an assembly engine is worthwhile. A second real port is the evidence for portability.
8. **Optimize and deepen self-hosting where useful.** Add optimized eZ80/6502 token engines, native compilation, a metacompiler or 9P/network services only against measured needs and the established contracts.

## Decisions for the next design discussion

The recommended default is a mostly-Forth OS over a small C/assembly substrate, a standard-C reference token engine, 24-bit logical Agon cells, 16-bit source-portability tests, cooperative tasks and source-first persistence. Those are proposed choices, not hardware measurements.

The decisions that materially affect implementation are whether existing GPLv3 code is acceptable for reuse, whether “native” also requires machine-code output immediately, which exact 6502-family CPU/board matters, and whether eventual self-hosting must include rebuilding the lowest machine layer. None blocks the already approved firmware preservation and C monitor deployment.

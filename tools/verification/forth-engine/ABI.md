# VIOS assembly VM engine evidence

`arch/ez80/forth/engine.s` is assembled twice. `--defsym TOKEN_ENGINE=1`
exports `_fvm_token_run`; the default exports `_fvm_direct_run`, `_fvm_docol`,
and `_fvm_direct_primitives`. The C compiler supplies the underscore prefix.
All VM globals, table entries and stack entries are native 24-bit cells.

`IY` caches IP; `IX` caches the next free data-stack address. Both C ABI
registers are restored on every trap, normal exit and error. All Forth return
addresses live on the bounded Forth return stack. The native stack holds only
fixed helper frames/temporary values; DOCOL immediately removes the native
CALL return address and uses it as the new thread IP. The private native-SP
snapshot permits bounds errors to unwind all helper frames safely.

Return values: 0 on empty-return-stack EXIT, 255 on error, `128+opcode` for
services. Traps publish IP/depths and leave operands for C handlers. DEFER
reads its inline cell address, reads the cell binding, pushes that binding,
then returns trap 163. MUL/DIV/MOD/LSHIFT/RSHIFT intentionally trap to the
shared C arithmetic implementation. EMIT/DOT/CR/KEY/EXECUTE trap too.

Both engines decrement the same native24 budget before every dispatch. DOCOL
itself consumes no additional dispatch: its threaded CFA entry already did.
Entry rejects invalid stack depths; errors already present remain present.
Data/return stacks hold 256 cells, and loop depth counts 32 limit/index pairs.
DO enters even when start equals limit. LOOP increments a 24-bit index and
leaves only when that index equals the limit. True is 0xffffff.

Ordinary stack/arithmetic/bitwise/compare/call/branch/loop/memory operations
stay inside assembly. This initial checked profile shares helpers and updates
depth globals as it executes. IP and data pointer are cached, while TOS is
memory resident. Every successful data/return push also compares its new native24 depth
against the corresponding high-water global and records a new maximum. These
checks preserve HL/DE and add 22 code bytes per push helper (44 bytes per
engine); both dispatch formats carry identical profiling overhead. Peaks
therefore include transient depths within a run and nested calls. This favors
small reliable code over maximum throughput;
hardware timings must be reported as this checked implementation profile.

Run `tools/verification/forth-engine/check.sh` from any directory. `AGONDEV`
may override the toolchain root; `PYTHON` may override the Python executable.
Generated objects, linked images, symbols and disassembly go only under
`build/verification/forth-engine/`. It assembles/links both variants and runs 142
semantic execution probes using the *actual linked disassembly and text/data
bytes*. Coverage includes signed24 extremes, wraparound/nested loops, native
CALL-DOCOL, stack bounds, byte/cell memory, traps/resumption, budget exhaustion,
IX/IY/SP preservation, and transient/monotonic high-water counters. The tiny Python interpreter is deliberately not a
cycle-accurate eZ80 emulator; real flag/ISA behavior and speed require separate
hardware differential tests. This verification script never opens hardware.

The global-storage fixture is probe-only and must not be linked into the
product. The Python interpreter executes the linked instruction semantics;
it does not independently decode the machine opcodes. Assembler/linker checks
and actual board execution remain complementary evidence.

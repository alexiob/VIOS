# VIOS multitasking: cooperative, tick-sliced or preemptive

Status: option B, stages B0, B1 and B2 implemented and measured on the board
(2026-10-20, see "B0 and B1 results", "B2 results" and "B5 results" at the end); B3 and B4 not started. Builds on the shipped
[background tasks, slice 1](2026-10-18-vios-tasks.md) and its
[acceptance](../../bring-up/vios-tasks-acceptance-2026-10-18.json) and
[slice sweep](../../bring-up/vios-task-slice-sweep-2026-10-19.json).
Related: [GUI toolkit](2026-10-19-vios-gui-toolkit.md),
[console](2026-10-19-vios-console.md), [handles](2026-10-18-vios-handles.md),
[timer](../../research/vios-timer.md), engine
[ABI](../../../tools/verification/forth-engine/ABI.md),
`CLAUDE_HANDOFF.md` sections 10 and 13.

## Question and short answer

The owner asked whether VIOS can do preemptive multitasking, what it would
take, and its pros and cons.

**Yes, but full preemption (option C) is a poor trade on this board.** The
recommendation is **option B**: keep tasks cooperative at the C level, and
add a PRT1 timer interrupt whose only effect is to make the inner engine's
*existing* budget check at `.Lnext` (`arch/ez80/forth/engine.s:87-95`) fail
at the next VM instruction. The dispatcher in `forth/system.c` then treats
that fault as a time-slice end and switches tasks at a VM safe point. Forth
code becomes preemptible at instruction boundaries, including the
foreground. Native primitives and C services stay atomic, so no driver has
to become reentrant. The hot dispatch path stays unchanged. The ESP/ZDI recovery path is not touched.

The reasons, each derived below:

- Under C, the longest non-preemptible regions are the same order of
  magnitude as B's slice: an SD sector over SPI, the 8 KiB checkpoint
  copies, the 1 ms console gate, a `platform_vdp_request` round trip. So C
  gains little worst-case latency in exchange for a full reentrancy audit of
  FatFs, the console transport, the text screen and every static buffer.
- C needs a native stack per task (FatFs runs on today's single 4 KiB stack)
  and a per-task copy of the engine's private `.Lnative_sp`
  (`engine.s:34,79,588`). That adds ~10 KiB or more against the ~37 KB left
  below the stack (handoff, Oct 19).
- B reuses `run_slice`, which already saves and restores a full VM context.
  It also reuses the budget fault, which the engines and the 142 engine
  probes already cover (`probe.py:145,159-160`).

## Where the system is today (option A, shipped)

`forth/system.c:80-113,276-391` implements slice 1:

- **Foreground** is REPL/`forth_eval` running on the VM globals with a
  1,000,000-instruction budget (`execute_index`, line 326). It is never
  preempted.
- **Four background tasks** (`FORTH_TASKS_MAX`, `include/forth/system.h:8`)
  run only at scheduling points: REPL idle (`forth_repl`, line 1059),
  foreground `KEY` waiting (line 309), `PAUSE` and `MS` (lines 494-499).
- **A slice is instruction-counted:** `fvm_budget = task_slice` (default
  1024, line 99). `FE_BUDGET` with `running>=0` ends the slice (line 289).
  This is already preemption at VM instruction boundaries, but only *among
  background tasks* and only *after the foreground offers time*.
- **Context switch is a copy.** `run_slice` (lines 348-373) saves the
  foreground's live cells, loads the task's, runs, saves the task's and
  restores the foreground's. Only live depth is copied. Task capacity is
  64/64/16 cells.
- **The console gate** `platform_console_quiet`
  (`boards/agon-light-2/platform_resident.c:144-151`) listens for ~1 ms with
  RTS asserted. If nothing arrives, it pauses RX for the slice. Without it,
  the 16-byte FIFO overran (handoff section 13).
- **Guards:** no scheduling while compiling (`current>=0`), during a
  transaction or observer, or when re-entering the scheduler (line 377).
  `ENGINE` and rollback are refused while tasks are live.

What section 13 lists as not done: per-task compiler/`BASE` context, locks,
task-safe module transactions. What slice 1 cannot do:

1. A long foreground word starves every task. At ~95k dispatches/s, the
   1,000,000 budget is ~10 s.
2. Wake-up latency is bounded by instructions, not time. The sweep's
   `10 MS` sleeper wakes 22.6/s at slice 2048 because one busy slice takes
   ~25 ms.
3. A foreground runaway cannot be interrupted by Ctrl-C. `forth_repl` reads
   input only between lines.

## Measured costs used throughout

| Quantity | Value | Derivation |
| --- | ---: | --- |
| Clock | 18.432 MHz, 54.25 ns/cycle | handoff section 4 |
| Timer tick (TMR0, /64) | 3.4722 µs | `docs/research/vios-timer.md` |
| VM dispatch, DIRECT checked profile | ~10.6-12.2 µs ≈ **195-225 cycles** | `docs/benchmarks/resident-2026-10-16.md`: BM-ARITH 7 dispatches/iter, 7.406 ms/100; BM-LOOP 3 dispatches/iter, 3.646 ms/100 |
| Cross-check | 2048 × ~12 µs ≈ 25 ms | matches "one busy slice takes ~25 ms" (tasks spec) |
| Budget check at `.Lnext` | 22 code bytes + 6 data bytes ≈ **~55 cycles** (~25% of a dispatch) | `engine.s:87-95`, 1 wait state on CS0 SRAM (MOS/recovery `CS0_BMC` setting) |
| Per scheduling round, slice 1 | **~1.7 ms**, of which ~1 ms is the gate | Fit of the sweep: loops/s = s / (k(R + s·t)). 128 → 4721/s and 4096 → 10234/s give kR = 15.1 ms and kt = 94 µs; with t ≈ 10.6 µs, k ≈ 8.9 instructions per loop and R ≈ 1.7 ms. Predicts 9.2k/s at 1024 (measured 8.8k). |
| UART0 byte time at 1,152,000 8N1 | 8.68 µs = **160 cycles** | 10 bits / 1,152,000 |
| UART0 FIFO from first byte to overrun | 16 bytes = 139 µs = **2,560 cycles** | eZ80F92 16-byte RX FIFO |
| Free RAM | ~37 KB between BSS end `0xA5E48` and stack bottom `0xAF000` | handoff section 13 |

These are figures for the checked/profiled engine on this board, not
general eZ80 numbers. Every figure marked "estimate" below is to be
measured in the acceptance runs.

## Stack addressing in the native engine

The engine addresses all three VM stacks at **fixed, link-time addresses**,
not through pointers:

- Data stack: `IX` caches the next free cell. It is computed **once at
  entry** as `_fvm_ds + 3·fvm_depth` (`engine.s:57-66`). After that, all
  data-stack traffic is `IX`-relative. ABI.md: "`IX` caches the next free
  data-stack address."
- Return stack: every `.Lrpop`/`.Lrpush` (lines 176-212) reloads the base as
  the immediate `ld de,_fvm_rs` (lines 189, 205). That covers `CALL`,
  `EXIT`, `DOCOL`, `>R`, `R>`, `R@`.
- Loop stack: `.Lpairaddr` uses the immediate `ld de,_fvm_loops` (line 239)
  for `DO`, `LOOP`, `I`, and the depth/bounds checks.
- Bounds are hard-coded to 256/256/32: entry compares with 257/257/33
  (lines 40-56), and push helpers test the high byte of the depth.
- C also indexes the arrays directly: `fvm_reference_run`, `capture`,
  `call_xt`, `native_j`, `(+LOOP)`, observer, checkpoints
  (`include/forth/vm.h` declares them as arrays).

So switching stacks has two possible costs:

| Approach | Per switch | Permanent cost |
| --- | --- | --- |
| **Copy live cells** (today) | Each switch copies live cells 4 times (foreground out, task in, task out, foreground in) at ~40-60 cycles per cell in the C `cells()` loop, plus ~16 scalar globals and two `error_text` copies. Typical (≤10 live cells per side): **~2-4k cycles ≈ 0.1-0.2 ms**. Worst case with a full foreground (256+256+2·32 = 576 cells = 1,728 B, in both directions): **~58k cycles ≈ 3.1 ms**. | None. Task capacity is limited to the saved 64/64/16. |
| **Pointer swap** | Swap 3 base pointers and ~11 scalars: **~300-500 cycles** in C, ~150 in assembly. No copying. | `ld de,_fvm_rs` becomes `ld de,(_fvm_rs_base)`: +1 code byte and +3 data reads, **≈ +8 cycles per return-stack or loop operation** (~4% on call-heavy code). The entry computes IX from a base variable (one-time). C users switch from arrays to pointers. ABI.md and the probes are updated. Each context needs full 256/256/32 stacks (**1,728 B**) unless the hard-coded bounds become per-task limits (+~20 cycles per push). |

The ~1.5 KB in the brief is the 256+256-cell data and return stacks
(1,536 B); with loop frames it is 1,728 B.

**Decision for B:** keep copying. At B's switch rate (about 100-250/s, see
below), copying costs ~0.2-1M cycles/s (1-5%) in the typical case. Move to
pointer swap only if measurement shows switches dominate. That is stage B4,
which can also place task stacks in the zero-wait 8 KiB on-chip SRAM.

## Global state inventory

Ownership for each option: **shared** means it is safe because only atomic
C code touches it; **swap** means saved and restored per context; **lock**
means one owner at a time; **refuse** means tasks are not allowed to use it.

| State (file) | Contents | A (today) | B | C |
| --- | --- | --- | --- | --- |
| VM registers (`forth/vm_reference.c:4-6`, `vm.h`) | `fvm_ds/rs/loops` (1,728 B), `depth, rdepth, ldepth, ip, error, budget, ds_peak, rs_peak` | swap (copy) | swap (copy). Budget gets a shadow, see below. | swap, plus IX/IY/SP and all registers |
| Engine private (`engine.s:588`) | `.Lnative_sp`, one per engine build | single (engine returns before any switch) | single (same reason) | **per task**. A second task entering the engine overwrites it and breaks the first task's fault unwind. |
| Native C stack (`ram.ld`, 4 KiB at `0xAF000`) | C frames, engine helper frames | single | single | **per task** |
| Compiler (`system.c:24-44`) | `words[512]`, `buckets`, code/data/source arenas, `controls[64]`, `leaves[16]`, `*_used`, `mark_*`, `source_map[640]`, `count`, `current`, `selected` | shared. Tasks cannot compile (`forth_eval` is not TASK-flagged). Scheduling is refused while `current>=0`. | same. The compiler stays a single critical section owned by the REPL. Ticks during compilation are ignored. | lock, or a per-task compiler (~600 B each for control/leave/marks), but the arenas are one shared bump allocator either way |
| INCLUDE (`kernel/vios_runtime.c:6-7`) | `include_source[4][4097]` (16 KiB), `include_depth` | foreground only | foreground only | lock (16 KiB per task is impossible) |
| Natives/hooks (`system.c:49-54,104-111`) | `natives[48]`, outer/idle/line/clock/gate/exit/io hooks | shared, read-only after init | shared | shared |
| Transaction/checkpoint (`system.c:55-79`) | `transaction_active`, `transaction_budget`, `have_activation`, two `struct checkpoint` (~9.1 KiB each: 8 KiB data plus 768 B stack), `test_data` 8 KiB, `activation_*` | foreground only. Tasks are refused SPAWN inside a transaction, and rollback is refused while tasks are live. | same. No switches while `transaction_active`. A tick sets the flag, the steal is ignored and the budget is restored. | lock. The 8 KiB checkpoint copy (~0.8M cycles) is a long critical section anyway. |
| Observer (`system.c:58-65`) | `observer_active`, `observer_budget`, `observer_before` (~0.9 KiB) | foreground only | same, no switches | lock |
| Error/crash (`system.c:46,140-146,154-170`) | `error_text[96]`, `crash` (~250 B), `executing`, `vm_message` | `error_text` is swapped by `run_slice`. `crash` is foreground-only (`running<0` test). | same | per task, or locked capture |
| TRACE (`system.c:101`) | `trace_left`, `trace_steps`, `tracing`; budget = 1 single-step | foreground only | foreground only. Ticks ignored while tracing. | foreground only |
| Task table (`system.c:83-112`) | `tasks[4]` (~0.7 KiB each), `foreground` (~1.85 KiB), `running`, `scheduling`, `yield_request`, `task_slice`, `next_task` | scheduler-owned | scheduler-owned. The ISR writes only `tick_pending`, the steal shadow and the ms clock. | scheduler-owned, DI sections |
| `BASE` (`system.c:45,121`; one `VARIABLE` cell in shared data space) | radix used by `.`, `U.` and number parsing | **shared** (tasks spec, "Sharing") | swap one cell per switch (~30 cycles), giving per-task `BASE` (a USER-variable of one) | swap |
| Console transport (`platform_resident.c:10-18,39`) | `rx` (vdp_transport), `output[128]`, `incoming[32]`, echo state `shown*`, `in_echo`, `foreign_output`, `idle_polls`, `console_tap` | shared; C atomic | shared; C atomic. The ISR never touches UART0 (stage B3 adds an RX ISR that only fills a byte ring). | lock. `poll()` and `vdp_transport_feed` are not reentrant. |
| UART0 driver (`arch/ez80/uart0.s`) | RTS state, `_ez80_uart0_rx_overruns` | shared | shared | lock |
| VDP request/reply (`platform_vdp_request`, `platform_host_request`) | synchronous command and reply on the same UART, up to a 1 s timeout | atomic native (long) | atomic native. This is the longest non-preemptible section; see latency. | lock. One outstanding request at a time. |
| VDU byte stream | multi-byte VDU commands built from several 1-byte `VDU` calls (`vios_services.c:43`) | **already not atomic** across task slice boundaries | not atomic across foreground ticks either. Rule: render via `VTYPE` of a prebuilt buffer (one native = one atomic send), or inside `CRITICAL( … )CRITICAL`. | same, plus a lock |
| SD/FatFs (`boards/agon-light-2/storage.c:10-14`, `drivers/fatfs`, `drivers/sd-spi/sd.c:8-10`) | `volume`, `file`, `streams[]`, `FatFs[]`, `DirBuf`, `LfnBuf`, `FF_FS_REENTRANT 0`, `FF_FS_LOCK 0` | atomic natives | atomic natives | **mutex** (FatFs reentrant mode needs OS sync hooks) plus a per-task stack for LFN frames |
| Services scratch (`kernel/vios_services.c:8,18`) | `file_data[4096]`, `clock_ms/ticks/last` | atomic | atomic. The ISR's ms counter replaces the polled `task_clock`. | lock |
| Handles/pipes (`kernel/vios_handles.c:13-33`) | `handles[]`, `pipes[]`, `snapshot` and owner, `host_reply[240]`, `last_error`, `devices[]` | atomic natives, owner field per task | same | lock |
| Text screen (`drivers/agon-vdp/text_screen.c:6-16`) | `desired`/`painted` planes, dirty rows, scroll queue, stats | single owner: the idle painter | same. Painting stays C and bounded (≤128 B per batch). | lock |
| Workbench (`kernel/workbench.c`, `workbench_runtime.c:9-13,94-100`) | `wb`/`view`, row buffers, `paint_xt`, console view `con_ring`/`con_shadow` (4.6 KiB)/cursor | foreground. `WB-PAINT-DEFAULT` is flags 0, so it is refused in tasks. | same | lock |
| libc/compiler temporaries | none found in the files above | — | — | audit needed |

The pattern: under A and B, every row except the VM registers, `BASE` and
`error_text` stays as it is, because preemption can only happen between VM
instructions, and no VM instruction is in the middle of a C service then.
Under C, nearly every row becomes a lock or a per-task copy.

## Option A: complete the cooperative design (PAUSE)

Keep the slice 1 model and finish section 13's list:

- `PAUSE` in long foreground words. Optionally a `PAUSE` every N `LOOP`
  iterations, compiled by a flag. That changes code size and benchmarks, so
  it is off by default.
- Per-task `BASE` (one-cell swap), per-task error text of 96 bytes (now 48).
- A compiler lock instead of a per-task compiler; INCLUDE stays foreground.
- `CRITICAL( … )CRITICAL` is unnecessary in A for task-versus-task (slices
  are instruction-counted, so a programmer can reason about them) but helps
  VDU sequences.

| Cost | A |
| --- | --- |
| Hot path | unchanged |
| RAM per task | ~0.7 KiB (today) + 3 B `BASE` + 48 B error = **~0.75 KiB**; foreground save 1.85 KiB once |
| Switch | ~2-4k cycles typical (copy) + gate ~1 ms per round |
| Latency | instruction-bounded; foreground starvation unbounded until it calls `PAUSE`/`KEY`/`MS` |
| Engine probes / differential | no change |
| Risk | lowest; already accepted on the board |

**Pros:** no interrupts, deterministic, zero engine change.
**Cons:** it does not answer the owner's question. A foreground loop
without `PAUSE` still freezes tasks, rendering and Ctrl-C.

## Option B (recommended): cooperative plus timer-driven slicing at VM safe points

### Mechanism

1. **PRT1 tick.** Timer 1 runs at /64 with reload 288 for **1 kHz** (reload
   4800 gives 60 Hz). `IRQ_EN` is set, and its interrupt is vector `0Ch`
   (eZ80F92 PS015317 Table 11). TMR0 stays the free-running 16-bit
   measurement timer, untouched (`timer.s`). Init must also clear
   `CLK_PPD2` bit 1, `PRT1_OFF` (PS015317 p38, Table 5; `timer.s` clears
   only bit 0), and set `TMR_ISS` bits 3:2 for the system clock.
2. **Vector path.** VIOS has no `ei` anywhere, and `start.s` executes `di`.
   On the F92, the vector address is `{I[7:0], IVECT}`, which lies in the
   low 64 KiB, that is MOS's flash. MOS's path is flash `__vector_table`
   (ALIGN 100h), then flash `__1st_jump_table` (`JP.LIL`), then the RAM
   `__2nd_jump_table` in segment IVJMPTBL
   (`docs/research/sources/agon-mos/src_startup/vectors16.asm:179-241`).
   MOS's RAM is `0BC000-0BFFFF` (`MOS.zdsproj:145`), outside the VIOS
   payload `0x40000-0xAFFFF` and above the VIOS stack (`0xAF000-0xB0000`).
   VIOS hooks PRT1 by rewriting one 4-byte `JP` entry in that RAM table,
   with interrupts disabled, exactly as MOS's own `_set_vector` does. The
   eZ80 flash is not written. Before the first enable, a **read-only ZDI
   probe** must confirm `I`, the IM 2 mode, the flash entry at
   `{I,0Ch}`, and the resolved RAM slot. Do not hard-code an address.
   Before the first `ei`, VIOS also points **all 48 RAM slots** at its own
   handler: `reti` for PRT1/UART0 and a counter-incrementing `reti` for the
   rest. A stray source (an SPI, GPIO or RTC interrupt, or a UART `IER` bit
   left set) then cannot jump into MOS's default handlers. The RAM table is
   valid only because MOS initialized it during this boot. A ZDI cold load
   that bypasses MOS init must fill the whole table itself, or leave
   interrupts off.
3. **ISR** (assembly, ~20 instructions; preserves everything it uses, does
   not touch IX/IY or the alternate registers):
   - acknowledge by reading `TMR1_CTL`;
   - `ms_clock += 1` (24- or 32-bit). This replaces the polled
     `task_clock`, which loses time across >227 ms gaps
     (`vios_services.c:16-21`);
   - if `preempt_armed` and a deadline is due (slice end, or a sleeper's
     wake time ≤ now), set `tick_pending = 1`. If `fvm_budget != 0`, set
     `budget_stolen = fvm_budget` and then `fvm_budget = 0`.
   Each 24-bit `ld (Mmn),hl` is one instruction and therefore atomic with
   respect to interrupts.
4. **Engine:** unchanged. The next `.Lnext` sees budget 0 and faults with
   `FE_BUDGET` without fetching. ABI.md: "A budget fault stops before
   fetching, so IP names the next instruction". If the CPU is in C at tick
   time (a native, a trap handler), the fault fires at the first dispatch
   after the C code returns, which is again a safe point.
5. **Dispatcher** (`dispatch()` in `system.c:277`): before the existing
   bookkeeping, `transaction_budget=fvm_budget` and
   `observer_budget=fvm_budget` (lines 287-288), apply this rule:
   ```
   if fvm_error == FE_BUDGET and tick_pending:
       fvm_error = 0; fvm_budget = budget_stolen; tick_pending = 0
       if switching allowed: switch (background: return 2 as today;
                                     foreground: run one scheduler round, then continue)
       else: continue     # tick ignored
   ```
   Switching is allowed only when not compiling, not in a transaction, not
   in an observer, not tracing, not inside the scheduler, and the critical
   section count is 0. These are the existing guards at `system.c:377`,
   plus `CRITICAL`. Real exhaustion (`budget_stolen == 0`) keeps today's
   meaning: the runaway limit for the foreground, the slice limit in
   deterministic mode. TRACE's `budget=1` stepping keeps working because
   ticks are not armed while `tracing`.

### The race and its fix

`.Lnext` is a non-atomic read-modify-write: `ld hl,(_fvm_budget)`, then
`ld de,0`, `or a`, `sbc hl,de`, `ld a,9`, `jp z`, `dec hl`, then
`ld (_fvm_budget),hl`. An interrupt between the load and the store has its
zero overwritten with `old-1`. The window is 14 code bytes, **~30 cycles of
a ~200-225-cycle dispatch, so ~14% of steal attempts are lost.**

- **Chosen: retry on every tick.** The ISR keeps stealing on each tick
  while `tick_pending` is set. A lost steal costs one more tick. At 1 kHz
  the expected extra delay is ~0.16 ms. The chance of two consecutive
  misses is ~2%, and of three, ~0.3%. A successful steal restores the exact
  budget, because each steal overwrites `budget_stolen` with the current
  value. The one stale case is a lost steal followed by *natural*
  exhaustion before the next tick, which would restore the earlier
  `budget_stolen`. That needs ≤~90 instructions of real budget left at
  steal time (1 ms ≈ 90 dispatches), so it is practically unreachable with
  a 1,000,000 runaway limit. The dispatcher clears `budget_stolen` on every
  restore and treats `budget_stolen == 0` as real exhaustion. Engine code,
  ABI.md and all 142 probes stay as they are.
- *Alternative, exact:* the ISR checks whether the interrupted PC lies in
  either engine's `[.Lnext, store]` window and, if so, rewinds it to
  `.Lnext`. This is idempotent because nothing was stored yet. It costs ~40
  ISR cycles and exports four labels. Use it only if the measured lost-tick
  rate matters for 60 Hz jitter.
- *Rejected:* a separate `tick` flag test in `.Lnext` (`ld a,(flag)`,
  `or a`, `jp nz`). That is 9 bytes + 1 read ≈ **~20 cycles ≈ 9% on every
  dispatch, permanently**, plus engine/ABI/probe changes. It is exact, but
  it violates "keep fast single-task execution".

### Scheduling policy

- **Foreground preemption.** On a tick while the foreground runs Forth, the
  dispatcher calls `forth_tasks_run()` for one round. `run_slice` already
  saves and restores the foreground. Then the foreground continues. The
  foreground is the REPL's current evaluation, so the REPL C loop stays as
  it is. The foreground has **no time slice of its own**. For the
  foreground, the ISR sets `tick_pending` only when a task is READY or a
  sleeper's wake deadline has passed, and at most every `fg_quantum_ms`
  (default 4 ms). Without that rule, every foreground tick would pay the
  ~1 ms gate listen (below). A foreground with only sleeping tasks
  therefore runs uninterrupted between their deadlines.
- **Time slices.** A background slice ends at `slice_ms` (default 4 ms,
  about 350 instructions at 11 µs) or at the first wake deadline of a
  higher-priority task. The instruction budget stays as a secondary bound
  (`TASK-SLICE`), and it is the only bound in deterministic mode.
- **Priorities.** Two classes: `frame` (woken by a frame deadline; preempts
  busy tasks and the foreground at the next tick) and `normal`
  (round-robin). Console and host control stay above both. The scheduler
  round still begins with the gate.
- **Ctrl-C and hotkeys.** Each foreground preemption round already passes
  through the gate's `poll()`, which feeds received bytes into the `rx`
  transport, but nothing reads them until the next prompt. So B2 adds a
  transport function, `vdp_transport_take_interrupt(&rx)`. It scans the
  pending *host* bytes for `3`, removes only that byte, and leaves all other
  input queued. The physical keyboard already has `rx.hotkey`. A hit sets
  an abort request. The dispatcher turns it into `fail("interrupted")` at
  the same safe point. This finally makes `BEGIN AGAIN` at the REPL
  recoverable without the 10 s budget. Framed harness requests are not
  scanned: a `3` inside a request line is data.
- **`CRITICAL( ... )CRITICAL`.** These words maintain a C counter. Ticks
  are ignored while it is non-zero, with the remaining real budget as the
  bound, so a forgotten `)CRITICAL` cannot hang the system. Use it for
  `@ 1+ !` on shared variables and for multi-call VDU sequences.
- **Long natives.** Worst-case jitter equals the longest native. `FILL` and
  `MOVE` over 8 KiB take ~100 cycles/byte through the checked accessors,
  about 44 ms. B makes them resumable in 256-byte chunks, using the `TYPE`
  rewind pattern at `system.c:469-478`. `platform_vdp_request` (bounded by
  a 1 s timeout, typically one VDP round trip) and SD I/O stay atomic and
  are listed in the jitter budget.

### Costs

| Cost | B |
| --- | --- |
| Hot path | **0 cycles** (engine unchanged) |
| ISR | estimate 150-250 cycles (8-14 µs) including MOS's two jump hops in flash. At 1 kHz that is **0.8-1.4% CPU**; fall back to 250 Hz if measured above 2%. |
| Switch | copy path as in A: ~2-4k cycles typical. At 60 Hz frame wakes plus 4 ms slices (~120 + ~250 switches/s with one busy task) ≈ **0.7-1.5M cycles/s ≈ 4-8%** worst case. That is the trigger for stage B4 pointer swap (~0.5k cycles/switch, under 1%). |
| Gate | today ~1 ms of listening per round. Under B, a round happens only when a task is due (foreground rule above), never per tick. A 60 Hz render task beside a computing foreground costs **~60 gates/s × 1 ms ≈ 6% CPU**. A busy background task pays one gate per `fg_quantum_ms`, so up to ~20% of the foreground's time while it competes (the intended share, which is mostly the task's own slice; the gate part is ~1 ms of each 4 ms round). Stage B3 removes the listen, leaving a few-cycle ring check. |
| RAM | per task: A's ~0.75 KiB + priority/deadline/steal fields ≈ **0.76 KiB**. Globals ≈ 20 B. ISR code ~80 B. B4 pointer swap: **1.73 KiB per context** (5 contexts ≈ 8.6 KiB, or 6.9 KiB of on-chip SRAM for the 4 tasks). |
| Engine probes | unchanged. Add a steal probe (below). |

### Interrupt latency against UART0 RX

- Today **no UART interrupt exists**. Correctness comes from RTS: while C or
  VM work runs, RX is paused (MCTL bit 1 clear, `uart0.s:78-87`), and the
  peer obeys it. The danger case is work done while RTS is asserted (the
  FIFO overruns 139 µs after the first byte, handoff section 13). The tick
  ISR never touches MCTL, so it cannot create that case.
- The ISR adds **8-14 µs ≈ 1-2 byte times** to anything it interrupts. In
  B, interrupts are disabled only inside the ISR and the vector patch.
  Scheduler bookkeeping does not need DI, because the ISR writes only
  `tick_pending`, `budget_stolen`, `fvm_budget` and `ms_clock`, each with a
  single store. **Worst-case maskable latency ≈ one ISR ≈ 250 cycles**,
  well below the 2,560-cycle FIFO margin and the 1,280-cycle margin at an
  8-byte trigger level.
- The latency that users notice is **scheduling latency**: how long RTS
  stays paused before the REPL polls again. In A, that is one
  instruction-counted slice (~25 ms at 2048, ~11 ms at 1024). In B it is at
  most `slice_ms` plus the longest native, so it is time-bounded.
- **Stage B3, the RX ISR.** A UART0 ISR (vector `18h`, trigger level 8)
  only moves FIFO bytes into a 256-byte ring and deasserts RTS when the ring
  is ¾ full. It does no parsing: `vdp_transport_feed` and `poll()` stay in
  task context, because the transport parser is not reentrant. RTS can then
  stay asserted during slices, which **removes the 1 ms gate listen**: the
  gate checks `ring non-empty || key events` in a few cycles. Budget: about
  30 cycles/byte inside the ISR at 115 kB/s, so ≤3.5M cycles/s at full
  line rate (≤19% during a burst, near 0 idle). Bursts are bounded by host
  frames. The ESP's RTS reaction (≤1-2 bytes in flight) must be measured.

### Differential tests

- **Engine probes** (`tools/verification/forth-engine/check.sh`, 142
  probes): unchanged, because the engine is unchanged. Add three probes per
  engine: (1) set `budget=0` at every instruction boundary of a probe
  program, clear and resume, and require the same final state as an
  uninterrupted run; (2) inject the store-overwrite race (write 0 between
  the load and the store) and require that execution continues correctly
  with `budget=old-1`; (3) a steal while inside a trap returns the fault at
  the next entry. The Python interpreter executes the linked instructions,
  so (2) models the real window.
- **Host reference** (`tests/forth_tasks_host.c`, `tests/test_forth_tasks.py`):
  ticks are off by default, so existing exact-slice assertions stay valid
  (`"slices 10 steps 10240"` at line 34 and `"steps 256 "` at line 77). Add
  a deterministic tick mode: a host hook "preempt every k instructions"
  that drives the same `tick_pending`/steal path. Seeded random k gives:
  (a) single-task programs produce identical results for every schedule;
  (b) two tasks with `CRITICAL` counters never lose an increment; (c)
  `CRITICAL`-free `@ 1+ !` *can* lose increments (documented, not a bug).
- **Schedule replay.** On the board, record for each switch
  `(context, steps)`. The host replays the same instruction counts on the
  reference VM and compares task outputs. This keeps C-reference
  differential testing meaningful even though hardware ticks are not
  deterministic.
- **Benchmarks.** `BENCH` with ticks disarmed must equal today's table.
  With ticks armed, the slowdown should be about the ISR share (≤1.5%).

## Option C: full preemption (switch the native stack anywhere)

Mechanism: the PRT1 ISR saves AF, BC, DE, HL, IX, IY, AF', BC', DE', HL'
and SP onto the current task's native stack, stores SP in the task record,
picks the next task, loads its SP, and restores.

| Cost | C |
| --- | --- |
| Hot path | 0 (no checks), but... |
| Switch | 10 pushes/pops of 24-bit registers (~7 cycles each at 1 WS) + SP + `.Lnative_sp` + VM global swap. **~500-1,000 cycles with pointer swap**, ~3k+ with copying. All of it with DI. |
| RAM per task | native stack 1.5-2 KiB (estimate; FatFs LFN frames, `vtype`'s 64-byte buffer, `file_read` path, printf-free but deep `forth_eval` frames: measure by stack painting) + VM context 1.73 KiB (pointer swap) ≈ **3.2-3.7 KiB**. 4 tasks + REPL ≈ **16-19 KiB of ~37 KB free**. |
| Reentrancy | everything in the inventory marked lock: FatFs (`FF_FS_REENTRANT 1` and OS sync hooks), console transport, VDP request/reply, handles, text screen, workbench, checkpoints, error capture. Every multi-step C service needs a mutex and every mutex needs a priority-inversion policy. |
| Latency | DI context switch ≈ 1k cycles (~6 bytes of UART time). Worst-case *task* latency is still bounded by the longest locked region (an SD sector, the 8 KiB checkpoint copy at ~0.8M cycles ≈ 44 ms, a VDP request), so it is no better than B in practice. |
| Differential | preemption inside C natives cannot be reproduced on the host reference. Differential tests stay meaningful only for single-task runs. Correctness needs randomized-interrupt stress on hardware. The engine probes need a model of switches inside helper frames (they assume `.Lnative_sp` is single). |
| Natives | `FORTH_NATIVE_TASK`'s "runs to completion, atomic" guarantee goes away; Forth code relying on atomic natives (`TYPE` output not interleaving, `SPAWN` slot allocation) needs review |

**Pros:** a C service can be preempted, which would matter for long
compute in C. VIOS deliberately has almost none: it moves policy into
Forth.
**Cons:** a large reentrancy refactor, a native stack per task, the
largest RAM use, nondeterministic failures that are hard to reproduce over
ZDI, and little latency gain over B because the long sections become
locks.

## Comparison

| | A cooperative | **B tick-sliced** | C preemptive |
| --- | --- | --- | --- |
| Foreground runaway interruptible | no | **yes, at the next VM instruction** | yes |
| Wake/frame jitter | instruction-bounded, ~25 ms at 2048 | ≤1 tick + the longest native (target ≤5 ms) | ≤ DI switch + the longest lock |
| Hot dispatch path | 0 | **0** | 0 |
| ISR overhead | 0 | 0.8-1.4% at 1 kHz | ~1k cycles/switch, ≥2-5% |
| Console gate cost | ~1 ms per round | ~6% with a 60 Hz task beside a busy foreground, until B3 | gate replaced by locks |
| RAM per task | ~0.75 KiB | ~0.76 KiB (1.73 KiB with B4) | ~3.2-3.7 KiB |
| Drivers to make reentrant | 0 | 0 | all |
| Engine/ABI change | none | none (B4 optional) | `.Lnative_sp` per task, probes |
| C-reference differential | unchanged | unchanged with ticks off; replay with ticks on | single-task only |
| ESP/ZDI recovery | untouched | untouched | untouched, but harder to diagnose |

**Recovery independence (all options).** ZDI halts the CPU regardless of
`IEF1`. Nothing here uses GPIO26/27, changes ESP firmware, or alters the
ESP's `vios-usb` control task. `_platform_restart` and `_start` already
begin with `di`, so a ZDI-reloaded image cannot take a stale PRT1
interrupt before VIOS re-patches its vector. `platform_timer_init` must
also clear PRT1's `IRQ_EN` before the patch. The handoff's recovery
technique still works: a ZDI `memory_write` of task state, then
`resync`. `ms_clock`, `tick_pending` and `preempt_armed` are additional
symbols to read from the exact flashed ELF.

## Staged plan

Each stage is one reviewable change with host tests first and an on-board
acceptance record. Nothing is flashed without the owner's go-ahead.

| Stage | Work | Acceptance |
| --- | --- | --- |
| **B0, probe (no behavior change)** | Read-only ZDI probe of `I`, IM mode, the `{I,0Ch}` and `{I,18h}` flash entries, the MOS RAM jump table (48 slots), and the current `TMR1_CTL`/`CLK_PPD2`/`UART0_IER` values. Measure `cells()` cycles/cell, run_slice fixed cost and the gate with TMR0. | Recorded addresses match MOS sources. Cycle figures replace this document's estimates. |
| **B1, tick and clock** | Fill all 48 RAM vector slots with VIOS handlers, then the PRT1 1 kHz ISR: acknowledge + `ms_clock` only. `MS`, `MILLIS`-style deadlines use it. `TICKS` diagnostic word. `preempt_armed` stays 0. | Engine `BENCH` within noise of the 2026-10-16 table. ISR cost measured (≤1.5%). 30-minute soak: `console-dropped` unchanged, no USB frame loss beyond the known issue #2 baseline. `MS` accurate across 1 s foreground runs (today's polled clock loses wraps). |
| **B2, first vertical slice: 60 Hz render task + REPL** | Budget steal, the dispatcher rule, foreground preemption, frame priority, `FRAME-WAIT` (sleep to the next 16.667 ms boundary on `ms_clock`), `CRITICAL( )CRITICAL`, Ctrl-C at preemption points. The render task is plain Forth using TASK-flagged natives only. It draws a frame counter and a moving bar in a reserved screen corner with **one `VTYPE` per frame** of a prebuilt VDU buffer (TAB, colour, text, colour restore), so each frame is one atomic send and does not split console VDU sequences. Workbench painting stays the C idle painter (`WB-PAINT-DEFAULT` remains foreground-only, flags 0), consistent with the GUI spec's "handlers do not run in background tasks". | (1) With the render task running, `1 2 + .` REPL round trip median within 5 ms of the no-task baseline (77.6 ms, tasks spec). (2) Render task reaches ≥58 frames/s for 10 minutes while idle, while a busy task runs, and while the **foreground** runs `: SPIN BEGIN AGAIN ; SPIN`. In the `SPIN` case, the measured `SPIN` throughput loss is ≤ 8% (≈6% gate + ISR + switches) and is reported. (3) Frame-start jitter p99 ≤ 5 ms (TMR0 timestamps in a ring read by `TASK-OUT`). The busy task here is a Forth-only loop with no large `FILL`/`MOVE`, `FILE-*` or `PIXEL@`/`CHAR@` calls: those natives are atomic (up to ~44 ms) until B5 chunks them, and their jitter is measured separately. (4) Ctrl-C (console and host) stops `SPIN` within 50 ms, and the prompt returns with the stacks reset. (5) Transactions, observer, TRACE, `ENGINE` and rollback guards still refuse or ignore ticks (host tests plus one board check each). (6) Zero `console-dropped` growth over the run. (7) All existing host task tests pass unchanged. New deterministic-tick tests and the three engine probes pass. |
| **B3, RX ISR and gate removal** | UART0 RX ISR into a ring with RTS high-water. The gate becomes a ring check. | Slice sweep re-run: small slices no longer lose throughput to the 1 ms listen. REPL latency under two busy tasks ≤ baseline + 2 ms. 45-minute soak without overruns. |
| **B4, optional, pointer swap** | Only if B2 measurements show switching above 3% CPU: base pointers in `engine.s`, C arrays to pointers, ABI.md and probes updated, task stacks in on-chip SRAM. | Engine probes green; benchmark delta reported (expected ≤4% on call-heavy kernels); switch cost ≤600 cycles. |
| **B5, task-safe services** | Per-task `BASE` swap, 96-byte task error text, chunked `FILL`/`MOVE`, compiler lock (allows `forth_eval` from at most one task at a time), and later a task-safe module transaction (activation only; rollback still refused while tasks run). | Host tests for each. `TASKS` shows per-task `BASE`. |

Option A remains the fallback at every stage: with `preempt_armed = 0`,
VIOS behaves exactly as slice 1.

## Open questions for the owner

1. Is the tick rate 1 kHz (better `MS` resolution and slice granularity)
   or 250 Hz (a quarter of the ISR cost)? The recommendation is 1 kHz,
   decided by the B1 measurement.
2. Should Ctrl-C at preemption points be enabled by default? It changes
   REPL semantics for long words; the recommendation is yes.
3. Should B3 (RX ISR) follow B2 directly, or wait for the GUI toolkit's
   Stage 1 console changes, which also touch the console path?


## B0 and B1 results (2026-10-20)

Owner's go-ahead: "Fully implement 1-5" (this design was item 5). Open
questions answered with the recommendations: **1 kHz** tick (B1 measured its
cost); Ctrl-C at preemption points and the B3 ordering belong to B2/B3.

**B0 (read-only, `IRQ-PROBE`).** No ZDI wire is needed: since the untethered
boot, MOS always runs first, so the probe runs on the CPU
(`kernel/vios_irq.c`, `docs/bring-up/vios-irq-probe-2026-10-20.txt`):
`I` = 01 (flash table 000100h), RAM 2nd jump table at **0BCB15h**, 45 of 48
slots MOS's default (001937h); MOS handlers on IVECT 18h (UART0), 1Ch (UART1)
and 32h (PB1 — the VDP vertical blank). UART0/1 `IER` = 0, PRT1 unused and
clocked (`TMR1_CTL` 00, `CLK_PPD2` 00, `TMR_ISS` 00), **PB1 still a
rising-edge interrupt** (`PB_ALT1`/`ALT2` bit 1 set).

**B1 (`vios_tick_start`, `arch/ez80/irq.s`).** Only when the stub booted VIOS
(`VIOS_BOOT_MAGIC`) and the chain is exactly MOS's layout: PB1 made a plain
input, UART IERs cleared, all 48 RAM slots pointed at VIOS (PRT1 → the tick,
the rest → a counting `reti.l`), PRT1 at /64 reload 288 = 1 kHz, then `EI`.
The ISR acknowledges PRT1 and increments a 32-bit `ms_clock` (AF and HL, 9
bytes of stack). The task clock (`MS`, deadlines) uses it instead of the
polled TMR0 clock. New words `TICKS` and `IRQ-STATS`; `preempt_armed` does not
exist yet (no preemption). On the board:

- `TICKS` vs the ESP clock over a 10 s `MS`: **10001 / 10001 ms**.
- Stray interrupts: **0**.
- `BENCH` against the same code without the tick (`firmware-main-8`):
  **+0.34 / +0.33 / +0.37 %** mean for engines 0/1/2 (max +0.63 %), target ≤1.5 %
  (`vios-bench-no-tick-main8-2026-10-20.txt`, `vios-tick-b1-2026-10-20.txt`).
- Boot unchanged (`BOOT-TIMES` 2,502 ms); the firmware confirmed on trial.
- 30-minute soak: see `vios-soak-tick-b1-2026-10-20.json`.

## B2 results (2026-10-20)

Implemented as specified, with these choices:

- **Planner instead of ISR deadlines.** `forth/system.c` plans a delay and
  the ISR only counts it down: 4 ms while a background slice runs; for the
  foreground, 4 ms while a task is ready, the next sleeper's wake time, or
  50 ms for the Ctrl-C check while a command runs. When it expires the ISR
  steals a non-zero `fvm_budget` and repeats on every tick until the next
  plan (the retry fix for the store race).
- **Dispatcher rule** as specified; ignored inside `CRITICAL( )CRITICAL`,
  compiling, transactions, observers and TRACE (`ignored` counter).
- **Board-found race, fixed:** a steal during a scheduler round's gate left
  the foreground with budget 0 ("execution budget exhausted"). The
  dispatcher now disarms before a round and `run_slice` reclaims a pending
  steal before saving the foreground. The host test reproduces it.
- **Ctrl-C:** the transport sets an interrupt flag for a Ctrl-C key, or a
  host line starting with byte 3, only while a command runs (at the prompt
  it still just clears the line); the interrupting byte is not also input.
  `platform_console_interrupt` opens RX for up to 0.25 ms (RX is paused
  while a command runs). Foreground `MS`/`FRAME-WAIT` waits poll it too.
- **`FRAME-WAIT`** sleeps to the next 50/3 ms boundary (rounded up).
- New words: `PREEMPT ( f -- )`, `PREEMPT-STATS`, `CRITICAL(`, `)CRITICAL`,
  `FRAME-WAIT`.

Board (`firmware-main-14`; `vios-preempt-b2a-2026-10-20.txt`,
`vios-ctrlc-frame-b2b-2026-10-20.txt`):

- A 1.04 s foreground word beside a busy task: ~2.55 s and the task
  progresses (no preemption: 1.04 s and the task starves). The share follows
  the 4 ms quantum + 4 ms slice + ~1 ms gate listen that B3 removes.
- Ctrl-C stops `BEGIN MILLIS DROP AGAIN` in **66 ms** and `600000 MS` in
  **25 ms** (both including the harness and ESP round trips); the REPL is fine.
- Render task (`BEGIN FRAME-WAIT ... AGAIN`): **60.1 / 60.0 / 60.0 frames/s**
  idle, beside a busy task, and while the foreground computes.
- REPL round trip `1 2 + .`: 46–50 ms idle; ~110 ms beside a busy task (gate).
- 0 stray interrupts, 0 ignored ticks; GUI acceptance passes; 30-minute
  soak: `vios-soak-b2-2026-10-20.json`.

Next: B3 (UART0 RX ISR into a ring; the gate becomes a ring check, which
removes the 1 ms listen per round and the ~110 ms REPL latency beside a busy
task), B5 (task-safe services), B4 only if switching cost matters.


## B5 results (2026-10-20, host-tested; board check pending)

- **Per-task `BASE`:** one cell per task, swapped around each slice; a new
  task starts in its spawner's radix; `TASKS` lines end with `base N`
  (`vios_task_client` parses it, optionally). +3 bytes per task.
- **Chunked `FILL` / `MOVE`:** at most 256 bytes per run; the native pushes
  its remaining arguments back and re-runs itself (the `TYPE` pattern), so a
  tick can preempt between chunks. `MOVE` keeps memmove semantics (a
  backward copy does the last chunk first). Host test: sizes 0–1000 with
  overlap both ways, byte-equal to `memset`/`memmove`, under ticks every 7
  instructions.
- **Not done, on purpose:** the 96-byte task error text (would cost 192 bytes
  of RAM for longer messages; the owner ranks memory first, 48 bytes stay);
  the compiler lock (no task can evaluate source today: `LOAD`/`INCLUDE` are
  REPL commands, not task words); the task-safe module transaction.

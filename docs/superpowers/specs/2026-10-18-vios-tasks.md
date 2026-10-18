# VIOS background tasks (Stage 2, slice 1)

Goal: two or more Forth computations make progress while the REPL, harness
control and the workbench stay responsive, without slowing ordinary
single-task execution or changing the native engines' dispatch code.

## Model

- **Foreground** is the existing REPL/`forth_eval` execution using the VM
  globals directly. Its semantics, 1,000,000 instruction budget and speed are
  unchanged. It is never preempted.
- **Background tasks**: `TASKS_MAX` = 4 slots, each with a saved VM context.
  They run only at defined scheduling points and only when no compilation,
  module transaction or observer is active:
  1. REPL idle (no partial input line, not compiling), after the idle painter;
  2. foreground `KEY` while no input byte is available (replaces the busy wait);
  3. foreground `PAUSE`, and foreground `MS` while it waits.
  Console/control priority follows: any received input byte or command line
  ends background work at the next slice boundary.
- **Time slicing by budget exhaustion.** Both native engines and the C
  reference test the budget before fetching an instruction and publish IP and
  depths on that fault; IP is not advanced. A background slice runs with a
  budget of `TASK_SLICE` (2048) instructions; `FE_BUDGET` there is cleared and
  treated as preemption at an instruction boundary. This departs from purely
  cooperative yield points deliberately: it needs no engine change and a
  runaway task cannot starve the system. A native verification probe checks
  clear-and-resume for DIRECT and TOKEN before relying on it.
- **Sharing.** Tasks share the dictionary, data space and `BASE`. Each VM
  instruction is atomic with respect to switches; read-modify-write sequences
  such as `@ 1+ !` are not. Slice 1 provides no locks: use one writer per
  variable, or `PAUSE`-free critical sections shorter than the slice are
  *not* guaranteed. Natives run to completion and are atomic.

## Context and memory

Per task: data stack 64 cells, return stack 64 cells, 16 loop pairs, IP,
budget, depths, peaks; state, xt, slices, executed instructions, wake time,
48-byte error text and a 128-byte output ring with a drop counter. Exceeding a
saved capacity at a switch fails that task ("task stack exceeds saved
capacity"); foreground limits are unchanged. Switching copies only live cells.
The foreground save area holds full 256/256/32-pair stacks. Budget: about
0.7 KiB per task plus 1.7 KiB foreground save, ~4.5 KiB BSS total of the
~87 KiB gap above BSS end `0x99B04`. The build reports the exact delta.

## Words and commands

| Word | Effect |
| --- | --- |
| `SPAWN ( xt -- id )` | Start xt in a free slot (1..4); fails if none, inside a transaction or observer. |
| `PAUSE ( -- )` | Background: yield. Foreground: run one round of ready tasks. |
| `MS ( u -- )` | Background: sleep u ms. Foreground: wait u ms running tasks. |
| `TASK-KILL ( id -- )` | Stop a task; its record stays inspectable until the slot is reused. |
| `TASK-STATE ( id -- n )` | 0 free, 1 ready, 2 sleeping, 3 done, 4 failed, 5 killed. |
| `TASKS` | Outer command: one line per slot with state, xt name, slices, steps, depths, peaks, output bytes/drops and error. |
| `TASK-OUT id` | Outer command: drain the task's output ring as hex. |

Background `EMIT`/`.`/`CR` write to the task's ring, never to the shared
console stream (which would land inside the next host frame). `KEY` fails the
task. Natives are allowed only when flagged `FORTH_NATIVE_PURE` or the new
`FORTH_NATIVE_TASK`; anything that may re-enter the VM (`forth_eval`,
`forth_observe`, workbench, storage, module services) is refused. A background
fault is recorded in the task, never in the foreground's last error, compile
state or stacks.

## Guards

`ENGINE` is refused while any task is ready or sleeping (DIRECT and TOKEN IPs
are different encodings). `forth_transaction_rollback` is refused while tasks
are live (it reclaims code a task may execute). Module activation only adds
code and is allowed. `forth_init` clears every slot. The millisecond clock is
a registered hook; the resident board accumulates the 288 kHz timer at
scheduling points (a foreground computation longer than ~227 ms without a
scheduling point delays wakeups but cannot corrupt them).

## Host surface and acceptance

Harness tools `tasks` and `task_output`; `snapshot` includes the task table.
Accept on host (reference VM) and Agon (DIRECT): two tasks increment counters
and print while REPL/`evaluate` traffic continues; a faulting and a looping
task are isolated and killable; ENGINE/rollback refusals; KEY wait runs
tasks; measured slice cost, REPL latency with tasks idle vs busy, and the
engine benchmark unchanged when no task exists.

## Agon results, 2026-10-18 (DIRECT, 1152000 internal, slice 2048)

[Acceptance record](../../bring-up/vios-tasks-acceptance-2026-10-18.json).
CPU image 112059 -> 117731 bytes; BSS +4.6 KiB (end `0x9C2B2`, ~77 KiB gap).

- First run lost host bytes: the REPL idle poll re-asserts RTS when no byte
  is waiting, and a slice then ran without draining the 16-byte UART FIFO
  (`console-dropped` rose by 2; the harness lost a frame). Recovered without
  reboot by killing both tasks through ZDI `memory_write`, completing the
  partial line and `resync`. Fixed by a platform task gate: listen ~1 ms
  with RTS asserted, defer tasks if anything arrives, otherwise pause RX for
  the slice. Native test `gate` covers it.
- With the gate: REPL round trip 77.6 ms median without tasks, 88.3 ms with
  a busy and a sleeping task; busy task ~11.8k loop iterations/s; a
  `10 MS` sleeper wakes ~26/s because one busy slice takes ~25 ms; no console
  drops over the run.

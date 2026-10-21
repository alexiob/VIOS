# VIOS extensible compiler: IMMEDIATE, POSTPONE, [ ], LITERAL, STATE, EVALUATE, DOES>, >BODY, FIND, MARKER/FORGET (design)

Status: **design only, 2026-10-21**, from main at `46d94ce`. Nothing here is
implemented. Numbers marked *measured* come from eZ80 builds of this commit run
under the cycle model (`tools/verification/forth_compile_cycles.py`) or
from symbol tables (§11). *Estimate* means not measured. Related:
[Forth milestone](2026-10-16-vios-forth.md), [tasks](2026-10-18-vios-tasks.md),
[preemption](2026-10-19-vios-tasks.md), [boot speed, Phase 3 cache](2026-10-19-vios-boot-speed.md).

## 1. Question and short answer

The owner asked: *"investigate and plan implementing an extensible compiler
instead of a fixed one: `IMMEDIATE`, `POSTPONE`, `[` `]`, `LITERAL`, `STATE`,
`EVALUATE`, `DOES>`, `>BODY`, `FIND`, `MARKER` or `FORGET`. Pros and cons?
effort? Speed and memory impact?"*

**Yes, and none of it needs an engine change.** All three engines (DIRECT,
TOKEN, C) keep their opcodes. Every new word is compiler or C-native work in
`forth/system.c`, and DOES> can be built from the existing `LIT` and call forms.

**One finding changes the plan.** Today the compiler's control words (`IF`,
`:`, `S"`, `CREATE`, `'`, `;`...) are not dictionary words. They are a chain of
about 60 `equal(s,"…")` string compares in `forth_eval`, and every token walks
it before the dictionary is searched. That has two consequences:

1. `POSTPONE IF`, `FIND`, `'` and user macros cannot see these words. An
   extensible compiler needs them in a lookup structure with an id and an
   "immediate" property.
2. The chain is most of the compile cost. On the eZ80 model it is ~60% of
   compile time (*measured*: 340 µs/token at HEAD). A prototype that searches
   the dictionary first measured **216 µs/token, −37%**, with a 57 B smaller
   image.

So the recommended first step (Phase 0) is that refactor: a hashed keyword
table plus an IMMEDIATE bit. It makes compiling faster on its own and is the
base everything else sits on.

**Recommended subset and order (§9):**

- **Phase 0:** keyword table and IMMEDIATE bit (1 day).
- **Phase 1:** `STATE` `[` `]` `LITERAL` `IMMEDIATE` `FIND` `COMPILE,` `POSTPONE` (2 days).
- **Phase 2:** input source as VM-visible state, run-time `CREATE` `,` `C,`
  `ALLOT` `HERE`, then `DOES>` and `>BODY` (3 days).
- **Defer** `EVALUATE` (Phase 3, 2 days) and `MARKER` (Phase 4, 1 day) until a
  real need appears.
- **Do not build `FORGET`.**

The real constraint is RAM, not speed. A clean HEAD build leaves 17.4 KB
between `__bss_end` and the stack (*measured*). The /dev/win branch reports
~12.5 KB after its work, and the spec gate is ≥ 12 KB. Phases 1–3 need about
4.5 KB (*estimate*, §6), so they must come with a matching reclaim (§8). The
best reclaim is not moving the compiler to the ESP but a compact name pool
(~10 KB, eZ80 only). If more is needed, the ~50 KB of rarely-read source and
map data can move to ESP PSRAM (§10).

## 2. How the compiler works today

Facts the design depends on, with references into `forth/system.c` at `46d94ce`.

- **Outer interpreter in C.** `forth_eval` (`system.c:870`) tokenizes and
  upper-cases each word (`next_token`). It then walks a fixed chain of keyword
  checks (`SYNC`, `ABORT`, `:`, `S"`, `."`, `CHAR`, `[CHAR]`, `CREATE`, `ALLOT`,
  `HERE`, `,`, `C,`, `;`, `VARIABLE`, `CONSTANT`, `DEFER`, the 13 control words,
  `RECURSE`, `EXIT`, `'`, `[']`, `IS`, `TRACE`, the task tools, `WORDS`, `.S`,
  `SEE`, `RESOURCES`, `ENGINE`...). Only then does it `find()` the dictionary
  (`system.c:997`) and either `compile_word` or `execute_index`. Numbers come last.
- **"Compiling" is `current>=0`**, the index of the open definition. There is
  no separate STATE: "a definition is open" and "compiling" are the same thing.
- **Dictionary.** `struct word` (`system.c:19`) is **55 bytes on the eZ80**
  (*measured*: `words` is 28,160 B for 512 entries). It holds a 32-byte name,
  `token`/`direct` code addresses, one `aux` cell, `link`/`source`/`length`/code
  lengths/`origin` (16-bit each), and `kind`/`opcode` bytes. The `kind` values
  are 0–5 (`WK_PRIMITIVE`…`WK_NATIVE`), so bits 3–7 are free. An xt is
  index+1. Hash chains link newer words to older ones (`buckets[128]`).
- **Dual emission.** `op`/`operand`/`literal` write the token stream (1-byte
  opcodes, 3-byte operands) and the direct stream (3-byte primitive addresses)
  side by side. A colon word's direct code starts with `CD <_fvm_docol>`
  (4 bytes, `begin_word`, `system.c:238`). In direct code, a call to a colon
  word is one cell (its code-field address). The `F_CALL` primitive
  (`engine.s:259`) also exists in the direct table and jumps to any thread
  address that follows it.
- **Constants and variables are inlined.** `compile_word` (`system.c:283`)
  emits `LIT aux` at the call site instead of calling the word. The project
  already chose speed over call-site size for data words.
- **Data-space words are interpret-only.** `CREATE ALLOT HERE , C,` refuse
  to run inside a definition (`system.c:911-915`): "definitions cannot grow
  managed data at run time (transactions and overlays account for `data_used`
  per definition)".
- **Source maps.** For each source token of a colon definition the compiler
  records (word, token offset, direct offset, source offset) (`system.c:888`).
  `SEE` prints the stored source text, not decompiled code. `TRACE` and
  `LAST-ERROR` map an IP back to the source token. The pool has 640 entries
  (5,120 B).
- **Module transactions** (`forth_transaction_eval`, `system.c:1156`):
  1. Take a checkpoint of the dictionary counters, all 128 buckets, all 8 KiB
     of data and the stacks.
  2. Turn on the data overlay: the VM can write only the 8 KiB overlay copy;
     any write outside data space faults.
  3. Run on the **C reference engine**, in decimal, with I/O and non-PURE
     natives refused.
  4. Run the test word; its data writes are discarded by restoring the overlay
     to `test_data`.
  5. Commit, or restore the checkpoint.

  Rollback of the latest activation is allowed only if nothing was allocated
  after it. Module records (`modules.c`) own words through `origin`.
- **Boot-module cache** (`forth_snapshot_*`, `system.c:1191-1224`). Snapshots
  hold the words, both code streams, source, source map, buckets and data
  added by the boot module loads. They are valid only for the same build and
  base, and `sizeof(struct word)` is checked. A miss costs a boot of 2,833 ms
  against 857 ms for a hit (boot-speed spec). The cache relies on a module
  load being a **pure, deterministic dictionary transformation**.
- **Tasks and preemption.** Background tasks run only when no definition is
  open, no transaction or observer is active and nothing is already scheduling
  (`forth_tasks_run`, `system.c:447`). A tick while a definition is open is
  ignored (`system.c:322`). Natives run atomically in C. Each task has its own
  BASE (swapped in `run_slice`). `CRITICAL(` depth is per context and reset
  after a failed foreground command.
- **Re-entrancy.** `execute_index` (`system.c:368`) sets `fvm_rdepth =
  fvm_ldepth = 0` and a fresh IP and budget. So C must not start Forth
  execution from inside a native (`system.h`: "no VM re-entry").
- **Natives.** There are 120 slots (`NATIVES_MAX`) and 111–117 are in use.
  Flags: `PURE` (allowed in transactions) and `TASK` (allowed in tasks).

**RAM** (*measured*, clean HEAD build): `__bss_end` 0xAAC15, stack bottom
0xAF000, gap 17,387 B. `system.o` has 36.8 KB text and 133 KB BSS.

## 3. The unifying change: a keyword table and an IMMEDIATE bit (Phase 0)

### 3.1 Measured cost of the current chain

`forth_eval` of a prelude-like source (201 tokens, 25 definitions) after
`forth_init`, on the resident ELF under the cycle model (*measured*):

| build | cycles | µs/token | top of profile |
|---|---|---|---|
| HEAD `46d94ce` | 1,260,484 | **340** | `_equal` 31.5%, `_forth_eval` 31.0%, `_next_token` 9.5%, `__imulu` 7.8% |
| prototype: `find()` before the chain | 798,400 | **216 (−37%)** | `_forth_eval` 23.4%, `_equal` 15.5%, `_next_token` 14.5% |

The prototype only moved the dictionary lookup ahead of the chain. Keywords
and numbers still walk all of it, which accounts for the remaining 15% in
`_equal`. The profile split is approximate: cycles are attributed by function
start address, so a tail call can be charged to a neighbour (`_unknown` 2–4%
in an error-free compile is a sign of this). The build totals are reliable.

At this rate, the boot spec's measured 640 ms cache-miss compile corresponds
to roughly 1,900 tokens of module source (*estimate*). The model reads ~6% low.

`__imulu` is the 55-byte `struct word` indexing in `find`, not the hash. A
`(h<<5)+h` hash compiled to the same code under `-Oz`.

### 3.2 Design

Replace the chain with a **const keyword table** in `.rodata`. Each entry
holds a name, a 1-byte id and flags: `IMMEDIATE`, `COMPILE_ONLY`,
`INTERPRET_ONLY`, `PARSES`. It is bucketed by the same 7-bit hash the
dictionary uses, so `hash(s)` is computed once per token. A `switch(id)`
replaces the `if(equal…)` cascade. Lookup order stays **keyword first, then
dictionary, then number**, so no existing program changes meaning (a word
named `TRACE` in a module still loses to the keyword, as today).

User words get **IMMEDIATE as bit 7 of `kind`** (`#define KIND(w)
((w)->kind&0x7f)`). That costs 0 B. The bit keeps `sizeof(struct word)` at
55 B, so the boot cache is not invalidated (a size change would force one
+2 s cache-miss boot per owner). `forth_inspect_word` reports the masked
kind plus a new `immediate` field, so host tools and the `kind 0` test
assertions are unchanged.

Rejected alternative (K1): make each keyword a dictionary word of a new kind
`WK_KEYWORD`. It is more uniform (FIND, `'`, `WORDS` and SEE see keywords for
free) but costs ~60 × 55 B = **3.3 KB** of the RAM gap, 60 of 512 dictionary
slots, and changes `WORDS` output and the `words 65/512` / `415/512` test
assertions. The table gives the same speed for ~0.5 KB.

Effect *(estimate)*: compile ≥ −37% (the prototype), likely −40–45% once
keywords and numbers no longer walk the chain. Text −0.3…+0.2 KB: the table
(~0.5 KB rodata) replaces ~60 inline `equal` call sites. Run speed unchanged.

## 4. The words, one by one

Each subsection gives the ANS meaning, the proposed VIOS design and how it
fits the engines, source maps, transactions, the boot cache, tasks, BASE and
CRITICAL.

### 4.1 `STATE`, `[`, `]`

**ANS.** `STATE ( -- a-addr )` is true while compiling; programs may read it
but not store into it. `[` (immediate) enters interpretation state. `]`
enters compilation state.

**Design.** Split today's `current>=0` into two predicates:

- `open()` (`current>=0`): a definition is being built.
- `compiling()` (`state!=0`): compile, don't execute.

`:` sets both; `;`, `rollback()` and errors clear both. `[` clears `state` and
`]` sets it, and `]` requires an open definition (VIOS has no compile-to-
nowhere). Each `current>=0` site in `forth_eval` goes into one of these
classes. **This classification is the effort for this item:**

| use today | becomes | sites |
|---|---|---|
| source text / source-map recording, `;`, `begin_word` "nested definition", outer hook (`current<0`), interactive tools refused, `forth_tasks_run` / tick gating, transaction / observer / snapshot "idle VM" checks, `rollback` | `open()` (unchanged) | ~12 |
| `S"` literal vs push, `."` compile vs run `TYPE`, `CHAR`/`[CHAR]`, `'`/`[']`/`IS` literal vs push, control words "compile-only", `RECURSE`, `EXIT`, dictionary word compile vs execute, number literal vs push | `compiling()` | ~13 |
| `CREATE ALLOT HERE , C,` "data-space word inside definition" | `compiling()`: becomes **legal under `[`** (`[ HERE ]`) | 1 |

`STATE` itself is a cell in data space, like `BASE`, created at `forth_init`
before the snapshot base. C writes it through `fvm_write_cell`, so overlay
writes stay correct in transactions. It is one dictionary slot plus 3 B of
data. It is read-only by convention, as in ANS.

**Fit.**

- *Tasks:* tasks never run while a definition is open, so a task always reads
  `STATE @` as 0. No per-task STATE is needed.
- *Transactions:* `[` … `]` inside a module definition executes on the
  reference engine under the transaction budget. A failure rolls back the
  definition and the transaction.
- *Source maps:* tokens between `[` and `]` are still recorded (open), so SEE
  shows them. They produce no code, so they have no map entries.
- *BASE:* numbers under `[` parse in the current BASE (decimal in modules).

**Pros:** enables `[ … ] LITERAL` constant folding and STATE-aware words.
Cheap. **Cons:** the open/compiling split touches ~25 lines of the hottest
function, and every one needs a test.

### 4.2 `LITERAL`

**ANS.** Immediate, compile-only: `( x -- )` at compile time, appends code
that pushes `x`.

**Design.** A keyword (`COMPILE_ONLY|IMMEDIATE`): pop, then `literal(x)`.
Emits 4 token / 6 direct bytes, the same as a number. Source map: the code
maps to the `LITERAL` token. **Pros:** trivial (~40 B text) and needed by
`[ ]` and by macros. **Cons:** none.

### 4.3 `IMMEDIATE`

**ANS.** Makes the most recent definition immediate: it executes even in
compilation state.

**Design.** `IMMEDIATE` sets bit 7 of `latest`'s `kind`. In `forth_eval`, a
dictionary hit that is immediate while compiling calls `execute_index` instead
of `compile_word`. That is one byte test per dictionary hit, < 0.5% of a
token's cost.

**The `latest` rule** (a conflict with transactions). ANS says "most recent
definition", but in VIOS that word may belong to a previous module. A module
whose source begins with `IMMEDIATE` would flip a header **outside** its
transaction. The checkpoint restores counters, buckets and data but not
headers, so a failed load would leave the flag set. So:

- `latest` is a static index set by `publish()`.
- It is cleared at transaction begin and end, at snapshot read and at MARKER.
- `IMMEDIATE` (and `DOES>`, §4.7) act only on `latest` and fail with
  "IMMEDIATE needs a definition from this unit" otherwise. A module can
  flag only its own words, and interactive code cannot re-flag a module's
  words.

**Compile-time execution inside a transaction** (an IMMEDIATE word used in
module source):

- It runs on the **C reference engine**, ~5× slower than TOKEN (*measured*:
  2,552 vs 483 cycles for the same loop body).
- It draws on the transaction's shared 1 M-instruction budget.
- I/O and non-PURE natives are refused, so a macro that prints at compile time
  fails inside a module load.
- A failure fails the module atomically, as today.

**Boot cache:** compile-time code can only use PURE natives and VM ops, and
those writes are confined to the overlay and code buffers, so a module load
stays a deterministic dictionary transformation and the cache stays valid. On
a cache hit the immediate words do not run at all. That is correct only
because they can have no effect outside the dictionary. §6 states this
invariant explicitly, and new PURE natives must keep it.

**Tasks and preemption:** compile-time execution happens with a definition
open, so ticks are ignored and **background tasks (audio, GUI frame tasks)
stall** for its duration, bounded by the budget. This is the same as a long
interactive command today, but it is new for compile time. Relaxing the
"no tasks while a definition is open" gate looks safe (tasks cannot reach
unpublished code, and dictionary-mutating natives are refused in tasks,
§4.6), but it needs its own review: open question, not in scope.

**CRITICAL:** unchanged. The `critical` reset stays tied to the outermost
`executing` level.

**Source maps:** an immediate word's map entry is recorded before it runs,
so the code it emits belongs to its token. TRACE and LAST-ERROR then show the
macro's name at that code, and SEE stays source-exact, which is better than a
decompiler would do.

### 4.4 `POSTPONE` and `COMPILE,`

**ANS.** `POSTPONE name` (immediate) adds name's *compilation* semantics to
the current definition. For a non-immediate word that means "compile a call
to name when this runs". For an immediate word it means "call name now when
this runs". `COMPILE, ( xt -- )` appends xt's execution semantics.

**Design.**

- `COMPILE,` is a native: `compile_word(xt-1)`. It handles all kinds,
  including the inlined `LIT` for constants and variables, and needs an open
  definition. Class `DICT` (§4.6).
- `POSTPONE name` (keyword, `PARSES`) handles three cases:
  - dictionary word, not immediate: compile `LIT xt` then a call to `COMPILE,`
    (token 8 B, direct 9 B);
  - dictionary word, immediate: `compile_word(n)`;
  - **keyword**: compile `LIT id` then a call to `(KEYWORD) ( id -- )`, one
    native that runs the keyword's C action from the table. This is what makes
    `: UNLESS POSTPONE 0= POSTPONE IF ; IMMEDIATE` work.
- The control-flow stack stays in C (`controls[]`), not on the data stack, so
  a postponed `IF` and the user's later `THEN` meet there. ANS `CS-PICK`/
  `CS-ROLL` are not offered.
- Keywords that parse (`S"`, `."`, `[CHAR]`, `[']`, `IS`, `POSTPONE`)
  read from the *current* input when the macro runs, so `POSTPONE S"`
  needs §4.6's input-source state. In Phase 1, POSTPONE of a parsing keyword
  fails with a clear message.
- Source map: `POSTPONE` and its name are recorded like `'` (both tokens go
  into the source text).

**Pros:** user control structures and macros without C changes. **Cons:**
`(KEYWORD)` lets Forth code reach C compiler internals. That is acceptable for
a trusted systems language, but every keyword action must check `open()` and
`compiling()` itself rather than assume the outer interpreter did.

### 4.5 `FIND`

**ANS.** `FIND ( c-addr -- c-addr 0 | xt 1 | xt -1 )`: look up a counted
string. 1 means immediate, -1 means normal.

**Design.**

- A native that reads the counted string through `fvm_read8`, which is
  correct under the overlay, upper-cases it and calls `find()`. Class `PURE`,
  so it is allowed in transactions and tasks (it is atomic C, and tasks never
  run mid-definition).
- Keywords have no xt, so `FIND` returns 0 for `IF` and `' IF` stays an error.
  This is a documented deviation; POSTPONE covers the real use.
- Optional extra: `FIND-NAME ( c-addr u -- xt|0 )`, because VIOS strings
  are `c-addr u` (`S"`) and counted strings are rare.

**Fit:** stale xts are the hazard, as with `'` today. Only MARKER (§4.9) can
invalidate an xt, and it is refused while tasks are live. **Cost:** one native
slot and ~150 B text (*estimate*).

### 4.6 Prerequisite: run-time data space and input source as state (Phase 2a)

`DOES>` is only useful in defining words (`: CONST CREATE , DOES> @ ;`), and
there `CREATE` and `,` run **inside a colon word, inside `dispatch()`**. Today
they are interpret-only by design. This item, not the engines, is where
DOES> costs effort.

1. **Input source as state.** `forth_eval`'s parse pointer `p` is a C
   local, so a native cannot read the next name. Make it a static
   `struct input {const char *p; unsigned char depth;}` saved and restored
   around nested `forth_eval` calls. Natives `PARSE-NAME`/`WORD`-style helpers
   and run-time `CREATE` read from it. In a background task there is no input,
   so these fail with "no input source". EVALUATE (§4.8) needs the same
   change, so do it once.
2. **`CREATE , C, ALLOT HERE` become natives** callable from definitions,
   removed from the keyword table. Interpretive use then goes through the VM
   (a few hundred cycles more per use; *estimate*).
3. **A new native class `FORTH_NATIVE_DICT`** for dictionary-mutating natives
   (`CREATE , C, ALLOT COMPILE, (DOES>) EVALUATE`, MARKER execution):
   - allowed in the foreground and inside module transactions;
   - refused in background tasks (`running>=0`), in observers
     (`observer_active`), and in the **module test phase**;
   - the dispatcher's transaction check becomes `!(flags&(PURE|DICT))`.

   The test phase needs a new `testing` flag, set between `fvm_overlay_copy`
   and `fvm_overlay_restore`. Dictionary growth there is not undone: the
   restore only rewinds data bytes, not `count`/`token_used`/`data_used`.
4. **Accounting still holds.** `data_used` growth inside a transaction is
   covered by the checkpoint counters. Growth while a definition is open
   (`[ 10 ALLOT ]`) is rewound by `rollback()` via `mark_data` if the
   definition fails. Growth at the REPL after a module activation blocks that
   module's rollback, as any later allocation does today. The
   `created_unfilled`/`unpublish_created` guard (a failed first `ALLOT` removes
   the CREATE'd word) must also cover run-time CREATE.

### 4.7 `DOES>` and `>BODY`

**ANS.** In a defining word, `DOES>` ends the defining part. At run time it
replaces the behaviour of the most recent CREATE'd word with "push body
address, then run the code after DOES>". `>BODY ( xt -- a-addr )` gives a
CREATE'd word's data address.

**Code layout, with no engine change.** A CREATE'd word today is token
`LIT body EXIT` (5 B) and direct `CD docol, LIT body, EXIT` (13 B). With
DOES>:

- **Parent** (`: CONST CREATE , DOES> @ ;`). `DOES>` (keyword) compiles
  `LIT t LIT d (DOES>) EXIT`. `t` and `d` are the token and direct addresses
  of the does-part: the native must patch **both** streams, whichever engine
  runs the parent. In the **direct stream only**, it then emits a 4-byte
  `CD docol` prefix so the does-part is callable as a one-cell target. After
  that, compilation of the does-part (`@ ;`) continues as normal. Source maps
  need nothing special: the direct offsets absorb the prefix. One-off cost per
  defining word: token 13 B, direct 22 B.
- **Child** (`5 CONST FIVE`). Run-time `(DOES>)` checks that `latest` is a
  CREATE'd word and that its code is still the **last code in both streams**
  (nothing compiled since). It then rewinds both streams to just after
  `LIT body` and appends:
  - token: `CALL t, EXIT`, giving 9 B (+4);
  - direct: `d`, the does-part's code-field cell, then `EXIT`, giving 16 B (+3).

  It sets `kind = WK_DOES` (a new kind; `aux` keeps the body) and updates
  `token_length`/`direct_length`. This is the same tail-rewind trick
  `unpublish_created` already uses.
- **Call sites inline the child**, as constants and variables already are:
  `compile_word(WK_DOES)` emits `LIT body` followed by the does-part call. It
  reads both operands back out of the child's own code, so it needs **0 header
  bytes**. A call site costs token 8 B (+4 over `CALL child`) and direct 9 B
  (+6).

**Run speed** (*measured*, cycles per call including `DROP`):

| | inline `V @` (VARIABLE) | does-child, inlined at call site (= one nest, ≈ a classic DODOES primitive) | does-child, called |
|---|---|---|---|
| DIRECT | 399 | **706** | 1,013 |
| TOKEN | 483 | **855** | 1,227 |

Inlining removes one nest (~307 cycles DIRECT, ~372 TOKEN) and reaches the
speed a dedicated `DODOES` opcode would give, without one.

**Rejected alternatives.**

- (a) A `F_DODOES` opcode: it touches all three engines (two assembly builds,
  `OP_COUNT`, the C reference, the engine ABI doc and its verification) to
  save ~3 B per child.
- (b) The does-address in `struct word`: +3 B × 512 = 1.5 KB of the RAM gap
  (+6 B if it also held a direct twin), and a cache-invalidating size change.

**`>BODY`:** a native that returns `aux` for `WK_VARIABLE` (CREATE'd or
VARIABLE) and `WK_DOES`, and fails for other kinds. ~80 B text.

**Fit.**

- *Engines:* the three engines run unchanged code forms (`LIT`, `CALL`, a
  code-field cell), so they cannot disagree. The existing engine-agreement
  tests cover the patterns; add one DOES> test per engine.
- *Source maps / SEE / TRACE:* the child has no source (SEE shows "data
  word"; it could print "created by CONST" later). Code in the does-part maps
  to the parent's `@` token, so TRACE shows `CONST+n src @`, which is correct.
  An inlined child's name does not appear in crash frames, only the caller and
  the does-part.
- *Transactions:* defining words used in module source run on the reference
  engine. Run-time CREATE parses from the module source. The child gets
  `origin` = the module, and everything rolls back with the checkpoint.
- *Boot cache:* children are ordinary words and code bytes, captured as is.
  The addresses are absolute, which is fine because the cache is per build.
- *Tasks:* calling a child in a task is fine. Defining children in a task is
  refused (`DICT`).

### 4.8 `EVALUATE`

**ANS.** `EVALUATE ( i*x c-addr u -- j*x )` interprets the string as the
input source, then restores the previous source. STATE is shared with the
caller, so evaluated text can compile into the open definition. This is the
classic text macro: `: SQ S" DUP *" EVALUATE ; IMMEDIATE`.

**The break.** A native that calls `forth_eval` re-enters the VM from inside
`dispatch()`. `execute_index` then zeroes `fvm_rdepth`/`fvm_ldepth` and
replaces IP and budget, destroying the outer word's return and loop stacks.

**Design.**

- **Nested execution.** Add `execute_nested(n)`. It saves `fvm_ip`, pushes a
  **sentinel return address** and enters `n` without touching the depths or
  the budget, so the remaining budget is shared and the transaction budget
  still applies.
  - The sentinel is a 2-cell stub made at `forth_init` (before the snapshot
    base): token `NATIVE (EVAL-RETURN)` (5 B), direct `[NATIVE] id` (6 B).
  - When the nested word's final EXIT pops to the sentinel, the engine traps
    into the dispatcher, which recognises the id and returns. No engine change.
  - After return it checks that `fvm_ldepth` is back to its entry value, then
    restores `fvm_ip`.
- **String copy.** The text lives in VM memory, and during a transaction its
  current bytes are in the overlay, not in `data_space`. It is copied through
  `fvm_read8` into an **eval pool** (512 B BSS, stack-allocated per nesting
  level) and NUL-terminated.
- **Nesting ≤ 4 levels** (pool- and stack-bounded). This is the one place a
  Forth call consumes recursive C frames (`forth_eval → dispatch → native →
  forth_eval`), which the milestone spec otherwise forbids. At ~150–200 B per
  level, 4 levels cost ≤ 0.8 KB of the 4 KiB native stack (*estimate*).
  Measure the high-water on the board before shipping.
- **Nested `forth_eval` mode:** no `outer_hook` (kernel commands must not run
  from inside a word), no `SYNC`, and `ABORT` becomes a failure.
- **Source recording.** Tokens evaluated while an *outer* definition is open
  are **not** recorded: the stored source must stay what the user typed, and
  the generated code maps to the `SQ` token. A definition *begun* inside the
  evaluated text records its own source normally, so
  `S" : FOO 1 ;" EVALUATE` then `SEE FOO` works.
- **Class `DICT`:** refused in tasks (a slice would run an interpreter, and
  "slice ended" inside a nested dispatch has nowhere to go), in observers and
  in the test phase.
- **BASE:** numbers parse in the caller's BASE, as ANS requires (decimal in
  modules, the task's own BASE is irrelevant because tasks are refused).
- **CRITICAL:** nested levels increment `executing`, so the reset still
  happens only at the outermost command.

**Speed:** EVALUATE at run time pays the full compile path per token, ~216 µs
after Phase 0 (*estimate*). It belongs in compile-time macros, never in loops.

**Pros:** text macros, configuration strings, building definitions from data
(for example GUI forms). **Cons:**

- recursive C frames;
- 512 B BSS plus ~1 KB text (*estimate*);
- a second interpreter mode to test;
- in modules it runs on the C engine.

POSTPONE (§4.4) already covers most macro needs, so this is the item with the
weakest value per cost. **Defer it.**

### 4.9 `MARKER` and `FORGET`

**ANS.** `MARKER name` creates a word that, when executed, removes itself and
everything defined after it. `FORGET name` (obsolescent) removes `name` and
everything after it.

**Design (MARKER only).**

- `MARKER name` creates a `WK_MARKER` word whose `aux` holds `data_used` at
  creation.
- Every other mark is derived from its own header: count = its index; token,
  direct and source starts = its own code and source starts; the map mark is
  the first `source_map` entry with `word ≥` its index. So it needs 0 extra
  bytes.
- Executing it (class `DICT`, foreground only, no open definition) checks the
  guards below, then:
  - resets the counters;
  - rewinds `buckets` by walking each head above the mark down its `link`s
    (no 256 B bucket copy per marker);
  - clears `latest` and `created_unfilled`;
  - bumps `catalog_revision`, so inspectors refresh and LAST-ERROR goes stale.

**Guards (each one a real conflict):**

1. **Module ownership.** Refuse if any word above the mark has `origin≠0`.
   `modules.c` records would otherwise list an active module whose words are
   gone (including retired generations kept "until restart").
2. **Rollback checkpoint.** If a module activation is still rollback-able,
   refuse marks below `activation_count`. (By rule 1 they would cross module
   words anyway.)
3. **Dangling bindings.** Refuse if any surviving `DEFER` cell holds an xt
   above the mark. Reuse `transaction_bindings_valid`'s walk. Variables that
   hold xts cannot be checked; that is documented, as in other Forths.
4. **Live tasks.** Refuse while tasks are live (a task's xt or return stack
   may point into the removed code), as `forth_transaction_rollback` already
   does.
5. **Natives.** Natives are registered only at init, below any user marker,
   but refuse marks below the snapshot base anyway, as a fence.
6. **Inside a transaction.** Allowed only for marks made in the same
   transaction. The checkpoint covers the rest.

**FORGET:** it is the same cut with the mark taken from `name`'s header, but
it can target any word, including ones deep under module words. Guard 1 would
refuse most uses, and ANS marks it obsolescent. **Do not implement.** Modules
already provide the managed way to replace code (stage, activate, rollback).

**Boot cache:** markers live in interactive space. A marker used inside module
source (define helpers, then remove them) works under guard 6, and the
snapshot then records the result. **Pros:** scratch sessions can be thrown away
without a reset. **Cons:** five guards to test, and value is modest next to
modules. **Defer.**

## 5. Conflicts at a glance

| conflict | where it bites | resolution |
|---|---|---|
| IMMEDIATE / DOES> act on "most recent definition" | flag or patch a header outside the transaction, which rollback does not restore | `latest` cleared at transaction begin/end; act only on `latest` (§4.3) |
| Compile-time execution inside a transaction | runs on the C engine, shares the 1 M budget, I/O refused | documented; failure fails the module atomically (§4.3) |
| Boot cache needs pure module loads | compile-time code with side effects would be skipped on a cache hit | only PURE/DICT natives allowed in transactions; invariant stated for new PURE natives (§4.3, §6) |
| Run-time CREATE / `,` / ALLOT | dictionary mutation from inside `dispatch()`; test-phase growth not undone | `DICT` class: foreground and transaction only; `testing` flag (§4.6) |
| DOES> against direct threading | does-part has no code field; two code streams | `CD docol` prefix in the direct stream only; `LIT t LIT d`; tail rewind (§4.7) |
| EVALUATE re-entrancy | `execute_index` zeroes the return and loop stacks | `execute_nested` plus a sentinel return stub; nesting ≤ 4 (§4.8) |
| EVALUATE in a task or with the outer hook | interpreter inside a slice; kernel commands from inside a word | refused / hook disabled when nested (§4.8) |
| EVALUATE / STATE and source maps | SEE text would diverge from typed source | no recording for nested tokens inside an outer definition (§4.8) |
| MARKER against modules, rollback, bindings, tasks | dangling module records, xts and task IPs | guards 1–6; FORGET not built (§4.9) |
| Ticks while compiling | long macros stall background tasks | accepted for now; relaxing the gate is an open question (§4.3) |
| Per-task BASE | none: tasks never compile or evaluate | n/a |
| `CRITICAL(` | nested execution must not reset `critical` early | reset still only at the outermost `executing` level |

## 6. Speed and memory impact

**Compile speed** *(measured on the model)*: 340 → 216 µs/token with the
dictionary-first prototype; ≥ −37% expected from Phase 0. A boot cache miss
compiles in ~640 ms today, so this saves roughly 240 ms+ (*estimate*). Cache
hits do not compile. Later phases add one byte test per dictionary hit
(< 0.5%).

**Run speed of the engines:** **unchanged**. No opcode, no dispatch change,
no change to `.Lnext`. DOES> children cost one nest more than a VARIABLE
(the table in §4.7). Compile-time code inside modules runs on the C engine
(~5× TOKEN), but only once per load, and not at all on a cache hit.

**Word-header size:** 55 B, **unchanged**. The IMMEDIATE bit and the new kinds
(`WK_DOES`, `WK_MARKER`) fit in `kind`. DOES> data lives in the child's code
and MARKER marks in `aux`. Changing the size would cost each owner one cache
miss (+~2 s) and +512 B per added byte.

**RAM, by kind** (*estimate* unless marked):

| item | new static BSS | eZ80 text + rodata | fixed pools used (no new RAM) |
|---|---|---|---|
| Phase 0: keyword table, IMMEDIATE bit | 0 | −0.3…+0.2 KB | — |
| Phase 1: STATE `[ ]` LITERAL IMMEDIATE FIND COMPILE, POSTPONE `(KEYWORD)` | ~10 B (`state`, `latest`) + 3 native slots (18 B) | ~1.2 KB | STATE: 1 word, 3 B data; 3 native words ≈ 3 × (5 token + 13 direct) B |
| Phase 2: input state, run-time data-space natives, DOES>, >BODY | ~10 B + 7 native slots (42 B) | ~1.6 KB | 7 words; per child +4 token / +3 direct; per inlined call site +4 / +6; per defining word 13 / 22 |
| Phase 3: EVALUATE | 512 B pool + sentinel state | ~1.0 KB | sentinel stub 5 token / 6 direct |
| Phase 4: MARKER | 0 | ~0.6 KB | one word per marker |
| **Total 1–4** | **~0.6 KB** | **~4.4 KB** | |

`NATIVES_MAX` 120 → 132 (+72 B, included above). Code and data pools
(`TOKEN_BYTES` 16 KiB, `DIRECT_BYTES` 32 KiB, `DATA_BYTES` 8 KiB,
`WORDS_MAX` 512) are preallocated BSS. New dictionary words consume capacity
in them but no new RAM.

**eZ80 image:** 186,039 B at HEAD (*measured*). It grows by ~4.4 KB if all
phases land, about +40 ms of stage1 load at 1,152,000 baud (*estimate*), and
less once Phase 0's shrink is counted.

**The invariant to keep:** a module load must stay deterministic given its
sources. Compile-time code may only use VM ops and PURE/DICT natives. A new
PURE native must never read time, input, random state or device state.

## 7. Effort, phased (days)

| phase | contents | days |
|---|---|---|
| 0 | keyword table (hash-bucketed `.rodata`), switch dispatch, IMMEDIATE bit plumbing (`KIND()` mask, inspect field), compile-speed test plus board measurement | **1** |
| 1 | `state`/`open` split (~25 sites, §4.1 table); `STATE` `[` `]` `LITERAL` `IMMEDIATE` `FIND` (`FIND-NAME`) `COMPILE,` `POSTPONE` and `(KEYWORD)`; `latest` rule; host tests for interactive, transaction and rollback, snapshot round-trip, three engines | **2** |
| 2 | input source as state; `CREATE , C, ALLOT HERE` as natives; `FORTH_NATIVE_DICT` and the `testing` flag; `DOES>` (parent and child layout, tail rewind, inlined call sites) and `>BODY`; DOES> tests on DIRECT/TOKEN/C, in modules, through the cache | **3** |
| 3 | `EVALUATE`: `execute_nested` plus sentinel, eval pool, nested-mode rules, source-recording rule, stack high-water on the board | **2** |
| 4 | `MARKER` and guards 1–6 | **1** |
| — | board acceptance across engines, manual and user-guide updates | **1** |
| — | RAM reclaim to fund Phases 1–3 (§8), separate work | **0.5–1.5** |

The recommended subset (0–2 plus acceptance) is **~7 days** plus the reclaim.
All phases come to ~10–11 days.

## 8. Risks

- **RAM headroom (highest).** 17.4 KB at HEAD, ~12.5 KB after /dev/win
  (its own note), gate ≥ 12 KB. Phases 1–3 need ~4.5 KB, so pick a reclaim
  first. §10 analyses the two best candidates: a compact name pool and moving
  the rarely-read data to the ESP. Other candidates (sizes *measured* from
  `system.o`; feasibility not analysed here):
  - the 5,120 B source-map pool (the handoff already lists it);
  - the 4.6 KiB console shadow (handoff);
  - the transaction machinery: `transaction_before` and `activation_before`,
    9,258 B each, `test_data` 8,192 B and the overlay 8,192 B, ~35 KB in all.
    For example, the 768 B full-stack copy in each checkpoint could save only
    `depth` cells.
- **Semantic drift in the hottest function.** The open/compiling split
  touches ~25 branches of `forth_eval`. Mitigation: Phase 0's switch makes
  each keyword's action a separate case, and the tests per §4.1 table.
- **STATE-smart pitfalls.** Users writing STATE-dependent immediate words
  get the classic Forth surprises (for example `POSTPONE` of a STATE-smart
  word). Documentation, not code.
- **Compile-time code stalls tasks.** A heavy macro freezes GUI and audio
  tasks while it runs (ticks are ignored when a definition is open). Bounded
  by the budget. The open question is in §4.3.
- **EVALUATE C recursion** on the 4 KiB native stack, with FatFs on the same
  stack. This is why it is deferred, and why it must be measured on the board
  before it ships.
- **Three-engine coverage.** No engine changes, but every new code shape
  (DOES> parent prefix, child tail, inlined child) needs a test on each engine.
  DIRECT can only be checked on the board or under the cycle model.
- **Inspector and host tools.** New kinds (`WK_DOES`, `WK_MARKER`) and the
  `immediate` field must be taught to `tools/` and the GUI inspector, or they
  show raw numbers.

## 9. Recommendation

1. **Phase 0 now**, on its own merits: ≥ 37% faster compile on cache misses
   and in interactive and harness use, slightly smaller image, no semantic
   change. It also prepares the ground for the rest.
2. **Phase 1** next. It gives user-defined syntax (`IMMEDIATE`, `POSTPONE`,
   `[ ] LITERAL`, `STATE`, `FIND`, `COMPILE,`) for ~1.2 KB, with no engine or
   cache impact.
3. **Phase 2** when the RAM reclaim is in. Defining words (`CREATE … DOES>`)
   are the largest expressiveness gain: structures, enums, tables, GUI widget
   definitions. The cost is the run-time data-space rules, not the engines.
4. **Defer Phase 3 (EVALUATE)** until a concrete need appears. POSTPONE covers
   most macro uses, and EVALUATE brings C recursion, 512 B BSS and C-engine
   speed in modules.
5. **Defer Phase 4 (MARKER)**, and **do not build FORGET**. Modules are the
   managed way to replace code. MARKER is a convenience for scratch sessions.

## 10. Alternative: offload to the ESP, and a compact name pool

The owner asked (2026-10-21) whether the compiler could move to the ESP32 and
use its RAM. The ESP has ~4 MB of free PSRAM. Its internal RAM is scarce: ~39 KB
free against the 24 KB floor fabgl needs (audio note).

### 10.1 Moving the compiler: no

A full move would free ~55–60 KB of eZ80 RAM (*measured* symbol sizes):
`words` 28,160 B, `source_space` 8,192 B, `source_map` 5,120 B, and ~20 KB
of compiler text (`forth_eval` alone is 10,878 B). But:

- **It conflicts with extensibility.** `IMMEDIATE`, `[ ]`, `DOES>` and
  `EVALUATE` run user Forth at compile time against the eZ80's data space
  and stacks. An ESP compiler needs either a fourth VM plus a coherent
  mirror of eZ80 memory, or a link round trip per compile-time execution.
  A *fixed* compiler is portable to the ESP; an extensible one mostly is not.
- **Atomicity would span two processors.** Module transactions and the boot
  cache rely on one commit point. Splitting them needs a commit protocol
  across the link.
- **There is little speed to gain.** After Phase 0 a 10-token line compiles
  in ~2 ms (*estimate*). That is about one round trip on the 1,152,000-baud
  internal UART, and boots with a cache hit don't compile at all.
- **The runtime still needs headers.** `EXECUTE`, `DEFER`, task names,
  `LAST-ERROR`/`TRACE` and natives look words up. At minimum the eZ80 keeps an
  xt→code table (~3 KB) and names for diagnostics, or makes a round trip each
  time.
- **Coupling and recovery get worse.** The eZ80 could not compile without a
  working ESP, every eZ80 change needs a paired ESP build, and the 6502-port
  boundary (handoff §2) gets an ESP dependency.

### 10.2 What to move instead: data the eZ80 rarely reads (~50 KB)

| data | eZ80 bytes (*measured*, `system.o` / `modules.o` BSS) | readers |
|---|---|---|
| module source pool | 32,768 | `MODULE-SOURCE`, inspectors, manager tool |
| staged (pending) module record and source | 4,368 | stage → activate |
| `source_space`: definition text | 8,192 | `SEE`, inspector, print-source of the source map |
| `source_map` | 5,120 | `TRACE`, `LAST-ERROR` |
| **total** | **50,448** | none on the execution path |

**Design: the ESP holds the bytes, the eZ80 holds the offsets.**

- The eZ80 keeps `source_used` and `map_used` and every header's
  `source`/`length` exactly as today. The ESP store is a dumb blob addressed by
  offset (one region per kind).
- **Writes.** The compiler keeps writing into a small eZ80 staging buffer
  (for example 512 B, enough for one definition plus its map entries). At
  `publish()` it sends one append request per definition. A typical definition
  is a few hundred bytes, ~3 ms at 115 KB/s (*estimate*). That request needs
  the link's CRC and an acknowledgement: a lost append would make SEE wrong.
- **Rollback is free.** Failed definitions, transaction rollback and MARKER
  only rewind the eZ80 counters. Stale ESP bytes beyond them are ignored, so
  atomicity and the single commit point stay on the eZ80.
- **Reads.** SEE, LAST-ERROR and module-source fetch over the link. TRACE
  fetches one word's map entries once per trace, not per step. The GUI
  inspector and manager already render on the ESP, so they could read the
  store directly, without an eZ80 round trip.
- **Module sources.** Activation compiles from the 4 KiB pending buffer,
  which stays on the eZ80. After commit, the source goes to the ESP pool
  instead of `source_pool`. Sources installed from the SD bundle could also be
  referenced by bundle offset instead of copied.

**Costs and conflicts:**

- **ESP resets lose PSRAM.** A VDP restart empties the store. SEE then reports
  "source unavailable" until it is reloaded, and execution is unaffected. Reload
  sources: the module sources from the SD bundle, the definition text and map
  from the boot-cache file.
- **The boot cache changes.** Today the snapshot carries the source, map
  and module-pool regions. On a cache hit they would have to be pushed to the
  ESP (~50 KB ≈ 0.45 s at 115 KB/s, *estimate*) or loaded lazily from the cache
  file on the first SEE. Lazy loading keeps the 857 ms boot.
- **Inspection APIs change.** `forth_inspect_word` returns a pointer into
  `source_space` and `forth_module_source` a pointer into the pool. Both become
  copy-out calls, and host tools and tests must follow.
- **The link.** Appends share the eZ80↔ESP channel with display traffic.
  Coordinate with the /dev/win protocol work rather than adding another framing.
- **Effort** *(estimate)*: 3–4 days (eZ80 store client, ESP blob service,
  boot-cache and inspect API changes, ESP-reset recovery, board tests).

### 10.3 A compact name pool, eZ80 only (~10 KB)

The 32-byte `name` field is 16,384 B of the 28,160 B header table. The 185
names a resident registers (core, prelude and kernel natives) average **5.8
characters** (median 6, p90 9, max 16; *measured* from the sources).

**Design.** Store names NUL-terminated in a bump-allocated pool and replace
`name[32]` with a 16-bit offset, so a header becomes 55 − 32 + 2 = **25 B**.

- The pool is rolled back with the other counters (one more field in the
  checkpoint and the snapshot base).
- `find()` compares against the pool, at the same cost.
- An optional length byte would let mismatches be rejected before any string
  compare.

**Size**, 512 words:

- headers 12,800 B, plus a pool sized for an average of 10 bytes per name
  (5,120 B), gives 17,920 B;
- against 28,160 B today, that is **~10 KB saved** (*estimate*; module word
  names may run longer than the resident average, so size the pool from the
  board's `WORDS` at the next acceptance).

**Costs.**

- `sizeof(struct word)` changes, so each owner pays one cache-miss boot.
- Every `words[i].name` use (inspect, WORDS, SEE, task and crash reports)
  goes through an accessor.
- A smaller header also makes the per-index `__imulu` (§3.1) cheaper.
- **Effort** *(estimate)*: 1–1.5 days.

### 10.4 Recommendation

1. **Do the name pool (§10.3) first.** It is eZ80-only and low-risk, and the
   ~10 KB it recovers covers the extensible compiler's ~4.5 KB with room for
   /dev/win and G5.
2. **Then, if more is needed, offload the rarely-read data to ESP PSRAM
   (§10.2)**, starting with the module source pool (32 KB, the largest and
   least used).
3. **Keep the compiler on the eZ80.**

## 11. Measurement method

`tools/verification/forth_compile_cycles.py ELF…` runs the linked resident
image under the semantic eZ80 interpreter from `tests/test_vios_native_c.py`,
with `compose_cycles.py`'s cost model (one cycle per bus byte, 18.432 MHz,
~6% below the board). It has two parts:

- **compile:** `forth_init`, then `forth_eval` of a 201-token prelude-like
  source, with a per-function profile.
- **calls:** per-iteration cost of inline `V @`, a colon `V @`, and the DOES>
  child layout (`V DP`, where `DP` is `@`) on DIRECT and TOKEN. DIRECT
  executes the `CALL _fvm_docol` at each colon code field from BSS, which the
  script decodes from memory.

It needs the main checkout's `.tools/agondev` (as `compose_cycles.py` does).

The prototype was a scratch copy of `46d94ce` with one change in
`forth_eval`. Right after the token's source is recorded, it looks the token
up in the dictionary and, if found, compiles or executes it and continues,
before the keyword chain:

```c
{int n=find(s);if(n>=0){if(current>=0){if(!compile_word(n))return 0;}else if(!execute_index((unsigned)n))return 0;continue;}}
```

Built with `make build/vios-resident.bin`, it produced 185,982 B against
186,039 B for the unchanged build, and the same `__bss_end` (0xAAC15).

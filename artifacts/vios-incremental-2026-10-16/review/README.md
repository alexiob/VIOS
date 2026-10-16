# Incremental workbench composition review

Host-only review of the final sparse-row/noinline editor composer. No serial,
hardware, or production source edits are performed by this harness.

Run from the repository root:

```sh
python3 artifacts/vios-incremental-2026-10-16/review/run_review.py
```

The runner extracts `kernel/workbench.c` from the immutable baseline archive
`artifacts/vios-workbench-2026-10-16/source-code.tar.gz` (archive SHA-256
`51013d71ad553990abb2649d03c082dcec8dd2a7e6924be0319c568d7cdd3c65`).
It compiles that source with renamed public functions alongside the current
composer and shared real portable Forth engine. Each composer has a separate
mock 80×60 character/style screen. AddressSanitizer and UndefinedBehaviorSanitizer
are enabled. No production files are rewritten.

`latest-run.txt` records exact compiler and executable invocations and results.
`review-manifest.json` identifies the baseline member, reviewed files, and
harness by SHA-256. `workbench_differential.c` contains the comparison harness.
Temporary compilation files disappear after the run.

The main check starts with an exact 4096-byte mixed CR/LF/tab draft, then runs
30,000 deterministic patch, cursor, focus, and undo operations (seed 7781).
Every third operation compares all 4,800 characters and their attributes with
the original full composer. It also covers live variable value shrink,
DEFER target-name shrink, dictionary growth, DS depth shrink, a changed top
value at unchanged depth, and BASE changes.

## Finding and regression reproductions

One invalidation defect had two manifestations. Before composing again, edits
and cursor movement could leave the byte offset unchanged while changing the
cursor's visual row or column. The old predicate only compared byte offset and
focus, leaving an old highlight or omitting the new one. The fixed predicate
also checks source damage, recomposing the old and new cursor rows.

1. Row shift: focused source `a\n\n`, cursor 3; compose. Insert a tab at offset 1,
   then move DOWN. The cursor remains byte 3 but moves from source row 2 to row 1.
2. Column shift: focused source `a\nb`, cursor 3; compose. Insert `x` at offset 0,
   then move DOWN and LEFT. The cursor remains byte 3 and source row 1 but moves
   from column 1 to column 0.

Both exact scenarios pass against the current code. The runner also reconstructs
just the original defective predicate in a temporary copy of the latest source;
both cases must then report a differential mismatch with exit code 2. These
negative controls are explicitly reconstructed, not historical source snapshots.
`original-cursor-red.txt` preserves the parent's original failing native test log.

No further material correctness findings were found in the latest reviewed code.
Sparse character/style runs preserve the full composer's output and cursor style.
`WB-COMPOSE` separately measures composition while preserving the inspected DS;
the native benchmark addresses the earlier TIMER@ stack-perturbation caveat.
Runtime profiling and the benchmark were inspected; the differential executable
covers the portable compositor, not UART, VDP timing, or the runtime command.

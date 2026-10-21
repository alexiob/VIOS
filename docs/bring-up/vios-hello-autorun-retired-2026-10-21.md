# HELLO's autorun retired (2026-10-21)

The HELLO demo module (`apps/hello-autorun.fth`, installed 2026-10-19 to
test the `AUTORUN` boot convention) printed `hello: heartbeat N` every 10 s
from a console task started at every boot. Its output mixed into typing and
test transcripts, so on the owner's request it no longer starts.

VIOS cannot unload a module, so HELLO version 2 replaced version 1: the
same `HELLO-BEATS` and `HELLO-BEAT`, no `AUTORUN`. Steps, through the
harness on the 0.8.0 release build:

1. `1 TASK-KILL` (the running heartbeat), `module_stage HELLO 2`,
   `module_activate HELLO`.
2. `boot_trial`: slot B got LIVE1I, LIVE1P, LIVE1D (11), WBM1P (1.0) and
   HELLO 2; slot A kept the same with HELLO 1.
3. A full reset (reopening the USB port): `boot trial confirmed B`,
   `BOOT-STATUS` = `boot confirmed B candidate NONE state CLEAN current B
   generation 19`, no `autorun task` line, task 1 free.

To bring the demo back, stage `apps/hello-autorun.fth` as HELLO 3 and run
`BOOT-TRIAL` again; `BOOT-REVERT` before confirming returns to the slot
before it.

# VIOS session report, 2026-10-18/19

Board `/dev/cu.usbserial-224130`; only ESP app0 was written (each image and
its manifest/ELF kept in `backups/app0-2026-10-18/`, original app0 backed
up and verified first). SD boot slots, eZ80 flash, bootloader and
partitions untouched. Oct 16 RAM work (WBM1P + draft) is RAM-only and is
restored with `tools/vios_fast_display_state.py` after flashes.

## Delivered (all on-board verified unless marked host-only)

| Area | What | Evidence |
| --- | --- | --- |
| Console | VGA REPL console at boot, F1 <-> editor, line editing + history, blinking cursor, preserved output, hidden harness framing | `vios-console-acceptance-2026-10-19.json` |
| Keyboard | Physical lines isolated from host frames; echo; root cause of "keyboard not working" (closed workbench, USB-only REPL) | `vios-keyboard-2026-10-18.md` |
| Tasks | 4 preemptive Forth tasks, PAUSE/MS, gate, slice sweep (default 1024), SPAWN-CON, AUTORUN (host-tested boot path) | `vios-tasks-acceptance-2026-10-18.json`, `vios-task-slice-sweep-2026-10-19.json` |
| Handles | OPEN/CLOSE/READ/WRITE/PIPE, /dev/null|cons|tasks, ownership cleanup | `vios-handles-acceptance-2026-10-18.json` |
| Pipelines | TASK-INPUT/TASK-OUTPUT, PIPELINE, backpressure, broken pipe | `vios-pipeline-acceptance-2026-10-19.json` |
| Inspection | LAST-ERROR pre-unwind capture, TRACE on all engines, source maps (host-tested, flashed with this report's image) | `vios-trace-2026-10-19.log` |
| Core words | prelude words, ." CHAR CREATE ALLOT HERE , C,, J LEAVE +LOOP ?DO UNLOOP, FILL MOVE U. | `vios-core-words-2026-10-19.json` |
| Host tools | snapshot, resync, complete/help, module_graph, develop, describe, events, sd_list/put/get, trace, last_error | 216 host tests |
| Soak 1 | 342 clean mixed-load iterations (31.9 min, 0 console drops), then 3 bytes lost inside one 1-char USB console frame (open issue #2); harness failed closed | `vios-soak-2026-10-19.json` |
| Echo coalescing | per-character echo held while input streams; REPL round trip 77.6 -> 35.9 ms; `resync` recovers wire loss without reboot | `vios-tasks-regress-2026-10-19.json` |
| Soak 2 | rerun on the coalescing firmware | `vios-soak-2026-10-19b.json` |

## Questions and physical tests for the owner

1. **Console on the monitor:** blinking cursor visible? Left/Right/Home/End,
   Backspace, Delete, Esc and Up/Down history behave as expected? Any
   flicker or wrong colours?
2. **Held keys / repeat:** hold an arrow and a letter in the console and in
   the editor; does repeat work without lost or doubled keys?
3. **Merge** `refs/heads/claude/frosty-faraday-fa0037` (BOOT-MANIFEST, real
   `persisted` in snapshots, +928 B)? It conflicts in 9 files with today's
   work; I would resolve and retest.
4. **SD boot-slot demo:** may I install `apps/hello-autorun.fth` as a module
   and write a boot trial (changes the inactive SD source slot; revertible
   with `boot_revert`) to show reboot-into-application on the board?
5. **Persist WBM1P?** The Oct 16 module and draft are RAM-only and lost on
   every power cycle. Should I `SAVE` it and include it in the boot slot?
6. **Cold start / power-cut tests** need you to unplug and replug the board;
   when convenient.
7. **Priorities next:** pipelines throughput (EMIT/KEY trap cost), more
   standard words, Plan 9-style namespaces, networking broker, or 6502 port
   planning?

## Remaining limits

CPU image ~144 KB, BSS end `0xA5E48` (~37 KB below the stack): new RAM
features now need explicit budgets. Background tasks share BASE and the
dictionary and have no locks; transactions are foreground-only. Pipelines
move ~2 KB/s (per-byte trap cost). Source maps hold 640 entries; later code
is unmapped. Physical cursor/editing/history checks and long power-cycle
qualification are not done.

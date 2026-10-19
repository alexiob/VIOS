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
| Devices | Forth words serve `/dev/NAME` (`DEVICE`), Plan 9 style | `vios-devices-2026-10-19.log` |
| Startup | `/VIOS/USER.FTH` loaded at boot when present; failures reported, boot continues | `vios-user-startup-2026-10-19.log` |
| Review | independent code review: 6 findings, 5 fixed (task exit drain, MS clock, ring headroom, echo redisplay, VGA backspace wrap) | handles spec |
| Inspection | LAST-ERROR pre-unwind capture, TRACE on all engines, source maps (host-tested, flashed with this report's image) | `vios-trace-2026-10-19.log` |
| Core words | prelude words, ." CHAR CREATE ALLOT HERE , C,, J LEAVE +LOOP ?DO UNLOOP, FILL MOVE U. | `vios-core-words-2026-10-19.json` |
| Host tools | snapshot, resync, complete/help, module_graph, develop, describe, events, sd_list/put/get, trace, last_error; CLI :ls :put :get :events :describe | 220 host tests |
| Soak 1 | 342 clean mixed-load iterations (31.9 min, 0 console drops), then 3 bytes lost inside one 1-char USB console frame (open issue #2); harness failed closed | `vios-soak-2026-10-19.json` |
| Echo coalescing | per-character echo held while input streams; REPL round trip 77.6 -> 35.9 ms; `resync` recovers wire loss without reboot | `vios-tasks-regress-2026-10-19.json` |
| Soak 2 | **passed**: 558 iterations / 45.1 min, 0 console drops, no wire loss (median iteration 4.8 s vs 5.5 s before). The VGA view's 512-byte ring dropped display-only output under continuous host traffic (expected; USB stream intact) | `vios-soak-2026-10-19b.json` |

## Owner decisions (2026-10-19) and what followed

| Decision | Outcome |
| --- | --- |
| Console cursor and editing keys work on the monitor | confirmed by the owner |
| Merge BOOT-MANIFEST | merged (95b2592) |
| Persist WBM1P | saved; boot trial B confirmed after reboot (eecba9f) |
| Mac files over USB (`/host/`) | implemented and hardened with request ids and bounded replies (d3c923b, d95e070) |
| ESP app1 for firmware A/B | `tools/vios_firmware_ab.py`; marker-based trial because this board's bootloader erases NEW entries (71786fd); see stage 6/7 spec |
| Boot speed | 1 us ZDI edges plus an on-CPU CRC32 stage: CPU health at 11.3 s instead of 66 s (56fee8b, e8f5c59) |
| GUI toolkit stages 0-1 | design `docs/superpowers/specs/2026-10-19-vios-gui-toolkit.md`; Stage 0 baseline (fb77d31), Stage 1a USB drain off the VDP parser (19d15c8), Stage 1c console on the ESP in progress |
| Cold-start / power-cut tests | owner will run them |
| Second architecture (6502) | on hold |
| HELLO autorun demo | **installed**: module HELLO 1 staged/saved, boot trial written to slot A, reboot ran it, confirmed (generation 15); AUTORUN runs as task 1 with a 10 s heartbeat. `n TASK-KILL` stops it; `boot_revert` returns to slot B |
| Boot to the GUI | on the board (image gui-boot-9): mode 0, GUI first, Ctrl+Alt+F1 text console; fixes for a boot crash, mode 0 memory and the frame budget (9a87e0d) |
| Stage 2a (ESP line editing) | on the board: key-to-console 0.125 ms idle and under load (was 27-34 ms under load) |
| GUI soak | passed: 387 iterations, 30.1 min, 0 failures, 0 drops |
| GUI acceptance on the board | **passed** (image gui-boot-10, `tools/vios_gui_accept.py`): window open/move/raise/close with pixel checks, Ctrl+S accelerator -> COMMAND event, input readback, button ACTIVATE, 0 protocol errors, vblank->start 87 us |
| GUI under console flood | 6000 numbers in 2.95 s; compositor step <= 3.65 ms (time budget), frame work <= 4.6 ms, vblank->start <= 95 us, 0 errors |
| Untethered boot (U6) | ESP user mode works without the wire, but this board's Quark MOS never talks to the VDP, so the SD boot stub never runs; the wire is back. Owner to choose: diagnose MOS, update MOS, or a flash loader |

Every ESP image since the A/B work was installed as a trial and confirmed;
one (the first CRC build) failed its trial and rolled back by itself, as
designed.

## Open questions for the owner

1. Replacing the ESP bootloader would enable the standard ESP-IDF rollback
   (covers images that crash before VIOS code runs). Recoverable over USB;
   best done with you at the board.
2. After Stage 1c the console text is held on the ESP: an ESP restart loses
   the on-screen history (the USB log keeps it). Accepted in the GUI decisions.

## Remaining limits

CPU image ~144 KB, BSS end `0xA5E48` (~37 KB below the stack): new RAM
features now need explicit budgets. Background tasks share BASE and the
dictionary and have no locks; transactions are foreground-only. Pipelines
move ~2 KB/s (per-byte trap cost). Source maps hold 640 entries; later code
is unmapped. Physical cursor/editing/history checks and long power-cycle
qualification are not done.

## Black bars at the top of the GUI (diagnosed 2026-10-19)

Probes on the board (image gui-boot-12): `esp_stats` `vga_isr_gap_max_us`,
`vga_isr_late` (VGA16 line-ISR timing) and `top_black_frames` (GUI frames
with colour 0 in the top 32 rows, where the desktop is colour 5).
Data: `vios-vga-isr-2026-10-19.json`, `vios-top-black-2026-10-19.json`.

- Line interrupts are never late: the longest gap is the vertical blank
  itself (1,477-1,486 us) in every workload, flood included.
- No black reaches the top rows when idle, in a 10,000-number console flood,
  or with ordinary window open/move/close (0 frames).
- Black appears only while the workbench opens and closes (21 of 129 frames).
  The workbench is a full-screen window whose blank background is black; the
  compositor's 3 ms frame budget repaints it top to bottom over ~13 frames
  (~0.2 s), so a black band sweeps down. The soak opens and closes the
  workbench every iteration, which is what was seen during the soak.
Fix (owner's choice: off-screen first; firmware-offscreen-6, b850f2b and
follow-up): `vios_offscreen.h`. When at least half the screen is damaged,
the frame task copies the screen into a 150 KB PSRAM shadow over ~5 frames
(composition paused, nothing changes on screen), composes the repaint there
under the usual 3 ms budget, then copies the written rows to the screen in
one go. Scrolls recompose instead of blitting during a pass (`vds.no_blit`:
row copies inside PSRAM cost ~10 ms). A 30-frame cap presents what is done
if damage keeps arriving (rapid open/close), then draws directly until the
damage clears. Board results (`vios-top-black-offscreen-2026-10-19.json`,
`vios-gui-accept-offscreen-2026-10-19.json`):

- A single workbench open or close is one present; no sweep.
- The present is one long frame: 9-14 ms (PSRAM read bound; the copy runs
  ahead of the beam, so no tear). It is reported apart as
  `present_frames`/`present_work_max`; GUI acceptance budgets it at 15 ms.
- Steady frames stay within budget: <= 4.5 ms in every workload (idle,
  5x2000 console flood, workbench cycles, window ops); vblank start <= 96 us.
- GUI acceptance passes (3 of 3 on offscreen-6); the earlier intermittent
  7-8 ms frames were a present landing in the run (now budgeted apart) and
  console scrolls blitting in PSRAM (now recomposed).
- New counters: `esp_stats` frame_lock_max / frame_service_max /
  frame_gui_max (where a long frame goes); GUI status presents, forced,
  abandoned, present_us_max and the slowest step's phase.

# GUI G1 on-board integration: mode 0, frame task, console on the blitter (2026-10-19)

Scope (split with the session building the portable compositor in
`firmware/esp32-vios/lib/vios-vds/`): the ESP/fabgl side. Installed as A/B
trials and confirmed; final build `backups/app0-2026-10-18/firmware-g1-kick.bin`.

## What changed

- **VIOS display mode (VDU 23,0,E6,0):** 640x480, 16 colours (VDP mode 0) with
  logical 0-3 pinned to mode 1's black/red/yellow/white in both the controller
  palette and the VDP's `palette[]` (so `COLOUR` and `CHAR@` agree). The CPU's
  `display_reset` uses it instead of `VDU 22,1` and requires 80x60x16 mode 0.
  `VDP-INFO` now reports `colours 16 mode 0`.
- **Console on the 4 bpp blitter:** in mode 0 the ESP console writes glyphs
  straight into framebuffer rows through two 1 KB lookup tables; scrolling
  rotates the controller's row-pointer table instead of copying 146 KB;
  show/clear mark rows dirty at full width instead of clearing 150 KB. Other
  modes keep the fabgl path.
- **Bounded, deferred drawing:** the terminal keeps dirty column spans and
  pending scrolls; `service(budget)` draws in beam order (top to bottom).
- **Frame task (`vios_frame.h`):** the VGA16 line ISR calls a hook at vertical
  blank (`vendor/vdp-gl` `vga16controller.cpp`); a task on the VDP core at
  priority 2 runs each frame (console drawing within 2,800 cells, blink). It
  preempts the VDU parser even while that waits for CPU bytes. Echo-sized
  updates (<= 160 cells, no scroll) wake it at once instead of at the next
  vblank. A recursive display lock serializes the terminal, the framebuffer
  and every VDP mode change.
- **VDP->CPU writes no longer stall:** UART2 has a 1 KB TX ring and
  `vios_vdp_tx_end` no longer flushes (cpu_halt still flushes before halting).
- **ESP glyph fast path** and stats: `esp_stats` adds per-section loop maxima,
  frames, vblank->start and frame work.

## Measured (board)

| Measure | Before G1 work | Now |
| --- | --- | --- |
| Frame rate | n/a | 60 fps steady |
| vblank -> frame start (max over all scenarios) | n/a (loop polled: up to 66 ms) | **<= 100 us** |
| Frame work max (full console redraw) | 66 ms in one VDU command | **1.7 ms** (~1,700 cells/ms) |
| VDP->CPU forwarding max | 5-8 ms | 0.3 ms |
| Idle key-to-glyph median / p95 | 1.0 / 1.3 ms (Stage 1c) | 1.3 / 1.5 ms |
| Two busy tasks, 20 cps median / p95 | 26 / 33 ms | 27 / 34 ms (CPU-bound; Stage 2) |
| Console flood (1000 numbers) | all drawn, 7,058 glyphs/s | all drawn, 7,594 glyphs/s |
| Editor <-> console round trip | 350-450 ms | ~290 ms |
| Console acceptance | pass | pass |

The VDP loop gap itself (parser waiting inside a command for the CPU's next
bytes, up to ~5.8 ms) no longer matters for frames: the frame task preempts
it. Data: `vios-gui-g1-frame-2026-10-19.json`, `vios-gui-g1-loop-sections-2026-10-19.json`,
`vios-ui-g1-2026-10-19.json`, `vios-console-g1-2026-10-19.json`.

## Not done here

The `vios-vds` compositor (windows, surfaces, styles) is not yet wired into
the frame task; its frame step can run inside `vios_frame_task` under the
display lock. Mode changes via user `MODE` still reallocate the framebuffer
under the lock; returning to the console needs `WB-CONSOLE`/`WB-REDRAW`.

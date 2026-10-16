# VIOS text workbench — Agon measurements, 2026-10-16

Historical first-milestone measurements. The later [incremental compositor
measurements](incremental-2026-10-16.md) supersede the composition bottleneck below.

Measured on the physical Agon Light 2/eZ80 at 18.432 MHz, with the pinned VDP 1.04 and 115200-baud UART. The populated fixture has a 1,969-byte source draft with 50 comment rows. No host emulation numbers appear in this table.

| Operation | Wire bytes | Cells / spans / batches | Native render ms | Host measurement ms |
|---|---:|---:|---:|---:|
| full repaint | 4123 | 1547 / 368 / 33 | 503.274 | 1604.400 |
| unchanged | 0 | 0 / 0 / 0 | 0.000 | 741.302 |
| one character edit | 32 | 4 / 4 / 1 | 4.215 | 1367.492 |
| browser selection | 39 | 11 / 4 / 1 | 4.760 | 1024.962 |
| source scroll one row | 370 | 69 / 43 / 3 | 50.809 | 1079.170 |

Target time sums native render calls with the 288 kHz 16-bit downcounter, using <=128-wire-byte batches. Each normal measured batch must finish within the 227.56 ms timer period. The sum excludes composition, observer snapshot/dispatch overhead outside the callback, idle gaps, console formatting and host transport. Host elapsed includes mutation/status/flush/fence/stats round trips; it is not one keyboard-event latency. An ordered `CHAR@` reply fences outgoing graphics. Counter totals include idle batches completed before the explicit flush.

The full repaint sends only changed/non-default cells after CLS, not all 4,800 cells. The bottom-right cell stays blank to prevent scrolling. A one-character edit also updates cursor/revision cells, accounting for four changed cells and four spans. The full repaint has a wire minimum of 357.899 ms at 8N1. Unchanged rendering emits no glyph bytes.

The 60-byte dirty-row index reduced the same 32-byte edit render from 100.906 ms to 4.215 ms (95.8% less). It skips clean rows instead of searching the full screen. Source-scroll rendering fell from 139.465 ms to 50.809 ms. The old run is diagnostic evidence only: its later framebuffer check caught an incorrect palette setup, corrected in the accepted run.

**Remaining bottleneck:** full model composition costs a median 50,416 timer ticks (175.056 ms); an independent 32-bit VDP-clock bracket measured 177 ms including query overhead. The first milestone still rebuilds the complete desired frame at command/key boundaries. This bounds local edit feedback at roughly 180 ms for this fixture, before keyboard scheduling/queueing. Incremental pane/line composition is the next performance task. Duplicate command-boundary composition was removed, and empty pending checks are constant time.

The actual 355-request acceptance passed eight framebuffer character comparisons, exact-source round trips, stale revision rejection, failed-stage state preservation, and broken-paint-hook repair. Fifty 235-byte source requests interleaved with redraws then passed with zero receive drops. Physical keyboard input changed the snapshot during a read, correctly causing a consistency rejection; a subsequent stable snapshot passed. The user confirmed the VGA display shows the inspector and the keyboard works.

The dictionary expansion was exercised to 391 live entries on hardware, including execution tokens above 255 in C, TOKEN and DIRECT. The default capacity is 512, configurable per build. At 55 bytes/entry plus three larger bucket tables it adds 21,696 BSS bytes over the prototype capacity.

The VM benchmark with the workbench open passed 45 conditions × 9 samples, matching results and affine scaling across all engines. DIRECT takes 11.1–14.0% less time than TOKEN and is 4.31–6.13× the C reference on these five workloads. Whole-call DIRECT times increased 0.56–1.24% versus the preceding resident build; 50-to-100-iteration slopes differ by at most one timer tick across all engines/workloads. This is consistent with fixed entry overhead, with no per-iteration regression detected. Native engine source remains byte-identical.

Evidence: [render report](../bring-up/vios-workbench-hardware-run4.json), [extra acceptance](../bring-up/vios-workbench-extra-acceptance.json), [render summary](workbench-2026-10-16.json), [VM samples and slope comparison](workbench-vm-2026-10-16.json).

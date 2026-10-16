# Source scrolling and newline composition — Agon, 2026-10-16

Measured on the physical Agon Light 2/eZ80 at 18.432 MHz, VDP 1.04, mode 1 (80×60), UART 115200 8N1. Final results are from the installed ESP32 application after resident startup without a RAM upload.

## Composition results

The original 1,969-byte, 50-comment-line fixture is unchanged. Both versions use `WB-COMPOSE`, which preserves the inspected DS and measures only composition. The interval excludes mutation/index maintenance, dispatch, observer setup, VDP output, command serialization and host transport. Medians of nine samples:

| Operation | Previous ms | New ms | Reduction |
|---|---:|---:|---:|
| Unchanged view | 1.038 | 1.038 | 0.0% |
| Cursor movement | 2.625 | 1.469 | 44.0% |
| Character edit | 8.503 | 7.628 | 10.3% |
| Newline insertion/removal | 72.861 | 25.264 | 65.3% |
| Browser selection | 11.156 | 11.156 | 0.0% |
| Source scroll one row | 67.938 | 19.323 | 71.6% |
| Forced full reconstruction | 164.083 | 128.212 | 21.9% |

The additional 2,128-byte fixture alternates tabs and spaces for indentation and mixes LF/CRLF endings. Seven-sample medians: source scroll **34.476 ms**, newline composition **39.816 ms**. No previous hardware result is claimed for this new fixture.

The 288 kHz timer is a 16-bit downcounter with a 227.56 ms period. Independent 32-bit VDP-clock brackets measured full composition at 132ms (plain) and 147ms (mixed), including query overhead, below the safe limit. The driver refuses unsafe full-frame timer sampling.

## What changed

Indexed line endpoints identify the bytes belonging to a row. For a visible prefix without tabs, a bounded `memchr` check and `memcpy` replace per-character projection; the Agon library uses native block/scan instructions. Tabs retain the general expansion path. CR, LF, CRLF, cursor positions and horizontal clipping preserve prior screen output.

Sparse row differences now walk glyph buffers with byte pointers. Old/new cursor style repairs happen outside that hot loop. Painting cannot run until composition returns, so intermediate desired-cell updates never reach the display. The existing retained screen and row cache are reused; no new data buffers or heap were added.

CPU payload 105,706 B (+474 B), BSS 254,031 B unchanged, separate 4 KiB stack and 94,919 B remaining before it. ESP app0 is 663,952 B (+480 B), static ESP RAM 37,948 B unchanged. UART flow control, command priority, observer execution and service hooks are unchanged.

## Serial rendering remains separate

The comparable plain fixture still transmits the same bytes; source-scroll rendering remains about 51 ms for 380 bytes. The composition improvement does not remove this serial/VDP cost. Host values below include several status/mutation/flush/fence/stats requests, not a single physical keyboard-event latency. `CHAR@` provides an ordered VDP-consumption fence.

| Operation | Output bytes | Native rendering ms | Host sequence before / after ms |
|---|---:|---:|---:|
| Full repaint | 4131 | 504.486 | 1004.5 / 977.1 |
| Unchanged | 0 | 0.000 | 134.2 / 132.6 |
| Character edit | 32 | 4.212 | 209.4 / 206.3 |
| Source scroll | 380 | 51.604 | 309.5 / 253.0 |

## Verification and control

163 host/native/sanitizer tests and 146 linked-engine probes pass. The historical differential runner checks 30,000 operations from a 4096-byte draft; an independent boundary review checks 24,959 cursor probes, comparing all 4,800 glyph/style cells after each composition. Cases include tabs around viewport edges, CRLF/NUL cursors, blank/reset rows, changed cursor glyphs, coalesced edits and focus changes.

Both RAM and resident hardware runs pass 355 requests with 8 actual framebuffer glyph checks and 49 focused requests including variable/deferred-target/stack shrink, BASE preservation, reset equivalence and pending KEY continuation. Each plain/mixed benchmark interleaves 45 maximal 235-byte requests with 5 redraw requests, with zero receive-drop growth. The counts are 50 total stress requests per run, rather than 50 maximal lines plus redraws.

Actual human CLI and 48-tool stdio MCP share one serial owner. Starting draft source is restored exactly, together with its dirty flag, cursor, browser selection, focus and original closed-workbench state. Source boot remains confirmed A generation 7. No eZ80 flash or SD startup writes. The prior app0 was freshly backed up and hash-verified before the application-only update.

Evidence: [plain native samples](source-rows-resident-native-2026-10-16.json), [mixed samples](source-rows-resident-mixed-2026-10-16.json), [preceding native samples](incremental-accepted-native-2026-10-16.json), [resident hardware checks](../bring-up/vios-source-rows-resident-hardware.json), [focused checks](../bring-up/vios-source-rows-resident-focused.json), [CLI/MCP and restored state](../bring-up/vios-source-rows-control.json).

After preserving current user work, reproduce through the existing TCP owner (these drivers replace the current draft with an unsaved fixture):

```sh
.venv/bin/python tools/vios_incremental_bench.py --native --samples 9 \
  --output /tmp/vios-source-plain.json
.venv/bin/python tools/vios_incremental_bench.py --native --fixture mixed --samples 7 \
  --output /tmp/vios-source-mixed.json
```

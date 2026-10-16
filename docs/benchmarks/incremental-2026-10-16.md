# Incremental workbench — Agon measurements, 2026-10-16

These measurements describe the retained compositor milestone. Later
[source-row optimization](source-rows-2026-10-16.md) supersedes its scrolling
and newline composition costs without adding static RAM.

Measured on the physical Agon Light 2/eZ80 at 18.432 MHz, VDP 1.04, 115200-baud UART, 80×60 mode 1. The 1,969-byte fixture contains 50 numbered comment rows. The final measurements below are from the installed ESP32 application, after resident startup without a RAM upload.

## Composition alone

The new `WB-COMPOSE` command brackets only the C compositor with the native 288 kHz timer. It preserves DS/BASE; output, command dispatch, painting, observer overhead and host transport are outside the interval. Medians of 9 samples:

| Operation | Native composition ms |
|---|---:|
| Unchanged view | 1.038 |
| Cursor movement | 2.625 |
| Character edit | 8.503 |
| Insert/remove newline | 72.861 |
| Browser selection | 11.156 |
| Source scroll one row | 67.938 |
| Forced full rebuild | 164.083 |


The prior workbench rebuilt the entire desired frame at each command/key boundary (approximately 175 ms in the original milestone). Ordinary updates now retain panes, source line offsets, cursor measurements and dynamic inspector row contents. Changed source rows emit only differing characters/styles. Source viewport shifts and newline changes still compose the visible source rows. An exploratory 50 ms viewport-composition target remains unmet; the measured 67.938 ms scroll is a remaining optimization opportunity.

A full rebuild remains necessary for open/reset/explicit invalidation. Its 32-bit VDP-clock cross-check was 168 ms including query overhead. The 16-bit timer wraps at 227.56 ms; the driver skips full-frame timer samples if the independent cross-check is>=215 ms. No wrapped full-frame value is reported as a short operation.

## Strict old/new comparison

The legacy timing probe places its initial timer value on DS while `WB-VIEW 0 0` composes. That changes the inspector stack row, so the “unchanged” case below includes an inspector refresh. Keeping this identical probe on both images gives comparable 7-sample medians; it is separate from the stack-preserving numbers above.

| Operation | Before ms | After ms | Reduction |
|---|---:|---:|---:|
| Unchanged view | 171.056 | 7.420 | 95.7% |
| Cursor movement | 171.115 | 9.007 | 94.7% |
| Character edit | 171.083 | 14.767 | 91.4% |
| Insert/remove newline | 167.913 | 79.184 | 52.8% |
| Browser selection | 167.722 | 17.538 | 89.5% |
| Source scroll one row | 180.604 | 74.319 | 58.8% |
| Forced full rebuild | 178.340 | 165.608 | 7.1% |


## Serial rendering and end-to-end harness work

The comparable run records the same output sizes on both images. Native render time sums<=128-byte default paint calls and excludes composition, observer setup, idle gaps and host work. An ordered `CHAR@` reply fences VDP consumption. Host elapsed includes multiple mutation/status/flush/fence/stats requests; it is not physical keyboard latency.

| Operation | Wire bytes | Native render ms | Host sequence before/after ms |
|---|---:|---:|---:|
| Full repaint | 4131 | 504.358 | 1666.6 / 1009.7 |
| Unchanged | 0 | 0.000 | 778.4 / 135.4 |
| Character edit | 32 | 4.215 | 936.1 / 208.6 |
| Selection | 51 | 6.615 | 939.3 / 200.3 |
| Scroll | 380 | 51.569 | 1052.5 / 303.0 |


The initial retained-row candidate still wrote whole changed source rows (143 ms scroll composition). Sparse runs reduced it to 120 ms. Preventing the eZ80 compiler from inlining the row builder reduced it further to 68 ms and removed 1,067 bytes of generated code. Skipping redundant rectangle clears after an already cleared full frame reduced full reconstruction from 235 ms to 164 ms. The accepted row builder keeps per-character state in a smaller stack frame.

## Resources and verification

CPU payload 105,232 B (+6,962 B), BSS254,031 B (+13,203 B), with 95,393 B before a separately reserved 4 KiB stack. The line offset index is 8,194 B at the 4096-byte source bound. Retained source/inspector caches account for most remaining growth; no third full screen plane or heap was added. ESP app 0 is 663,472 B (+6,960 B); static ESP RAM remains 37,948 B.

Verification:163 host/native/sanitizer tests,146 linked-engine probes,355 resident hardware requests with 8 actual framebuffer glyph checks,49 focused resident requests (cursor regressions, variable/deferred-target/stack shrink, BASE, reset equivalence, pending KEY), actual shared CLI and 48-tool stdio MCP, and 50 maximal 235-byte requests interleaved with redraws in each benchmark run. The final resident runs report 0→0 console drops. The old baseline started with 63 historical drops but remained 63→63 during its stress test. Source boot A generation 7 and original eZ80 flash are preserved.

Independent differential review compares every character/style against the immutable original full composer, including 30,000 operations beginning at the 4096-byte limit and negative controls for two cursor highlight defects. Current output matches; the reconstructed defective predicate fails both exact reproductions.

The original live draft was copied to host before deployment and restored with exact source, dirty state, cursor and focus. Additional test words are RAM-only; startup bundles were not rewritten.

Evidence: [native samples](incremental-accepted-native-2026-10-16.json), [comparable after samples](incremental-accepted-comparable-2026-10-16.json), [before samples](incremental-baseline-2026-10-16.json), [resident hardware](../bring-up/vios-incremental-resident-hardware.json), [focused control](../bring-up/vios-incremental-focused-resident.json), [shared CLI/MCP](../bring-up/vios-incremental-control.json).

Reproduce with one TCP owner and a preserved draft (these drivers replace it with a RAM fixture):

```sh
.venv/bin/python tools/vios_incremental_bench.py --native --samples 9 \
  --output /tmp/vios-composition.json
.venv/bin/python tools/vios_incremental_accept.py \
  --output /tmp/vios-live-inspection.json
```

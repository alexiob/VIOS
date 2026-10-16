# VIOS text workbench implementation ledger

The approved first milestone combines a module/word browser, source editor and
live inspector in an 80×60 text workbench. The USB CLI remains the evaluation
workspace; the same draft operations are exposed to host MCP clients.

## Dictionary capacity

The original compiler used 128 entries to bound prototype static storage. This
was a compiler resource policy, not an Agon hardware or Forth execution limit.
The eZ80 target stores 55 bytes per entry, including a fixed 32-byte name.
The new Agon default is 512 entries and 128 hash buckets, configured through
`include/vios/config.h`. Ports can override the two `VIOS_FORTH_*` macros.
Indices/links remain 16 bits; compiled calls and native dispatch are unchanged.
The larger dictionary and three copies of the bucket table add **21,696 bytes**
of target BSS. This is accounted separately from the workbench's 48 KiB budget.
Token code, direct code, data and retained source keep their separately reported
budgets. Increasing entry capacity does not promise that all possible 512-word
programs fit those other arenas.

Verified with sanitized real compiler tests: 350 additional live words compile
and run, exhaustion at 512 recovers, and an independently compiled 64-entry/
16-bucket profile reports its actual budget and recovers from exhaustion.

## Implementation evidence

Structured inspection/observer, model/editor, dirty renderer and keyboard demux
have real-C sanitizer coverage. The native observer snapshot is 888 bytes;
model BSS 8,707 bytes and renderer BSS 19,219 bytes before controller integration.
Model mutations check both draft revision and catalog identity. Physical keyboard
events use a bounded queue separate from the serial command byte stream.

Hardware deployment and rendering benchmarks pass; the verified resident app0 embeds the accepted CPU image. Source boot remains confirmed A, generation 7.

## First hardware finding and fix

The first verified RAM image booted with 80/512 words and preserved confirmed
slot A, generation 7. During the second acceptance attempt, painting completed
but the next request stopped after 14 echoed bytes. Independent ZDI reads found
the CPU in console receive code and the hardware UART overrun counter at 1.
The old idle adapter resumed reception before a full 4,800-cell pending scan.
This allowed incoming bytes to fill the 16-byte FIFO while C work ran.

The new input contract leaves RTS paused after keyboard polling/routing and
explicitly resumes it after the complete bounded UI slice, including close and
error paths. The renderer tracks an exact dirty-cell count, so pending checks
are constant time (one extra three-byte target counter). Native tests reproduce
an 80-byte host request during slow C work and verify no overrun, and execute the
target pending routine to confirm it does not read either screen plane.
The original failed RAM image, ELF and map are retained under artifact diagnostics.

Real REPL command echoes also exposed parser ambiguity in source/view/stats
records; host parsers now distinguish lower-case device field names from echoed
numeric/RESET arguments. Fixtures now include real command echoes.

## Accepted milestone

- CPU image 98,270 bytes; BSS 240,828 bytes; 4 KiB reserved stack and 115,558 bytes before it. Incremental workbench/observer/input BSS is 29,050 bytes; the separate capacity increase is 21,696 bytes.
- Renderer uses 19,282 bytes BSS, including the two planes and 60 row counters. The measured edit render fell from 100.906 ms to 4.215 ms, selection 4.760 ms, source scroll 50.809 ms and populated full repaint 503.274 ms. Zero bytes on unchanged frames.
- Full desired-frame composition remains 175.056 ms (177 ms clock crosscheck). Incremental pane/line composition is the next optimization. Duplicate composition was removed, and a batched full-view host read now uses one native command.
- The pinned VDP palette setup initially used a wrong selector. Actual framebuffer testing caught it; mode 1 now retains its matching black/red/yellow/white hardware and cached palette. Eight actual glyph comparisons pass.
- 355 hardware requests pass, preserving stack/BASE through failed staging, stale patches and broken paint observers. Fifty maximum-length requests during repaints show zero UART drops. A 391-word dictionary executes high execution tokens correctly in all three engines.
- Source save/reload survives resident restart. App0 is 656,512 bytes, SHA256 `643a3ef6517077f4910471b44719ada1d246160a9a00b401b169cd7c25ab31dc`; the embedded CPU SHA256 is `36fb5f932f477a182a556ae45f033820d0ce250396718fa0a795d77323383bbe`. ESP static RAM remains 37,948 bytes.
- App0 backup exactly matches the prior accepted resident firmware. No eZ80 flash writes, bootloader/partition table/app1 updates, or source-boot journal changes occurred.
- Resident acceptance passes 14 checks; actual MCP stdio proxy exposes all 48 tools; pending KEY resumes over USB while panes are open. The user connected VGA/keyboard and confirmed visible inspector and apparent keyboard operation. Keyboard navigation during a snapshot caused a correct conflict; subsequent stable full-view read passes.
- The current TCP harness remains on 127.0.0.1:8765 with the WBM1P example loaded, saved and open in the workbench. See `vios-workbench-live-session.json`. The source snapshot PNG is reconstructed from WB-VIEW with the pinned Agon font, not a camera photograph.

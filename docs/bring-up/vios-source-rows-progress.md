# Source scrolling/newline optimization — 2026-10-16

User authorized reducing the remaining 68–73 ms composition time, hardware
benchmarks and responsive CLI/LLM control. This is a bounded extension of the
retained compositor. Parent owns code and serial; reviewer has host-only tests.

- Preserved actual starting draft/status/DS/BASE in vios-source-rows-before.json:
  WBM1P 1969B, dirty, cursor0, browser MODULES selection0, focus browser, closed,
  DS0 BASE10. This differs from the previous session metadata; restore this
  actual current state, preserving exact source.
- Performance RED: prior hardware scroll67.9375ms/newline72.8611ms exceeds the
  exploratory35/40ms budget (build/source-rows-performance-red.json).
- kernel/workbench.c editor_row now uses indexed line endpoints, trim CR/LF,
  and memchr/memcpy for a tab-free visible source prefix. Tabs retain prior
  projection. CRLF-LF cursor quirk is preserved. Sparse glyph differences use
  pointer walks, with old/new cursor styles repaired outside that hot loop.
- No additional BSS, no renderer/UART/scheduling changes. Candidate CPU105706B,
  SHA7e47cda6905b69bcffadb77e53d02317b5a7b8af1e47c6ad049347880abc0753.
- Full163 tests PASS; copied immutable historical differential runner into new
  artifacts/vios-source-rows-2026-10-16/review and it passes30000 max-source
  operations, live inspection and negative controls. Earlier releases untouched.
- Reviewer source_rows_review is checking boundary fixtures separately.
- RAM deploy via existing TCP owner35300 running exec session99314; output
  vios-source-rows-ram-deploy.json. Do not open a second serial connection.
  Existing resident flash remains cf7660CPU/b8d884ESP accepted incremental build.
- PIO build passes; firmware candidate available, not flashed yet.

Remaining: RAM benchmarks, focused checks, native engine probes; fresh app0
backup (current663472B SHA b8d884a1733505de995ee94f5585abaa6ed9ee9986468529cb32dc9fc3fefb2f),
application-only update, resident benchmarks/acceptance, restore actual user
state, archive/evidence/docs. No eZ80 flash or SD boot writes.

RAM readback verified candidate hash. Hardware355requests/8glyph checksPASS;
plain9samples and mixed7samples benchmarksPASS; focused49requestsPASS including
pending KEY continuation. Plain compositionms: scroll19.333, newline25.264;
mixed tabs/CRLF: scroll34.476, newline39.816. Performance budget nowGREEN.
No new receive drops in either50-request maximum-line stress run.
Final suite163PASS including explicit CRLF/tab clipping/blank cursor fixtures.
Independent review adds24959 boundary cursor cases with4800-cell equality.
App0 backup running session11950 after parent stopped serial owner35300.

## Final installed result

Fresh663472B app0 backup matched b8d884a1733505de995ee94f5585abaa6ed9ee9986468529cb32dc9fc3fefb2f.
Application-only flash663952B verified SHAe0ed0d20b2438e26e47404a152e9aeeeb98268f7c52113fcedec6854a8c845ae.
CPU105706B SHA7e47cda6905b69bcffadb77e53d02317b5a7b8af1e47c6ad049347880abc0753.
Payload+474B; BSSunchanged254031; bssend0x97d39; gap94919B before4KiB stack.
Resident startup withoutRAMdeploy confirmed bootA generation7 and values11/12.

Resident355requests/8glyph checks, plain9sample benchmarks, mixed7sample
benchmarks and49focused requests allPASS. Plain native compositionms: scroll
19.323 (71.6% reduction), newline25.264 (65.3%), cursor1.469, character7.628,
unchanged1.038, full128.212. Mixed2128B source: scroll34.476/newline39.816.
No receive-drop growth during45 maximal235-byte plus5redraw requests per run.
Serial rendering remains separate:380B scroll51.604ms,32B edit4.212ms.

Restored actual starting state (not stale previous-session metadata): WBM1P
sourceexact1969B, dirty, cursor0, MODULES selection0, browserfocus, CLOSED.
DS0 BASE10. Three immutable RAM code samples matched the accepted image.
Actual humanCLI and48-tool nc stdio MCP passed through one serial owner.
Current PID41720, execsession46066, vios-source-rows-live-session.json.

Final163testsPASS;146engine probesPASS; historical30000-operation differential
PASS; independent24959-boundary reviewPASS; both reconstructed negative cursor
controls fail as expected. NativeC and VM C reference retained unchanged outside
the workbench row optimization. No unresolved material review findings.

Final report docs/benchmarks/source-rows-2026-10-16.md. New immutable release
artifacts/vios-source-rows-2026-10-16; prior releases/backups unchanged.

# Incremental composition progress — 2026-10-16

User request: incremental pane/line composition, hardware benchmarks, responsive
CLI and LLM control. Plan: docs/superpowers/plans/2026-10-16-incremental-workbench.md.

- Preserved live draft/status/source in vios-incremental-preserved-session.json
  and vios-incremental-before-deploy.json before replacing the draft for tests.
  WBM1P source1969 bytes, dirty, cursor1969, editor focus, WORDS browser index0,
  DS empty, BASE10. Original exact source is in those JSON reports. SD WBM1P
  package /VIOS/A8783CA5/203C0319.VIM remains unchanged.
- Baseline hardware report docs/benchmarks/incremental-baseline-2026-10-16.json
  passed all cases and50 maximum235-byte host lines with no new drops. Old
  unchanged composition171.056ms; full composition175ms from original milestone.
- Renderer rectangular clear and generation APIs implemented, dirty-accounting
  tests pass. Model retained panes/source-line index and scalar inspector
  sampling implemented. Source editor emits changed character/style runs.
- Review reproduced cursor highlight bug when multiple mutations preserve the
  byte offset but alter visual position. Added two literal regression scenarios;
  RED then GREEN; cursor rows now recheck when source damage is pending.
- Sparse scroll test RED then GREEN bounds composed cells under200 for moving
  all visible numbered comment lines, rather than rewriting38x54 cells.
- Explicit WB-COMPOSE profiling command measures composition without putting
  timer values on the inspected data stack. 16-bit timer only valid below227.56ms;
  benchmark uses32-bit VDP clock cross-check before full-frame timer samples.
- Independent review compared against immutable historical compositor:200,000
  mixed operations and30,000 max-draft operations, plus live variable/defer,
  stack/base/catalog cases. Latest sparse-run version also passed30,000 max-draft
  differential operations. No unresolved material findings.
- RAM candidate f0178261821aa43ce7107a096bacd4be118496d4f58393048a4c23f5e3f5d702
  deployed/readback verified; provisional report incremental-candidate*.json.
  It precedes cursor fix/sparse runs/profiling command; not release candidate.
- Final current CPU106223B SHA3e1d707efc74be3e88ba05a59ba7f2ad2705ee161c7c9338666550d5354bd815;
  text106210 data13 BSS254031, bssend0x97f3e, gap94402B before0xaf000 stack.
  ESP664464B SHAb5136b457802f14883bf06c08b6fbaaaa1e76a8c6cd4855d84f9b542fab3d976.
  Latest full suite163PASS (build/incremental-final-checks.txt), native146probesPASS.
- Final RAM deploy running via existing harness. Harness serialowner launch
  exec session16058, TCP127.0.0.1:8765, logvios-incremental-live.serial.jsonl.
  Deploy client exec session30374; output savesvios-incremental-final-ram-deploy.json.
  Do not open a second serial owner. Current ESP flash remains old accepted
  workbench app0 (656512B, SHA643a3ef6...). eZ80 flash untouched.

Remaining: final RAM benchmarks (legacy comparable and --native), focused live
acceptance/KEY/CLI/MCP, fresh currentapp0 backup+hash verification, application-only
update with approved authorization, resident boot acceptance, restore original
draft, final docs and new immutable archive. Never overwrite old archives.

## Accepted resident result

The previous running-status notes above are historical. Final CPU105232B
SHA cf7660c708f01a7956b8ded85424cfb019ffd68f7870527a9dce02f46d81e1b4,
ESP663472B SHA b8d884a1733505de995ee94f5585abaa6ed9ee9986468529cb32dc9fc3fefb2f.
BSS254031, bssend0x97b5f, gap95393B before stack.

The eZ80 compiler inlined editor_row into a265-byte-frame compositor, adding
substantial register spill overhead. Explicit noinline removed1067 bytes of
code and reduced scrolling from120ms to68ms. A final RED→GREEN operation-count
test removed redundant pane clears after full-frame clear, reducing forced
full composition from235ms to164ms. No raw RX/RTS contract changes.

Ruling: the exploratory50ms scroll target remains unmet at67.938ms; retain the
measured improvement rather than extending this change into a new viewport
representation. Ordinary edit/selection updates no longer rebuild a full frame.

Final full suite163PASS (incremental-accepted-checks.txt),146 engine probesPASS,
accepted differential reviewPASS incl30,000 max-draft operations and negative
controls. The freshly backed up app0 hash matched the previous accepted release;
app0-only write completed with data hash verification. Resident boot attached
withoutRAMdeploy, confirmed A generation7, startup values11/12.

Final resident acceptance355requests/8glyph checksPASS; focused49requestsPASS;
legacy comparable7samples/case and native9samples/casePASS; each benchmark's
50 maximal235-byte stress requests passed with0→0 drops. Native compositionms:
unchanged1.038, cursor2.625, character8.503, newline72.861, selection11.156,
scroll67.938, fullfallback164.083. 32-bit clock full cross-check168ms.

Original draft/source/dirty/cursor/focus restored; BASE10 DS0. Three immutable
RAM code samples matched the accepted host image. Actual CLI and nc stdio MCP
(48tools) passed via the same owner. Current session metadata is
vios-incremental-live-session.json; parent harness PID35300, session17073.

One initial focused-driver run failed because its test word WB-IV used the
reserved WB-* command namespace. The driver now uses IC-* names; the known
single888 stack value was inspected and removed before rerunning. The failed
report is retained as diagnostic evidence. No core defect was inferred from it.

New release is artifacts/vios-incremental-2026-10-16; earlier releases and
backups were preserved. See benchmark/runbook for exact costs and limitations.

# Incremental workbench composition

The user authorized this optimization and on-board benchmarks. Retain the
existing 80×60 layout, editor semantics, live inspection, CLI and MCP surface.
Replace unconditional screen clearing and reconstruction with retained pane
and source-line composition. Measure CPU composition separately from VDP output.

1. Add bounded rectangular clearing and a screen generation counter. Reset and
   explicit full frame clearing invalidate compositor caches. Verify dirty
   accounting, clipping, pending repaint and reserved bottom-right behavior.
2. Cache browser state and source line positions; update affected rows only.
   Compare inspector/footer row contents so arbitrary live Forth changes remain
   visible. Screen reset forces full composition. Test full-output equivalence,
   CR/LF/CRLF, tabs, scrolling, edits before painting, undo and catalog changes.
3. Build and run the complete suite and linked engine checks. Preserve the live
   draft, RAM deploy, then measure unchanged, cursor, edit, selection, scroll and
   reset composition on the Agon. Stress 235-byte CLI requests and pending KEY.
4. Back up current ESP app0, embed the verified image, update app0, reconnect to
   resident boot and repeat focused checks. Preserve boot slots and eZ80 flash.
   Save benchmark evidence, footprint changes and a new immutable release.

Review focus: screen resets; shortened lines; newline edits shifting subsequent
rows; multiple mutations between composition; live variable/deferred target,
stack and BASE changes; UART pause/resume while composition runs.

Execution: parent owns renderer, integration, tests and hardware. A bounded
worker owns kernel/workbench.c and its header. No simultaneous serial access.
Ruling: proceed under the user's explicit instruction; do not repeat approval
gates. This workspace has no Git repository, so no worktree/commit operations.

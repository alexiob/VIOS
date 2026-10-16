# VIOS text workbench — first hybrid milestone

The user approved the hybrid direction and explicitly requested implementation of its first text-pane milestone. This spec makes that approved scope concrete; proceed without further approval gates. Existing resident artifacts/backups remain immutable. This workspace has no Git repository. Only the parent agent owns hardware access.

## Outcome

On the Agon, mode 1 (640×480, four colours, 80×60 cells) shows a module/word browser, module-source editor and live inspector, with focus/commands/status. The serial CLI and host MCP remain available while bounded rendering proceeds. Source editing uses the existing stage/test → activate → save pipeline; editing never silently changes live definitions or SD startup. A new workbench does not automatically select a new boot bundle.

First milestone includes one 4096-byte ASCII module draft, one undo snapshot, cursor/navigation, module/word selection, stack/BASE/resource/deferred-binding inspection, exact source inspection and revision-checked headless editing. No overlapping windows, mouse, suspended image or resumable debugger. The current setup has no display/keyboard; headless VDP readback and injected UI events prove protocol behavior, with physical readability/key mapping explicitly unobserved.

## Architecture and budget

Standard-C model/editor and 80×60 desired/painted character+attribute planes; <=48 KiB incremental CPU BSS. Cell attributes pack foreground/background into one byte. Emit only changed text spans in <=128-wire-byte idle batches. Fixed-size buffers, no heap. Rendering/backend calls remain outside native VM opcode dispatch. Forth exposes a deferred WB-PAINT binding to a native renderer; observer invocation uses a temporary stack and bounded VM budget, preserving application VM state. Structured inspection obtains metadata directly, not by scraping SEE text.

The idle hook runs only at an empty, noncompiling REPL boundary. Serial source has priority; local keyboard packets route to a separate bounded event queue while the workbench is active. Workbench events/commands use the same controller and editor operations as host tools. Unchecked native I/O stalls still require independent supervisor recovery. Existing source-CRC and one-owner transport rules remain unchanged.

## UI and commands

Browser at left, inspector above source editor at right, status/command rows along the bottom. Keyboard focus/navigation and F5 stage/test, F6 activate, F7 save, F8 undo, F10 close; printable input edits the focused source. The view shows dirty/staged/active/saved states and actual errors/capacity limits. Dirty drafts require explicit discard before replacement. Activation validates source/dependencies again; save persists only the activated source. Failed operations preserve the draft and live system. Large drafts can be exported separately before a generation-capacity failure.

Outer WB-* commands provide open/close/status, catalog, selection, module draft load/new/patch/read/undo/stage/activate/save/export, key injection, view rows, redraw and benchmark. Patch requests include the editor revision; stale writes fail without mutation. Host JSON/MCP remains on the Mac. GUI and host commands are serialized on the existing VM owner. Primitive/native words are inspectable; source editing initially operates on source modules rather than pretending all primitives have editable Forth source.

## Acceptance

Host tests exercise real model and renderer, editor bounds/stale revisions/undo/errors, safe escaping, dirty-span and batch limits, keyboard demultiplexing, inspection and observer stack/BASE preservation. Existing tests/native probes must remain green.

Real hardware: attach existing harness, preserve startup files/backups, deploy reviewed RAM image first; verify browsing, editing a temporary module, stage/activate/rebind/rollback, error preservation and concurrent CLI/MCP during painting. Benchmark full repaint, unchanged frame, one-character edit, selection and scrolling; record wire bytes and target timer plus host elapsed time with a VDP reply fence. Re-run VM BENCH for impact. After RAM acceptance, embed the exact CPU in reviewed app0 firmware and verify resident boot/recovery without host upload. Never program eZ80 flash.

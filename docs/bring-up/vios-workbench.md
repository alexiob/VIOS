# VIOS text workbench

The first workbench provides a module/word browser, live inspector and exact
source editor on the Agon Light 2. It uses an 80×60-cell screen. It was built
in VDP mode 1 (four colours); it now runs in the VIOS display mode 0 (640×480,
16 colours) and, under the GUI, opens as a full-screen window that owns the
keyboard. **F1** switches between it and the console. The USB Forth CLI remains the evaluation workspace. Headless clients
operate the same model through the existing host harness; no JSON or MCP parser
runs on the eZ80.

## Start and control

Use the shared resident harness described in [the harness runbook](vios-harness.md).
Only one process owns the board's serial port (`ls /dev/cu.usbserial-*`; pass
`--port`, the suffix can change). A CLI client and multiple MCP
clients share its serialized board session.

```sh
.venv/bin/python tools/vios_cli.py
```

At the CLI, `:wb open`, `:wb status`, `:wb view`, `:wb redraw` and `:wb close`
control the workbench. Normal Forth input remains available. Raw `WB-*` commands
also work at the Forth prompt. Closing preserves the RAM draft.

**Ctrl+Space** toggles the source editor between its 54-column pane and an
80-column full-width pane. Headless clients use `WB-ZOOM 0|1` and `WB-ZOOM?`
through evaluation. The browser and inspector remain above the expanded editor.
`WB-REPAINT` reconstructs the screen without an ordinary VGA mode change;
`WB-REDRAW` retains the full mode-reset recovery path. `WB-FLUSH-FENCED` is the
diagnostic variant that also waits for the ESP framebuffer to complete.
See [fast transport and viewport scrolling](../benchmarks/fast-display-2026-10-16.md)
for timings, buffer reservations and recovery limits.

With a physical keyboard, Tab switches browser/editor focus, arrows navigate,
Left/Right switch modules/words in the browser, and Enter opens a module. Kernel
primitives are read-only; selecting a module word opens its owning source module.
F5 stages/tests, F6 activates, F7 saves, F8 undoes one source edit, and F10 closes.
Home/End move within a source line; PageUp/PageDown currently map to Home/End.
The editor follows the cursor vertically and horizontally. It preserves existing
ASCII tabs and CR/LF exactly; keyboard Enter inserts LF. A patch can insert tabs.

## Live editing through MCP

The harness exposes 19 `wb_*` tools. For example:

```python
from tools.vios_cli import RPCClient
with RPCClient() as client:
    client.initialize()
    client.tool('connect')
    client.tool('wb_open')
    state = client.tool('wb_new', {'module': 'DEMO', 'version': '1'})['workbench']
    state = client.tool('wb_patch', {'revision': state['revision'],
        'offset': 0, 'remove': 0, 'source': ': DEMO-VALUE 42 ;\n'})['workbench']
    client.tool('wb_stage', {'revision': state['revision']})
    client.tool('wb_activate', {'revision': state['revision']})
    print(client.tool('evaluate', {'source': 'DEMO-VALUE . CR'})['output'])
```

`wb_browse`, `wb_select`, `wb_edit`, `wb_source` and `wb_view` inspect the live
system. `wb_metadata` sets version/test word while preserving module dependencies.
`wb_patch` applies one atomic replacement of at most 80 ASCII bytes; offsets,
delete length and expected revision are checked before mutation. Larger source
uploads use successive patches and each acknowledged new revision. A lost reply
is never automatically retried. `wb_undo` requires the current revision too.
Replacing a dirty draft requires explicit `discard=true`.

Staging compiles and tests against an isolated transaction. Activation requires
that exact staged source, metadata, dependencies and unchanged live catalog.
An unrelated published definition requires reloading the draft before staging.
Failed updates retain the draft and application state. `wb_save` saves only the
exact active draft to its immutable source package; it does not change boot slots.
Use existing explicit boot tools when a saved module should enter an A/B trial.

`wb_view` returns desired rows and packed attributes, with revision/state checks.
This is a headless model view. `CHAR@`/`PIXEL@` query the actual VDP framebuffer.
Use `wb_flush` and a VDP reply as an ordered completion fence before readback.
After trusted raw `VDU` or `MODE` operations, use `wb_redraw` to restore mode,
palette and renderer cache.

## Malleability, resource and recovery boundaries

Dense buffers, editing and dirty-span rendering are portable standard C; the
Agon adapter supplies VDU transport, timer and physical key events. The native
VM opcode engines remain unchanged. The `WB-PAINT` deferred Forth word initially
binds to `WB-PAINT-DEFAULT`, which emits at most 128 wire bytes per idle batch.
Composition happens at command/key boundaries with the real application stack.
The paint observer runs on a temporary stack with a 4,096-instruction budget and
restores application DS, BASE, engine, errors and execution bookkeeping.
`KEY` is rejected inside observers. On a paint error the draft remains and painting
stops. Restore the default and invalidate the display from the ordinary CLI:

```forth
' WB-PAINT-DEFAULT IS WB-PAINT WB-REDRAW
```

Normal CLI input has priority; painting does not run during partial lines,
compilation or application execution. Physical key events have a separate
bounded 32-event queue. Hardware flow control pauses incoming bytes during C
composition/painting and reopens reception after the slice; UART send waits keep
draining incoming packets. Empty frames emit no glyph bytes and pending checks
are constant time. Raw native callbacks,
arbitrary memory writes and a stalled VDP remain trusted systems operations;
the independent ESP32 supervisor provides debug and forced restart recovery.
A forced restart loses unsaved RAM drafts.

There is one 4,096-byte module draft and one source undo snapshot; no heap is used.
The two packed character/attribute planes use 19,200 bytes. The bottom-right cell
is reserved blank to prevent the VDP's automatic full-screen scroll.
The Agon dictionary now has 512 configurable entries and 128 hash buckets.
Names, source, data, token code and direct code have independent finite budgets.
See [the implementation ledger](vios-workbench-progress.md) for memory accounting.

Composition now retains the desired frame. Browser selection changes update the
old/new rows; catalog/filter/scroll changes rebuild that pane. A bounded 16-bit
source-line index and cursor-column cache avoid rescanning preceding source.
Source damage rechecks affected rows and both cursor rows, then emits only
changed character/style runs. Inspector scalars are sampled at each composition
boundary, including live variables, deferred targets, DS, BASE and resources;
unchanged rows skip formatting. Reset/full-frame clear advances a renderer
generation so retained caches cannot reuse an invalid display.

These caches add 13,203 BSS bytes. On October 16 the total was 254,031 BSS bytes,
with 94,919 bytes remaining before the reserved 4 KiB stack; see
`build/vios-resident.map` for the current build. Source viewport changes
still compose its visible rows; opening/explicit redraw uses a full fallback.
Raw VDU or trusted direct screen writes require `WB-REDRAW`.

Source-row projection uses indexed line endpoints and bulk copying when the
visible prefix contains no tabs. Tab expansion keeps the general path. Sparse
glyph differences use pointer walks; cursor style repair happens outside that
loop, before any painting can run. The source-row optimization adds 474 payload
bytes and no BSS. See [source scrolling/newline measurements](../benchmarks/source-rows-2026-10-16.md).

`WB-COMPOSE` reports `elapsed_ticks`, `timer_bits 16`, and `timer_hz 288000`
without changing the inspected stack. It measures only composition, excluding
output, painting and dispatch. The result is modulo 65,536: it is valid only for
intervals below 227.56 ms. The benchmark driver independently checks full-frame
duration with the 32-bit VDP clock and skips unsafe 16-bit full-frame samples.
See [incremental measurements](../benchmarks/incremental-2026-10-16.md).

## Reproduce hardware rendering measurements

With a freshly deployed/booted image and the shared harness running:

```sh
.venv/bin/python tools/vios_workbench_hardware.py \
  --output docs/bring-up/vios-workbench-hardware-new.json
```

The driver refuses an existing dirty workbench or reused WBM1P module namespace.
It makes a RAM-only fixture, measures full/empty/edit/selection/scroll frames,
tests stale patches and failed staging, repairs a failed paint binding, compares
eight framebuffer characters with desired cells, and restores application stack
and BASE. It does not save to SD or change boot slots. Hardware observations
establish protocol/readback behaviour; no attached display or physical keyboard
was required for the automated checks. The user subsequently connected VGA and
a keyboard and confirmed that the text inspector is visible and the keyboard
appears to work. Actual queued F6/arrows/Enter events and corresponding model
navigation were also observed during the shared session.

[The captured model snapshot](vios-workbench-screen.png) reconstructs actual
WB-VIEW cells using the unmodified Agon font from the pinned VDP source. It is
not a camera photograph. Font source attribution and notices remain in
`firmware/esp32-vios/vendor/agon-vdp/video/agon_fonts.h`.

## Application-only rollback

The preceding workbench application was read back before the source-row update:
`backups/esp32/pre-vios-source-rows-app0-2026-10-16.bin`, 663,472 bytes, SHA256
`b8d884a1733505de995ee94f5585abaa6ed9ee9986468529cb32dc9fc3fefb2f`.
Raw `esptool write-flash 0x10000` is no longer the procedure. With the
serial-owning harness stopped, install it like any other build with the A/B
installer, which backs up the slot it replaces (see the manual's
[Installing](../manual/VIOS-MANUAL.md#installing) chapter):

```sh
.venv/bin/python tools/vios_firmware_ab.py --port /dev/cu.usbserial-210 \
  install backups/esp32/pre-vios-source-rows-app0-2026-10-16.bin --backup-dir backups/ab-restore
```

This old image predates untethered boot (it needs the ZDI wire) and the trial
marker, so it does not confirm itself; return with `vios_firmware_ab.py ... rollback`.
It restores the previous CPU image embedded in that firmware. It does not erase source
packages or A/B source boot files on SD. The eZ80 flash is never programmed.

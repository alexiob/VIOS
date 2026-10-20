# VIOS GUI tools: browser, workspace, inspector, manager (design)

Status: **implemented and host-tested, 2026-10-20** (branch `claude/vios-gui-tools-91fb31`); board acceptance in §8. Amends the
[GUI windows design](2026-10-19-vios-gui-windows.md) (G4 widgets: TextView,
tree, table, tabs) and adds the first set of development tools on top of it.
Numbers marked *measured* come from builds or the board; *estimate* means not
yet measured. Implementation status is tracked at the end of this document.

## 1. Intent

VIOS should feel like a Smalltalk system: you develop, inspect and manage the
running system *from inside it*, with tools that are themselves ordinary parts
of it. The owner's constraints for the tools (2026-10-20):

- **Fully keyboard controllable**, like the rest of the VIOS GUI (no mouse yet).
- **Memory as low as possible.** eZ80 RAM is the scarce side (*measured*
  `__bss_end` 0xA3D63, stack bottom 0xAF000: **45.7 KB gap**; spec gate ≥ 12 KB).
  ESP internal RAM is the second scarce resource (24 KB floor for fabgl); PSRAM
  (~4 MB free) is plentiful.
- **Widgets fast, reliable, small**: reuse the existing ones first; add only
  what the tools need; run them on the ESP display server.

Goals: four tools (§3), the widgets they need (§4), protocol additions (§5),
host tests for all of it, an on-board acceptance tool.

Non-goals (this round): mouse, Canvas/display lists, drag and drop, syntax
colouring, multiple instances of the same tool, an ESP-side Forth parser.

Anti-goals: a tool that makes the REPL/harness unreliable (unsolicited output
on USB, stolen events, blocked idle loop); CPU code growth that eats the
headroom user modules need; widget state that can silently diverge from the CPU.

## 2. Architecture

```text
 eZ80 (meaning)                                   ESP32 (presentation, frame time)
 kernel/vios_tools.c  tool engine (~C)            vds_widget.c  list/table/tree rows, tabs,
   Browser, Workspace, Inspector, Manager                       TextView (local editing),
   feeders: dictionary, modules, memory,                        key policy, nudge
   captured command reports (TASKS, ...)          all widget state in vds_ui (PSRAM)
 kernel/vios_ui.c  op encoder, event poll  <── E4 ops ──> vds_proto.c
                     claims tool events    ──> nudge (0xF0) / replies (0xF1)
```

The rule from the windows design stays: anything that must react within a frame
runs on the ESP (navigation, scrolling, editing, selection, tab switching,
type-ahead); anything that decides meaning runs on the CPU (what a selection
shows, compiling, evaluating, killing a task, changing boot slots).

### 2.1 Where tool logic lives (decision: C engine now, Forth extension point)

| Option | eZ80 cost | Malleable | Risk |
|---|---|---|---|
| A. Tools in C (`kernel/vios_tools.c`) | code in the resident: *target* ≤ 9 KB code + ≤ 0.6 KB BSS | Tools fixed; everything they show/edit is live Forth | Low; host-testable end to end |
| B. Tools as a Forth module | none new: uses the pre-allocated pools (512 words, 16 KB token, 8 KB source) | Fully: the browser edits the browser | Competes with user modules for the same pools (8 modules × 4 KB); needs ~25 more natives anyway for the feeders; slower; a broken tool module breaks the tools |

**Choice for this round: A**, because the feeders (dictionary walk, module
metadata, memory pages, output capture, accept pipeline) must be C either way,
and the composition code on top of them is small. The tools are fully
extensible from Forth without editing C (§3.6). **Open decision for the owner:**
move tool composition into a Forth module later (B) once the pools are sized
for it.

### 2.2 Events: claimed, nudged, never stolen

- Widget and WM events are queued on the ESP (16 deep) and pulled with op 36.
- **Nudge:** after `CONFIG 3 1` the ESP sends one 1-byte packet 0x70 (seen as
  0xF0) when an event is queued while none is outstanding; the next poll re-arms
  it. The CPU transport already stores any ≥ 0x80 packet in a reply slot, so
  checking for the nudge costs **no UART bytes**. Idle traffic with no events:
  0 B/s (*target*; previous alternative: a poll every 33 ms ≈ 300 B/s).
  Fallback: one poll per second while a tool is open (lost nudge, old firmware).
- **Claiming:** the tool engine pulls events in the REPL idle hook and claims the
  ones that are its own (tool window ids, tool command ids); every other event
  goes to the existing CPU ring, so applications using `UI-NEXT` keep working.
- **Key policy (op 5A):** keys a widget or intrinsic does not use normally go to
  the CPU (0x81) and would end up in the REPL line. Tool windows set policy 1:
  unconsumed keys are dropped on the ESP.

### 2.3 Evaluation and output capture

- Tool actions run from the idle hook, which the REPL calls only with an empty
  input line and nothing compiling (`forth/system.c` `forth_repl`). `forth_eval`
  is safe there; it is exactly what typing the line would do.
- Reports reuse the existing textual commands (`TASKS`, `HANDLES`, `RESOURCES`,
  `BOOT-STATUS`, `BOOT-MANIFEST`, `SEE`, `.S`): a new platform call
  `platform_console_divert(sink)` sends console output **only** to a sink while
  set, so nothing reaches USB (no unsolicited harness output) or the Console
  window. The tool engine's sink cuts lines into list items as they arrive (one
  64-byte line buffer). This replaces a dozen special-purpose feeders with one.
- Workspace *do it* output is **not** diverted: it is shown in the Console
  window, which is the Transcript (§3.2). *Print it* diverts and inserts.

## 3. Tools

Window, surface and widget ids are reserved for the tools (§6). Each tool is a
single window; opening it again focuses it. All four are in the world menu
(Smalltalk: F10; Turbo Vision: the menu bar) and have global accelerators.

### 3.1 System Browser (window 20) — Ctrl+Shift+B

```text
┌Browser─────────────────────────────────────────────────────────────┐
│ (all)          │ SQUARE          │ kind colon  xt 312               │
│ (core)         │ CUBE            │ module DEMO 1  crc 4112551253    │
│ DEMO         * │ AREA            │ token 9  direct 21               │
│ WBM1P        * │                 │ deps: CORE ok                    │
├────────────────┴─────────────────┴──────────────────────────────────┤
│ MODULE DEMO 1                                                       │
│ : SQUARE ( n -- n*n ) DUP * ;                                       │
│ : CUBE DUP SQUARE * ;                                               │
├─────────────────────────────────────────────────────────────────────┤
│ DEMO 1: active, saved                         Ctrl+S accept  F10 menu│
└─────────────────────────────────────────────────────────────────────┘
```

- **Modules pane** (list): `(all)`, `(core)` (words without a module), then
  each module with `*` when active. Select → words pane and code pane follow.
- **Words pane** (list, type-ahead): the selected module's words.
- **Info pane** (table): word facts (kind, xt, token/direct bytes, module) or,
  for a module, version, CRC, sizes, **dependencies** (`ok`/`stale` against the
  live CRC) and **used by** (reverse dependencies). This is the dependency view.
- **Code pane** (TextView): the module's exact source (`forth_module_source`) or
  the word's retained source. Editable.
- **Accept** (Ctrl+S): module → the text becomes the workbench draft
  (`vios_workbench_edit` + `vios_workbench_patch`, streamed from the ESP in
  chunks, no new CPU buffer) and is staged and activated with the existing
  revision-checked calls; errors land in the status line. A word outside a
  module → the text is evaluated (redefinition, like a Smalltalk method accept).
- Pane menu (Shift+F10): Accept, Save to SD (`vios_module_save`), Revert
  (reload), Rollback, Inspect word, Browse dependency.

### 3.2 Workspace (window 21) — Ctrl+Shift+W

A TextView for snippets. Smalltalk keys, all ESP-local except the evaluation:

| Key | Action |
|---|---|
| Ctrl+D | *do it*: evaluate the selection (no selection: the caret's line, which is then selected); output in the Console/Transcript |
| Ctrl+P | *print it*: evaluate, insert the printed output (or the new stack values) after the selection, selected |
| Ctrl+I | *inspect it*: evaluate, open the Inspector on the stack |

Errors are inserted after the selection (selected, so Backspace removes them),
the Smalltalk convention.

### 3.3 Inspector (window 22) — Ctrl+Shift+I

Tabs widget over one table and one read-only detail TextView; the CPU refills
the table when the tab changes (one list on the ESP, not six).

| Tab | Rows | Enter / commands |
|---|---|---|
| Stack | depth, hex, decimal of each cell | detail: `SEE` when the value is an xt |
| Memory | 16 bytes per row, hex + ASCII, 16 rows; PgUp/PgDn and Up/Down past the edge page (§4.1 pager) | Go to address (from the stack) |
| Tasks | `TASKS` report | Kill task (`TASK-KILL`, confirmation) |
| Handles | the handles report | — |
| System | `RESOURCES`, display server status (`UI-SYNC`, op 33 stats), ESP/VDP info | Refresh (Ctrl+R) |

### 3.4 Manager (window 23) — Ctrl+Shift+M

Tabs: **Boot** (boot status and manifest; the table lists Trial, Confirm,
Revert — Enter runs one after a modal Yes/No dialog, window 24) and **Modules**
(Enter saves to SD; the pane menu rolls back the latest activation). The
display server status is the Inspector's System tab (one copy, not two).

### 3.5 Keyboard map (complete)

Global (world menu items too): Ctrl+Shift+B Browser, Ctrl+Shift+W Workspace,
Ctrl+Shift+I Inspector, Ctrl+Shift+M Manager. Window keys are the existing G2/G3
ones (F6 next window, Ctrl+F5 move/resize, F5 zoom, Ctrl+W close, Shift+F10
pane menu, Ctrl+K palette).

Inside a tool: Tab / Shift+Tab next/previous pane; Ctrl+Tab / Ctrl+Shift+Tab
next/previous tab; Ctrl+R refresh; Esc leaves a dialog.

| Widget | Keys |
|---|---|
| List / table | ↑ ↓ PgUp PgDn Home End, letters type-ahead, Enter activate; tree rows: → expand, ← collapse; pager lists: moving past either end requests the next page |
| Tabs | ← → change tab |
| Dialog | Tab between Yes/No (No has focus, so Enter alone cancels); Esc cancels |
| TextView | arrows, Home/End (line), Ctrl+Home/End (text), PgUp/PgDn; Shift+movement selects; Ctrl+A all; Ctrl+C/X/V clipboard (ESP-local, 1 KB); Enter newline; Backspace/Delete; Ctrl+[ indent / Ctrl+] de-indent (two spaces on every line the selection touches); Ctrl+S accept; Ctrl+D/P/I evaluate; read-only views move and select only |

Tab moves focus out of a TextView (uniform navigation); there is no tab
character. Indentation: Ctrl+[ and Ctrl+] (owner decision, 2026-10-20).

### 3.6 Extension from Forth

- Tool commands use ids 0x7F00–0x7FFF; any other command id the engine receives
  for a tool window, and every world-menu command an application defines, stay
  in the CPU ring for `UI-NEXT` as before.
- A deferred word `TOOL-ACTION ( cmd window -- )` (default: no-op) is called
  through `forth_observe` (bounded, 4096 instructions) for tool-window commands
  the engine does not know, so Forth code can add pane-menu entries with
  `UI-COMMAND`/`UI-MENU` and handle them. *(stage T5, optional)*

## 4. Widgets

Existing (vds_widget.c): box (column/row by fixed or weight), label, button,
check, radio, input, list, progress. Additions, all ESP-side, all state in
`vds_ui` (PSRAM):

### 4.1 List upgrades (list, table, tree, pager)

The list pool is 64 items × 24 B shared by all lists with u8 counts: a
512-word dictionary cannot load. Decision: **a bigger PSRAM pool, u16
indices**, not virtual paging, because the tools' lists are bounded (≤ 512
words, a few dozen report lines) and local type-ahead and scrolling need every
row; memory is the one exception (pager below).

- Pool: `VDS_LIST_POOL` 64 → **1024 items × 32 B** = 32 KB PSRAM (*estimate*).
  `count`, `top`, `first`, selection: u16. Existing ops 85/86 keep their u8
  forms; new ops take u16 (§5).
- **Append op**: several items in one op (`len text` pairs) — a 500-word list
  is ~4.5 KB on the wire, ~40 ms at 1.152 Mbaud (*estimate*).
- **Table**: column widths (op 93); cells separated by `\t` in item text and in
  the widget text, which becomes a header row.
- **Tree**: flattened on the CPU (indent and `+`/`-` in the item text). With
  flag TREE, → and ← send EXPAND events instead of nothing; the CPU re-sends rows.
- **Pager**: with flag PAGER, moving past the first/last row or PgUp/PgDn at an
  edge sends a PAGE event (direction) instead of clamping; the CPU replaces the
  rows (memory view).

### 4.2 TextView (new kind 9)

- Retained text buffer on the ESP, **local editing** (same authority model as
  the existing Input widget: the ESP holds the draft, the CPU reads it back on
  accept and owns what is compiled). Buffer: `VDS_TEXT_MAX` 4096 B (the module
  source limit), **4 slots** in a pool (`VDS_TEXT_SLOTS`) = 16 KB PSRAM.
- State per view: length, caret, anchor (selection), top line, left column,
  dirty, revision (u16, +1 per edit).
- Rendering scans from the cached top-line offset: O(visible text), no line
  table. Selection drawn in the selection colours; caret is the surface caret.
- Events (kind 05 TEXT): accept, do-it, print-it, inspect-it with the selection
  range; one CHANGE when the view first becomes dirty (not per keystroke).
- Read-back: op 8C returns 25 bytes per reply (the CPU's 32-byte reply slots).
  4 KB = 164 round trips (*estimate* 0.2–0.5 s; measured on the board, §8).
  *Open decision:* a bulk reply path if this is too slow.
- Clipboard: one shared 1 KB buffer in `vds_ui`.

### 4.3 Tabs (new kind 10)

One row: the widget text split at `|`, the selected tab inverted. ← → when
focused, Ctrl+Tab / Ctrl+Shift+Tab from anywhere in its window; SELECT event.
Pages are not managed by the ESP: tools refill or show/hide panes on the
event (the existing HIDDEN flag), which keeps the widget at ~60 lines of code.

### 4.4 Not built (and why)

- **Splitter**: panes are sized by weights; resizing is a later command that
  changes `UI-WSIZE` weights — no new widget.
- **Separate tree widget**: CPU-flattened list (§4.1) covers it.
- **Virtual-paging list**: only the memory view needs it; the pager flag does.

### 4.5 Memory budget per widget (*estimates*, replaced by measurements in §8)

| Item | ESP PSRAM | ESP internal | ESP code (-Os) | eZ80 |
|---|---:|---:|---:|---:|
| Widget table 48 → 64 entries | +0.8 KB | 0 | 0 | 0 |
| List pool 64×24 → 1024×32 | +31 KB | 0 | ~0.6 KB | 0 |
| Table columns (8 × u8 per list) | in widget | 0 | ~0.3 KB | 0 |
| TextView (4 × 4 KB + state) + clipboard 1 KB | 17 KB | 0 | ~3 KB | 0 |
| Tabs | in widget | 0 | ~0.4 KB | 0 |
| Key policy, nudge | 33 B | 0 | ~0.2 KB | 0 |
| UI words for the new ops (`kernel/vios_ui.c`) | — | — | — | ~1.2 KB code, ≤ 8 natives |
| Tool engine (`kernel/vios_tools.c`) | — | — | — | ≤ 9 KB code, ≤ 0.6 KB BSS |

ESP internal RAM: unchanged (`vds_ui` is allocated in PSRAM by `vios_gui.h`).

## 5. Protocol additions (E4 ops, little endian)

| Op | Bytes | Meaning |
|---|---|---|
| 5A | `id mode` | key policy of window id: 0 unconsumed keys to the CPU (default), 1 drop |
| 74 | `3 on(u16)` | config 3: nudge (one 0x70 packet when events wait) |
| 8A | `id mode n ch*n` | TextView text: mode 0 replace all, 1 append |
| 8B | `id caret(u16) anchor(u16)` | TextView caret/selection (scrolls to the caret) |
| 8C | `id offset(u16) n` | reply `8C id offset(u16) n ch*n`, n ≤ 25 |
| 8D | `id n ch*n` | TextView insert at the caret, replacing the selection; the inserted text becomes the selection |
| 8E | `id` | reply `8E id len(u16) caret(u16) anchor(u16) rev(u16) dirty` |
| 90 | `id index(u16) n ch*n` | list item (u16 index) |
| 91 | `id count(u16)` | list length (u16) |
| 92 | `id k (n ch*n)*k` | append k items to a list |
| 93 | `id n w*n` | table column widths (n ≤ 8) |

Widget kinds: 9 TextView, 10 Tabs. Widget flags: 8 READONLY (TextView),
16 TREE, 32 PAGER (lists). Events: `05 TEXT id what sel_start(u16) sel_end(u16)
window` (what: 1 accept, 2 do it, 3 print it, 4 inspect it); `08 EXPAND id
index(u16) window open`; `09 PAGE id dir(s8) window`. Existing: ACTIVATE,
SELECT (lists: index u16), CHANGE (TextView: dirty).

## 6. Ids reserved for the tools

| Resource | Total | Applications | Tools |
|---|---|---|---|
| Windows (CPU) | 0–26 | 0–19 | 20 Browser, 21 Workspace, 22 Inspector, 23 Manager, 24 dialog, 25 spare; 26 stays the workbench |
| Surfaces (CPU) | 0–10 | 0–4 | 5 dialog, 6–9 one per tool; 10 stays the workbench |
| Widgets | 0–63 | 0–39 | 40–63 |
| TextView slots | 4 | 1 | Browser code, Workspace, Inspector detail, Manager detail (all four when every tool is open) |
| Commands | u16 | below 0x7F00 | 0x7F00–0x7FFF |

*Open decision:* raise `VDS_PROTO_SURFACES` so applications keep more than 6
surfaces (it renumbers the reserved menu surfaces).

## 7. Stages

| Stage | Work | Tests |
|---|---|---|
| T1 | ESP: widget table 64, list pool + u16 ops, append, table columns, tree/pager flags, tabs, TextView, key policy, nudge | `firmware/esp32-vios/tests/vds_widget.c` + new `vds_text.c` (random edits vs a model, full-render oracle) |
| T2 | CPU: UI words for the new ops; event claim API; `platform_console_divert` | `tests/vios_ui_host.c` end to end |
| T3 | CPU: tool engine — Browser, Workspace | new `tests/vios_tools_host.c`: Forth + engine + real ESP library |
| T4 | Inspector, Manager, dialogs | same |
| T5 | On-board acceptance `tools/vios_gui_tools_accept.py` (keys injected through the WM with `esp_gui GK`, screens/pixels/status read back), memory measurements | board |

## 8. Measurements and status

**Implemented** (host-tested; board results below when run):

| Stage | Where | State |
|---|---|---|
| T1 widgets | `vds_text.c` (TextView), `vds_widget.c` (tables, tree, pager, tabs, 1024-row pool, append), `vds_ui.c` (key policy, Esc closes a modal window), `vds_proto.c` (ops 5A, 8A–8E, 90–93, nudge) | done; `tests/vds_text.c` (8000 random steps + full-render oracle) |
| T2 CPU words | `kernel/vios_ui.c`: UI-ROW UI-COLUMNS UI-KEYS UI-TEXT UI-TEXT+ UI-INSERT UI-SELECT UI-TEXT@, u16 UI-ITEM/UI-ITEMS, ids 0..63; C API for clients; claim hook | done; `tests/vios_ui_host.c` |
| T3/T4 tools | `kernel/vios_tools.c`, `include/vios/tools.h`, `TOOL ( n -- )`; `platform_console_divert`, `platform_vdp_nudged` | done; `tests/vios_tools_host.c` (keys on the real ESP library, CPU acting from idle) |
| T5 board | `tools/vios_gui_tools_accept.py` | written; see the board run below |

**Memory, measured (builds of 2026-10-20):**

| Side | Item | Size |
|---|---|---|
| eZ80 | gap below the stack (`0xAF000 − __bss_end`) | 45,725 B before → **30,315 B** after (gate 12 KB) |
| eZ80 | `vios_tools.o` | 9.2 KB code, 2.1 KB rodata (1.3 KB of it strings), 0.9 KB BSS |
| eZ80 | new UI words in `vios_ui.o` | +3.1 KB code |
| eZ80 | `NATIVES_MAX` 96 → 112 | +96 B |
| ESP | code (xtensa `-Os`) | `vds_text` 3.1 KB, `vds_widget` 8.6 → 10.5 KB, `vds_ui` 11.8 → 12.5 KB, `vds_proto` 4.1 → 4.3 KB: **+6.3 KB flash** |
| ESP | `vds_ui` state (PSRAM, `vios_gui.h`) | 10.8 KB → 62 KB: rows 32 KB, TextViews 16 KB, clipboard 1 KB, 64 widgets 4.5 KB |
| ESP | internal RAM | unchanged (nothing new is allocated there) |

The eZ80 cost (about 15.4 KB with the UI words) is above the 9.6 KB target:
eZ80 clang output is large for this kind of code (24-bit arithmetic, many small
calls). Further reductions, in order of value: move tool layouts/strings to an
ESP template op (presentation only), shorten strings (~0.4 KB), drop the
dependency/used-by scan (~0.3 KB).

**Traffic (host test, the whole tool session):** 48 nudges, 64 requests,
10.2 KB CPU→ESP. Idle with no events: no polls (no UART bytes); with a tool
open, one poll per second as the fallback.

**Found while building:** the module system keeps two generations per module
per boot ("two module versions retained; rollback or restart before a third").
So the Browser can accept a module once per boot; a second accept is refused
with that message. Re-accepting a *word* (redefinition) is unlimited. Lifting
the limit is a module-system change (owner decision, §10).

## 9. Builder calls (settled without the owner, from the design review)

- **eZ80 code lives in RAM** (the resident is loaded to 0x40000), so the tool
  engine's code counts against the 45.7 KB gap. If it runs over budget, cut in
  this order: Firmware tab, Handles tab, memory pager, Manager module actions.
- **Runaway snippets**: Workspace evaluation is what typing the line at the
  REPL does, so the existing foreground execution budget applies ("execution
  budget exhausted"); the error is inserted in the workspace. No new watchdog.
- **Feeders exist**: `forth_inspect_word` gives each word's retained source,
  module, kind and sizes; `forth_module_info`/`_dependency_at`/`_source` give
  modules, their dependencies with CRCs (live CRC = the active module's
  `source_crc32`); "used by" is a scan over all modules' dependencies.
- **Defaults of §10 are shipped** as stated; none blocks T1. Read-back is built
  chunked and measured; a bulk path would be an added op, not a change.
- **Compatibility**: CPU and ESP builds are always paired (the `vios-resident.gui`
  sidecar rule). The nudge is switched on by the CPU (`CONFIG 3 1`); the 1 s
  fallback poll is kept and tested so a lost nudge only delays.
- **No text-input dialog**: Manager "load" picks from a list of `/VIOS` files
  (`DIR` capture); Inspector "go to address" takes the top of the data stack
  (set in the Workspace, Smalltalk-style). Firmware tab: display server epoch,
  errors, frame stats (op 31/33) and `BOOT-STATUS`; nothing new in the resident.
- **Console/Transcript** is the server-owned window 32 and always exists; do-it
  output shows there even when it is behind other windows. Print-it with no
  printed output inserts the values the evaluation left, in the current BASE,
  separated by spaces, and leaves them on the stack.
- **Widget ids**: Browser 7, Workspace 2, Inspector 4, Manager 4, dialog 4 =
  21 of 24. The Manager has no TextView.

## 10. Owner decisions (2026-10-20) and what is still open

1. Tools in C now (chosen; kept by the owner) vs a Forth tool module later.
2. TextView read-back: chunked 25-byte replies (chosen); the board measures it (§8).
3. Surfaces: applications keep 0–4 (chosen); raising the count is a later protocol change.
4. **Indentation: Ctrl+[ indent, Ctrl+] de-indent** (owner).
5. ESP-local editing in TextView (chosen, like Input).

Still open: (a) more than two generations per module per boot, so the Browser
can accept a module repeatedly; (b) whether the eZ80 cost (§8) is acceptable
or the tool layouts should move into an ESP template op.

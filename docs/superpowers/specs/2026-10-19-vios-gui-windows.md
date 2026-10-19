# VIOS GUI: windows, widgets and a pixel compositor (amendment)

Status: **design proposal, 2026-10-19.** Research and design only. Nothing in this
document has been built or run on the board. Numbers marked *measured* come from
board evidence in this repo; numbers marked *estimate* or *target* are unverified
until the gate in §14 (G0) measures them.

This amends the [GUI toolkit design](2026-10-19-vios-gui-toolkit.md) to meet new
owner requirements. It **supersedes** these parts of that document:

- §6, the compositor: cell-positioned tiles become pixel-positioned windows.
- §8, layout: "overlap later" becomes overlap from the first compositor stage.
- §9, pointer policy: the mouse plan and its hardware cost are restated here.
- The window-manager, menu, animation and pointer rows of the §4 authority table.
- §13, Stages 1b, 5 and 6: replaced by the G0–G6 plan in §14.

Everything else stays in force: the `E4`/`F0`/`F1` wire framing (§5.1), the
existing op and event codes, the malleability and inspection model (§10),
`/dev/win`, the measurement discipline (§11), and the Stage 0/1a/1c results.

---

## 1. Requirements (owner, 2026-10-19)

| # | Requirement |
|---|---|
| R1 | Overlapping windows: move, resize, raise, close, minimise and maximise. |
| R2 | Widgets: pulldown menus, dialogs, buttons, inputs, lists, trees, scrollbars, tabs and so on. |
| R3 | Every action is reachable from the keyboard, with full navigation. |
| R4 | A mouse later, once the development/debug wire is removed. |
| R5 | Fast and responsive: real-time animation, smooth window moves, dirty-rectangle updates. |
| R6 | Text UI (Turbo Pascal/Turbo Vision style with modern TUI features) and/or full graphics. **If the text console's framebuffer is the same as graphics mode, prefer graphics.** |
| R7 | Core functionality in C. |

Inherited constraints from the handoff and the existing design still hold. The
CPU (eZ80) stays the authority for documents and behaviour. No new USB traffic
classes are allowed (open issue #2). ZDI/USB recovery must stay independent. And
the existing `WB-PAINT` observer rules still apply.

---

## 2. The deciding fact: text and graphics share one framebuffer

In every VDP mode, text is drawn as pixels into the same framebuffer that graphics
use. The ESP console added in Stage 1c already does exactly this:
`firmware/esp32-vios/include/vios_console_vdp.h` calls `canvas->drawChar(x*fontW, y*fontH, ch)`.
There is no separate character generator or text plane in the VGA path. A pure
cell-grid TUI therefore saves no memory and no bandwidth. It only gives up
pixel-level positioning, shadows, icons and smooth motion.

**Decision (per R6): build a graphical compositor.** Text stays first-class inside it.

The real choice is how window *content* is stored:

| Option | Storage per full-screen window | Move/expose cost | Fits |
|---|---|---|---|
| A. Pixel backing store per window | 150 KB (640×480×4 bpp) in PSRAM | Copy pixels | Canvas/graphics windows only |
| B. **Cell surface** (2 B per 8×8 cell) re-rasterised on expose | 9.6 KB in PSRAM | Re-blit glyphs (cheap, §5.4) | Text, widgets, decorations |
| C. Display list (vector ops) replayed on expose | ≤2 KB per canvas | Replay clipped | Diagrams, simple graphics |

**Recommendation: hybrid.** Text and widget content lives in cell surfaces (B), so
the Turbo Vision look comes for free. Graphics canvases use display lists (C), with
an optional pixel backing (A) for bitmap-heavy windows. Window *frames* are
positioned in pixels, so windows overlap, cast shadows and move smoothly, while
their text stays crisp on the 8×8 font grid.

**Default theme:** Turbo Vision's visual language, with frames, title bars, drop
shadows, a menu bar and a status line, rendered with real pixels: 1-pixel frames,
a soft shadow and a focus ring. It also takes features from current open-source TUIs,
such as Turbo Vision's magiblot port, Textual, notcurses and ratatui: a command
palette, constraint layouts, themes and an always-visible focus indicator.

---

## 3. Display mode (owner decision, gated by measurement)

Candidate modes in the pinned VDP 1.04 (`vendor/agon-vdp/video/graphics.h`):

| VDP mode | Resolution | Colours | Buffers | Framebuffer (internal RAM) |
|---|---|---|---|---|
| 1 (today) | 640×480 | 4 | single | 75 KB |
| **0** | 640×480 | **16** | single | 150 KB |
| 129 | 640×480 | 4 | double | 150 KB |
| 3 | 640×240 | 64 | single | 150 KB |
| 8 | 320×240 | 64 | single | 75 KB |

Mode 0 and mode 129 cost the same internal RAM. There is **no double-buffered
640×480 mode with 16 colours.**

**Recommendation: mode 0 (640×480, 16 colours).** A windowed UI needs at least six
visual states:

- desktop
- window body
- active title
- inactive title
- selection/focus
- shadow, disabled text and text colours on top of those

Four colours cannot show these distinctly. Tearing in a single-buffered mode is
handled by beam-ordered composition (§5.5). **Fallback:** mode 129 (4 colours,
double buffered) if G0 shows tearing that cannot be fixed. **Last resort:** mode 1
with a 4-colour theme.

**Palette compatibility (mode 0):** on entering mode 0, VIOS pins palette
entries 0–3 to today's mode 1 colours (black, red, yellow, white). Every existing
`attr` value then keeps its meaning:

- `text_screen.c`'s default `attr=3`;
- the workbench styles;
- custom `WB-PAINT` painters;
- the Stage 1c console renderer.

Theme roles use indices 4–15. Tools and tests that assert `colours 4 mode 1` must
be updated in G1: `tools/vios_resident_acceptance.py` ("Pinned nonlegacy mode 1"),
the `VDP-INFO` fixture in `tests/test_vios_resident_acceptance.py`, and the
fast-display and console acceptance tools.

**Gate G0:** nobody has measured the ESP's free internal heap after a mode change.
The pio report gives only the static use, *measured* at 44,332 B of 327,680 B. G0
reads the free heap and the largest free block (`heap_caps_*`, already planned for
`esp_stats`) after setting mode 0, mode 129 and mode 1, with the compositor's pools
allocated. **Mode 0 is accepted only if ≥24 KB of internal RAM stays free** (the
existing doc's floor).

---

## 4. Architecture and authority

```text
 eZ80 (authority, behaviour, C + Forth)              ESP32 (presentation, intrinsics, C/C++)
 ┌───────────────────────────────────────┐   UART2   ┌───────────────────────────────────────────┐
 │ apps, handlers (Forth, observer-bound) │ 1.152 Mb  │ VDS: window server                        │
 │ vios_ui.c: widget/window/command table │ ────────► │  windows, z-order, clip lists             │
 │   op encoder/batcher (E4), F0 dispatch │ ◄──────── │  surfaces (cells, PSRAM), canvases        │
 │ workbench model, console REPL          │  events   │  compositor: dirty rects → blitter         │
 │ /dev/win/*, UI vocabulary              │           │  WM intrinsics, menus, focus, key router  │
 └───────────────────────────────────────┘           │  animation tick (60 Hz), pointer overlay  │
                                                     │  fabgl VGA16 framebuffer (internal RAM)   │
                                                     └───────────────────────────────────────────┘
```

The rule: **anything that must react within a frame runs on the ESP. Anything that
decides meaning runs on the CPU.** A real-time window move cannot survive one CPU
round trip per step: the CPU waits on UART RTS windows and 288 kHz slices, and
typing under load measured 26/33 ms (median/p95) in Stage 1c.

### Authority table: new and changed rows

| State | Authoritative | Copy | Notes |
|---|---|---|---|
| Window identity, kind, owner app, constructor source | CPU | ESP node | Same as widgets (§10 of the existing design). |
| Window geometry, z-order, min/max state | **ESP** while the user moves or resizes; CPU on `WGEOM` | CPU, via `WIN` events | The ESP applies keyboard/mouse moves locally and reports the final geometry once per gesture, plus throttled updates (≤10 Hz) during it. The CPU can veto by sending `WGEOM` back. |
| Focus window, focus widget, focus chain | ESP | CPU, via `FOCUS` events | Tab order comes from the CPU's tree order; the ESP walks it. |
| Menu tree, command ids, labels, accelerators | CPU (defines) | ESP (navigates) | Enabled/checked state is pushed by the CPU (`CMDSTATE`). Opening, navigating and closing a pulldown is ESP-local. The CPU gets one `COMMAND id` event. |
| Animation tweens | ESP (runs) | CPU (requests) | `ANIM` op; `ANIMDONE` event. CPU-driven per-frame updates are also allowed (§9). |
| Pointer position and cursor | ESP | CPU, via coalesced `POINTER` events | Cursor drawn by the compositor, not a fabgl sprite (§5.6). |
| Theme (role → palette index, frame style) | CPU (sets) | ESP (renders) | One `THEME` op per role. |

The recovery rule is unchanged. On an ESP restart or a VDS epoch change, the CPU
replays its constructors. Window geometry the user changed since the last `WIN`
event is lost, which is acceptable.

---

## 5. Compositor

### 5.1 Objects

- **Window:** pixel rect `(x, y, w, h)` with flags (frame, title, shadow, modal,
  resizable, always-on-top). It has a decoration theme and a client area bound to
  a surface or canvas, with a scroll offset. Pools are fixed at init, starting at
  32 windows.
- **Surface:** a `cols×rows` cell grid `(ch, attr)` in PSRAM with per-row dirty
  spans, as in the existing §6. The `attr` layout is unchanged (`fg | bg<<4`
  palette indices, already 16-colour-capable); indices 4–15 become meaningful in
  mode 0.
- **Canvas:** a retained display list (lines, rects, fills, ellipses, text, bitmap
  refs). An optional pixel backing store takes 4 bpp × w × h in PSRAM.
- **Overlays:** menus, tooltips, the drag outline and the pointer. These are
  windows in a top z-band that are never saved to a document.

### 5.2 Horizontal alignment rule

At 4 bpp, one byte holds 2 pixels. A glyph cell (8 px) is exactly 4 bytes, so a
cell-aligned glyph row is one 32-bit store.

**Rule:** window client-area *x* snaps to a multiple of 8 px; *y* is free.

- With this rule, every text glyph blit is aligned word stores with no shifts.
- Frames and shadows can still be drawn at any pixel.
- Horizontal moves and animation step 8 px at a time, which looks smooth at
  60 Hz on a 640-px screen (80 positions).
- Vertical motion is pixel-smooth.
- Allowing any *x* is possible later, at the cost of shifted glyph blits. That's
  an owner decision (§16).

### 5.3 Dirty rectangles and occlusion

- Every op that changes something visible marks damage in screen coordinates.
  This includes a surface edit inside a visible window, a geometry or z change,
  an animation step, and a pointer move.
- The damage list is bounded at 32 rects. On overflow, the closest pair merges
  into its bounding box, so the list never grows without bound.
- Each window keeps a **visible region**: its rect minus the opaque rects above
  it (a window's shadow band is translucent, so it is never part of the opaque
  rect, but it is part of the window's move damage), as a short rect list. It is recomputed only when geometry or z-order changes
  (O(windows²), with ≤32 windows).
- Composition intersects each damage rect with each window's visible region,
  top-down, and rasterises only those pieces. Each pixel is written once per frame.
  The desktop fills whatever is left.
- **Moving a window** damages its old rect and its new rect. The exposed area is
  re-rasterised from the cell surfaces underneath. No pixel backing store is
  needed for text windows.
- **Shadows** are a translucent band, drawn by remapping colour indices through a
  16-entry "shade" lookup table as the pixels below are rasterised. That makes a
  real drop shadow with no alpha blending.

### 5.4 Blitter (estimates, to measure in G0)

- At 4 bpp, each `(fg, bg)` pair gets a lookup table of 256 entries × u32 (1 KB),
  mapping one font byte to 8 pixels. Eight cached pairs take 8 KB of internal RAM.
- One cell is then 8 × (font-byte load → table lookup → aligned 32-bit store).
  The VGA16 row layout is plain bytes with the high nibble as the left pixel
  (`VGA16_SETPIXELINROW` in `vga16controller.cpp`) and no word swap. On the
  little-endian ESP32, table entry byte 0 therefore holds pixels 0 and 1.
- *Estimate:* about 100 cycles per cell at 240 MHz. A full 4800-cell screen
  would take about 2 ms, and a 40×20-cell window move (old plus new rect, about
  1600 cells) under 1 ms.
- The compositor writes `getScanline(y)` rows directly. The paletted controllers
  expose `sgetScanline`, a vertical-blank interrupt (`setProcessPrimitivesOnBlank`)
  and `swapBuffers` (`vdp-gl/src/dispdrivers/vgapalettedcontroller.h`).
- Before a batch, the compositor waits for queued fabgl primitives to finish
  (`canvas->waitCompletion`, as today), so it never races legacy drawing.

### 5.5 Frame loop and tearing

- The compositor runs as a **60 Hz frame**, started at vertical blank. Damage that
  arrives during a frame is coalesced into the next one.
- **Single-buffered (mode 0):** rasterise damage rects in top-to-bottom order,
  starting at vertical blank. The beam needs 15.3 ms for 480 lines. While
  composition stays ahead of the beam, the frame shows no tear. *Estimate:*
  ordinary frames (≤1600 cells) finish in about 1 ms.
- If a frame's damage would not finish ahead of the beam, the compositor stops at
  a line boundary and finishes in the next frame. The result is one dropped frame,
  never a torn window.
- **Double-buffered (mode 129 fallback):** compose into the back buffer, then
  `swapBuffers` at vertical blank. The back buffer is two frames stale, so it must
  also receive the previous frame's damage (buffer age 2).
- **How the compositor gets control:** the existing design runs VDS inside the
  VDP task's `loop()`. Here the paletted controller's vertical-blank interrupt
  sets a flag or gives a semaphore, and the VDP task starts the frame at its next
  check between VDU commands.
  - That start latency is bounded by the longest VDU primitive in flight.
  - Legacy primitives may also execute at blank (`setProcessPrimitivesOnBlank`),
    competing for the same window.
  - The paletted ISR converts framebuffer lines into DMA line buffers a few lines
    ahead of scanout, so "ahead of the beam" means ahead of that conversion lead.
  - G0 measures both the vertical-blank → compositor-start latency and the lead.
- **Budget:** at most 4 ms of compositing per 16.7 ms frame, so UART parsing and
  the VDP task are never starved.
- **Exit test (G0):** a scripted window drag must show zero torn frames, checked
  with an ESP-side frame counter and the probe.

### 5.6 Pointer

- The compositor draws the pointer in its final pass, with a save-under of its
  16×16 rect.
- fabgl sprites are not used for the pointer: their saved backgrounds are
  corrupted by direct framebuffer blits, as the existing design notes.
- A pointer move costs two small damage rects per frame, coalesced to at most one
  per frame.

---

## 6. Window manager (ESP intrinsics, CPU policy)

- **Decorations:**
  - frame, title bar with mnemonic, close/zoom/minimise glyph buttons;
  - resize grip and scrollbars bound to the client surface's scroll offset;
  - inactive windows drawn in the inactive palette roles.
- **Intrinsics:** move, resize, raise, lower, minimise to the window list,
  maximise or restore, tile and cascade, and focus cycling.
- **Modal dialogs:** a modal window restricts focus and pointer hits to itself and
  its children, and dims the rest using the shade table.
- **Events to the CPU:**
  - `WIN moved|resized|raised|minimised|close-request` with the geometry;
  - `FOCUS` changes.
- **Close is a request:** the CPU decides (unsaved draft → confirmation dialog)
  and then sends `DEL` or ignores it.
- **Policy stays in Forth/C on the CPU:** which windows exist, what close means,
  and saved layouts (`UI-SNAPSHOT` also records geometry).
- **Keyboard move/resize mode** (Turbo Vision Ctrl+F5) is described in §8.

---

## 7. Widgets and layout

Widgets render into their window's cell surface. Pixel extras such as frames,
focus rings and the scrollbar thumb are drawn by the compositor from the widget
rects.

| Widget | ESP intrinsics | Events | Stage |
|---|---|---|---|
| Window, Dialog (modal) | §6 | `WIN`, `FOCUS` | G2 |
| MenuBar, Menu (pulldown), ContextMenu | Open, navigate, mnemonic, accelerator match, submenus, close on Esc or click-out | `COMMAND id` | G3 |
| CommandPalette | Fuzzy filter over the command table, recent commands first | `COMMAND id` | G3 |
| StatusLine | Key hints for the focused context (Turbo Vision style) | `COMMAND id` | G3 |
| Button, Check, Radio | Focus ring, press feedback, Space/Enter, mnemonic | `ACTIVATE`, `CHANGE` | G4 |
| Input (LineField) | Editing, history, selection, clipboard | `CHANGE`, `ACTIVATE` | G4 (exists in the plan as Stage 2) |
| TextView | Retained document, cursor, selection, speculative edits | `CHANGE` (revision-checked) | G4 |
| List, Tree, Table | Virtual paging, type-ahead, sort indicator | `SELECT`, `ACTIVATE`, page requests | G4 |
| ScrollBar, Splitter, Tabs | Drag or keys, local | `SCROLL`, `RESIZE`, `SELECT` | G4 |
| ProgressBar, Label, Icon | Render, animate | — | G4/G5 |
| Canvas, Sprite | Display-list replay, tweened motion | `POINTER` (optional) | G5 |

**Layout** runs inside windows in cell units, computed on the ESP. It is
deterministic integer arithmetic, like the existing §8.

- Rows and columns take constraints: `fixed n | min m | weight w | percent p`,
  following ratatui's and Textual's constraint layouts.
- Children can anchor to their parent's edges, for dialogs.
- Re-layout happens on window resize with no CPU round trip.
- The CPU can pull the result (`GETP rect`), so MCP and inspectors see real
  geometry.

---

## 8. Keyboard model (complete, ESP-routed)

Every action is reachable by keyboard (R3). The ESP key router (existing §9)
resolves keys in this order:

1. Active menu or palette
2. Modal dialog
3. WM mode
4. Global accelerators
5. Focused widget
6. Forward to the CPU (`0x81`)

| Key | Action |
|---|---|
| F10, Alt alone | Activate the menu bar |
| Alt+letter | Open the menu, or press the control whose mnemonic is underlined |
| ←/→, ↑/↓, Enter, Esc (in menus) | Move between menus, move between items, choose, close one level |
| Tab, Shift+Tab | Next and previous control in the window's focus chain |
| Ctrl+Tab, Ctrl+Shift+Tab | Next and previous tab (Tabs widget) |
| F6, Shift+F6 / Alt+Tab | Next and previous window |
| Alt+0 | Window list |
| Alt+F3, Ctrl+W | Close window (sends a close request) |
| F5 | Zoom (maximise/restore) |
| Ctrl+F5 | WM mode: arrows move by 8 px horizontally and 1 cell vertically; Shift+arrows resize; Enter keeps the change, Esc reverts it |
| Ctrl+Shift+P (also Ctrl+K) | Command palette |
| Esc | Unwind the innermost layer: menu → dialog → WM mode → selection |
| F1 | Context help (CPU command) |
| Ctrl+Space | Zoom the editor (as today) |

Every widget shows a focus ring, and mnemonics are underlined. The status line
shows the keys valid in the current context. Accelerators and mnemonics come from
the CPU's command table, so keyboard, menu, palette, CLI and MCP all invoke the
same command ids.

---

## 9. Animation

- **ESP tween engine:** `ANIM id prop from to ms easing`.
  - Properties: window x/y/w/h, scroll offset, canvas object position, palette
    entry.
  - Easings: linear, ease-in-out and spring, all in fixed-point.
  - It steps once per 60 Hz frame. Each step only marks damage, which the
    compositor already handles.
  - Up to 16 tweens run at once. `ANIMDONE id` reports completion.
  - Uses: window open/close slides, menu drop, scrolling, the progress bar.
- **CPU-driven real-time animation** (games, live plots) streams per-frame
  updates: a sprite or window position is about 6–10 B per object per frame.
  - *Estimate:* 20 objects × 10 B × 60 Hz = 12 KB/s, about 10 % of UART2 at
    1.152 Mbaud. That's well within budget.
  - The ESP renders whatever arrived by each frame's vertical blank, so a slow
    CPU drops frames without tearing.
- **Palette animation** (cycling, fades, a pulsing selection) costs one op and no
  pixels in paletted modes.

---

## 10. Mouse (R4) and its hardware cost

The pointer model is input-agnostic: a keyboard pointer, host injection from the
harness, and later a physical mouse. They all produce the same `POINTER` stream,
with hit testing on the ESP.

**Hardware cost the owner must weigh:**

- fabgl's mouse is a PS/2 device on ESP **GPIO26/27**. On this board those pins
  carry the **ZDI recovery wires**: a "USB mouse" here means a PS/2 mouse on
  those pins.
- The expansion header (Rev. B, `docs/research/hardware.md`) exposes ESP GPIO26,
  27, 35, 36, 37, 38 and 39. GPIO34–39 are **input-only** on the ESP32, so
  **26/27 are the only exposed output-capable ESP pins**. ZDI cannot move
  elsewhere, and neither can PS/2's bidirectional clock and data.
- **Enabling the mouse therefore removes the independent ESP→eZ80 recovery
  route** that the handoff relies on (forced reboot of a wedged CPU, register and
  memory inspection).
- A "USB" mouse must be dual-mode USB/PS/2 through a passive adapter. Many
  current mice are USB-only.

**Proposed approach:**

- **Two builds:** `dev` (ZDI wired, mouse off) and `user` (mouse on GPIO26/27,
  ZDI recovery disabled and reported as such).
- Each has a boot-time check that refuses to enable the mouse if ZDI responds.
- In the `user` build, recovery relies on the ESP firmware A/B rollback (already
  implemented) and SD source-slot rollback.

---

## 11. Code in C (R7) and layering

| Layer | Language | Location | Notes |
|---|---|---|---|
| Compositor, blitter, WM, menus, layout, widget intrinsics, animation | C (C++ only for fabgl glue) | `firmware/esp32-vios/lib/vios-vds/` (portable, no fabgl or Arduino) plus `include/vios_vds_fabgl.h` (adapter) | Host-testable, rendering into a fake 4 bpp framebuffer with golden images (`firmware/esp32-vios/tests/vds.cpp`). |
| UI client: window/widget/command tables, op encoder and batcher, event dispatch, `/dev/win` | C | `kernel/vios_ui.c`, `drivers/agon-vdp/vds.c` | Fixed tables, no heap. Dispatch to Forth handlers through `forth_observe` (4096-instruction budget). |
| Apps, handlers, layouts, themes, window policy | Forth | modules | Thin `UI` vocabulary over the C natives (existing §10 examples still apply). |

**CPU RAM budget** (eZ80):

- *measured:* the main tree's BSS ends at `0xA665F`, leaving about 35 KB below
  the stack at `0xAF000`.
- The C client adds an *estimated* 6–9 KB of code plus about 4 KB of tables
  (64 widgets × 24 B, 32 windows × 16 B, 64 commands × 24 B, plus names).
- That is paid for by reclaiming `text_screen`'s `painted`/`desired` planes
  (19.2 KB) and the workbench view buffers (5.4 KB) once the editor runs in a
  window. This is the existing §12.1 reclaim, now made concrete.

---

## 12. Wire additions (draft, inside `E4` batches)

Op ranges continue the existing table in §5.2. All coordinates are pixels unless
an op says cells. u16 values are little-endian.

| Op | Meaning |
|---|---|
| `50 WIN id x(u16) y(u16) w(u16) h(u16) flags` | Create or replace a window (x snapped per §5.2) |
| `51 WGEOM id x y w h` | Set geometry (CPU veto or programmatic move) |
| `52 WZ id op` | Raise, lower, or top band |
| `53 WSTATE id s` | Normal, minimised or maximised |
| `54 WTITLE id n bytes…` | Title, with `&` marking the mnemonic |
| `55 WCLIENT id kind ref` | Bind a surface or canvas as the client area |
| `60 CMD id(u16) flags key mods n label…` | Define a command (accelerator, mnemonic) |
| `61 MENU id parent(u16) n cmd(u16)…` | Menu or submenu contents (menu bar = id 0) |
| `62 CMDSTATE id(u16) flags` | Enabled and checked state |
| `70 ANIM id prop from(u16) to(u16) ms(u16) easing` | Start a tween |
| `71 ANIMSTOP id prop` | Stop a tween |
| `72 THEME role colour` | Theme role → palette index |
| `73 SHADE idx shade-idx` | Entry in the shadow lookup table |

New `F0` event kinds: `10 WIN`, `11 COMMAND`, `12 ANIMDONE`, `13 DRAG`. `07 POINTER`
gains the window id and window-local coordinates.

---

## 13. Performance targets and benchmarks

Baselines *measured* on the board on 2026-10-19:

- full workbench compose on the CPU: 39.0 ms (it was 125.7 ms on Oct 16). Same
  method (`WB-REPAINT WB-COMPOSE`) and a similar 1969-byte fixture (WBM1P), not
  the benchmark's `WBFAST`/MODULES scene;
- idle key-to-glyph: 1.0 / 1.3 ms (median/p95, Stage 1c);
- console flood: 7058 glyphs/s.

| ID | Scenario | Target |
|---|---|---|
| B10 | Keyboard drag of a 40×20-cell window across the screen (Ctrl+F5, held arrow) | 60 fps, 0 torn frames, ≤2 ms compositor time per frame, 0 CPU bytes until Enter |
| B11 | Menu open (F10 → ↓) and navigation | Key to visible menu ≤ 1 frame (16.7 ms), independent of CPU load |
| B12 | Overlap correctness: 8 overlapping windows, random raise/move/edit sequence | `GET` and pixel readback match a host golden render, 100 % |
| B13 | 16 concurrent tweens plus a CPU-streamed 20-sprite animation | 60 fps with no dropped frames at idle CPU; UART2 ≤ 15 % |
| B14 | Typing in an editor window behind a moving window | Key-to-glyph p95 ≤ 3 ms (Stage 2 LineField/prediction) |
| B9+ | ESP free internal heap after mode set plus VDS pools | ≥ 24 KB |

---

## 14. Staged plan

Each stage is a reviewable vertical slice:

- host tests first;
- matched-baud CPU/ESP builds;
- install through the A/B installer (`tools/vios_firmware_ab.py`, with backup and
  automatic rollback);
- acceptance JSON under `docs/bring-up/`;
- the old path stays selectable until the new one passes.

Effort figures are rough, for one focused developer-agent.

| Stage | Work | Accept | Effort |
|---|---|---|---|
| **G0: gates** | Heap after mode 0/129/1 with pools; 4 bpp blitter microbenchmark; vertical-blank hook prototype racing the beam; vertical-blank → compositor-start latency and ISR line lead | Heap ≥ 24 KB free in the chosen mode; blitter cells/ms measured; tear test passes or the fallback is chosen | 1–2 days |
| **G1: compositor core** (replaces old 1b) | Mode 0 with palette 0–3 pinned; acceptance tools updated for 16 colours; windows plus cell surfaces, dirty rects, occlusion, shade table, pixel frames, 60 Hz frame loop; console and editor become two windows; `RAW` hand-back kept | B4 parity or better; B12; console acceptance unchanged | 1–2 weeks |
| **G2: window manager** | Decorations, move/resize/raise/min/max, keyboard WM mode, focus cycling, modal dialogs, `WIN` events, geometry in `UI-SNAPSHOT` | B10; all §8 window keys | 1–2 weeks |
| **G3: commands and menus** | Command table, menu bar, pulldowns, context menu, accelerators, mnemonics, palette, status line; workbench actions exposed as commands | B11; every workbench action reachable by menu, accelerator and palette | 1–2 weeks |
| **G4: widgets and client** | Widget set and layout (§7); `kernel/vios_ui.c`; `UI` vocabulary; `/dev/win`; workbench panes ported (old Stage 3) | B7 and B14; CPU RAM reclaim per §11 | 2–3 weeks |
| **G5: animation and graphics** | Tween engine, palette animation, Canvas display lists, sprites, icons via `A0` buffers | B13 | 1 week |
| **G6: pointer** | Keyboard pointer and host injection first; physical mouse in the `user` build after the owner decides (§10) | Pointer B10/B11 variants; recovery behaviour documented | 3–5 days plus wiring |

Stage 2 of the existing plan (ESP line editing) stays as it is and can run
between G1 and G2.

---

## 15. Risks

| Risk | Mitigation |
|---|---|
| Mode 0 leaves too little internal heap | G0 gate; fallback to mode 129 or mode 1 with a 4-colour theme; fixed pools; content in PSRAM |
| Composition falls behind the beam (tearing) | Line-boundary split into the next frame; 4 ms budget; mode 129 fallback |
| PSRAM bandwidth for big expose areas | Cell surfaces are small (9.6 KB per full screen); keep glyph tables and active window metadata in internal RAM; measure in G0 |
| ESP and CPU state diverge | Existing batch atomicity, `SYNC` epoch, CPU replay; geometry reported per gesture |
| Feature creep in ESP intrinsics turns policy into firmware | Intrinsics are limited to frame-time behaviour (§4 rule); meaning stays in CPU handlers; every intrinsic has an inspectable state through `GETP` |
| Mouse removes ZDI recovery | Two builds, boot check, explicit owner decision (§10) |
| eZ80 code growth | Reclaim the planes first; Stage gate on the measured gap ≥ 12 KB (existing rule) |

---

## 16. Owner decisions requested

1. **Display mode:** mode 0 (640×480, 16 colours, single buffer, beam-ordered
   composition) as the target, subject to the G0 heap and tear gates, with mode 129
   (4 colours, double buffer) as the fallback.
2. **Mouse versus ZDI:** accept that the physical mouse lives in a separate `user`
   build that gives up ZDI recovery; the `dev` build keeps ZDI and has no mouse.
3. **Horizontal snap:** client-area x snaps to 8 px (fast aligned glyph blits;
   horizontal motion in 8-px steps). The alternative is free x with shifted blits.
4. **Look:** Turbo Vision-style theme as the default, with pixel frames and
   shadows, plus a command palette and constraint layouts from modern TUIs.
5. Approve **G0** as the next step: measurement only, plus one A/B-installed probe
   build.

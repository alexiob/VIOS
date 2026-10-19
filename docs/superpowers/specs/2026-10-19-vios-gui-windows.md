# VIOS GUI: windows, widgets and a pixel compositor (amendment)

Status: **design proposal, 2026-10-19; G0 gates measured on the board the same day**
([report](../../bring-up/vios-gui-g0-2026-10-19.md), commit `308c464`). No
compositor has been built yet. Numbers marked *measured* come from board evidence
in this repo; numbers marked *estimate* or *target* are still unverified.

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

**Look: Smalltalk by default, Turbo Vision as an alternative** (owner decision,
§16). See [§2.1](#21-two-presentation-styles-one-toolkit).

### 2.1 Two presentation styles, one toolkit

Both styles run on the same compositor, widgets, command table, events and
keyboard model. A *style* changes only how windows are decorated and how commands
are presented:

| Aspect | Smalltalk (default) | Turbo Vision |
|---|---|---|
| Desktop | Grey dithered pattern | Blue dithered pattern |
| Window frame | 1 px black border (2 px when focused), white body | Double pixel frame on a blue body |
| Title | Tab on the top-left edge | Centred in the top frame, with close and zoom boxes |
| Shadow | Soft 4 px pixel shadow | 2-cell × 1-cell translucent drop shadow |
| Selection | Inverted (black) in the focused pane, grey elsewhere | Green bar |
| Commands | **Pop-up menus:** a per-pane menu (Shift+F10 or the Menu key) and a world menu (F10) | **Menu bar** with pulldowns (F10, Alt+letter) and a status line of key hints |
| Typical tools | Multi-pane browsers (modules / categories / words / views over source), workspace, transcript, inspector | Editor windows, dialogs, lists |

**Commands are defined once.** The CPU's command table (`CMD`/`MENU`) has the
labels, accelerators and enabled state. The ESP presents the same tree as a menu
bar (Turbo Vision) or as world and pane pop-ups (Smalltalk). Accelerators, the
command palette, the CLI and MCP all invoke the same command ids in either style.

**Memory and selection.**

- A style is about 80 bytes of data (16 palette entries, 16 shade entries,
  roughly 40 bytes of decoration parameters) plus its decoration and menu
  presentation code.
- *Estimate:* under 1 KB of ESP internal RAM for both styles together, and
  6–10 KB of flash each. App0 flash has about 590 KB free (*measured* 716,752 B
  of 1,310,720 B).
- The CPU stores only the selected style id.
- So **both styles are compiled in by default and switchable at run time**,
  with `CONFIG style 0|1` and the Forth word `UI-STYLE`. A switch re-decorates
  and repaints every window; no window state is lost.
- Build flags `VIOS_UI_STYLE_SMALLTALK` and `VIOS_UI_STYLE_TURBO` (both on by
  default) can drop a style if flash ever gets tight; the build fails if both are
  off.
- Each style ships with its own default palette. Indices 0–3 stay pinned in both
  (§3).

From current open-source TUIs (Turbo Vision's magiblot port, Textual, notcurses,
ratatui), both styles take a command palette, constraint layouts, theming and an
always-visible focus indicator.

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

**Gate G0: passed** (*measured* 2026-10-19). The gate required ≥24 KB of
internal RAM free in mode 0 with the compositor's pools allocated (12 KB internal
plus 4 × 9.6 KB surfaces in PSRAM):

| Mode | Internal free | Largest block | Verdict |
|---|---:|---:|---|
| **0** (640×480, 16 colours) | **36,856 B** | 29,684 B | pass: 12.9 KB above the floor |
| 129 (4 colours, double) | 36,296 B | 29,684 B | pass, not needed |
| 1 (today) | 113,464 B | 98,292 B | reference |

PSRAM has about 4.1 MB free in every mode. **Mode 0 is confirmed.** Mode 129 is
not needed as a fallback (§5.5).

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
- **Configurable (owner decision, §16):** a global `snap_x` setting, 8 (default)
  or 1 (free x), set with `CONFIG snap_x n`, plus a per-window flag that can
  override it. With free x, glyph blits shift by `x & 7` pixels: two partial
  32-bit stores per glyph row instead of one aligned store. The blitter has both
  paths from G1, and B10 is measured in both modes.

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
- *Measured (G0):* the aligned 4 bpp table blitter (x snap 8) runs at **719
  cells/ms**.
  - A full 4800-cell screen takes 6.7 ms. The design estimate was about 2 ms;
    the real rate is 3.3× slower but still well inside the 15.3 ms scan.
  - A 40×20-cell window move (old plus new rect, about 1600 cells) takes about
    2.2 ms.
- *Measured (G0):* free x (two masked read-modify-write stores per glyph row)
  runs at **325 cells/ms**, 14.7 ms for a full screen. That is 2.2× the cost of
  the aligned path, so 8 px snap stays the default.
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
  composition stays ahead of the beam, the frame shows no tear.
  - *Measured (G0):* frame period 16,683 µs, ISR line counter 0..480, conversion
    lead 2 lines (4 DMA line buffers, 2 per interrupt).
  - The tear test drew 60 beam-ordered frames from vertical blank, for both the
    full screen (4800 cells) and a 40×20 window: **0 torn frames, 0 late rows**.
  - Ordinary frames (≤1600 cells) take about 2.2 ms.
- If a frame's damage would not finish ahead of the beam, the compositor stops at
  a line boundary and finishes in the next frame. The result is one dropped frame,
  never a torn window.
- **Double-buffered (mode 129):** no longer needed, since G0 showed no tearing in
  single-buffered mode 0. Kept only as a documented alternative: compose into the
  back buffer, `swapBuffers` at vertical blank, and also re-blit the previous
  frame's damage (buffer age 2).
- **How the compositor gets control:** the existing design runs VDS inside the
  VDP task's `loop()`. Here the paletted controller's vertical-blank interrupt
  sets a flag or gives a semaphore, and the VDP task starts the frame at its next
  check between VDU commands.
  - That start latency is bounded by the longest VDU primitive in flight.
  - Legacy primitives may also execute at blank (`setProcessPrimitivesOnBlank`),
    competing for the same window.
  - The paletted ISR converts framebuffer lines into DMA line buffers a few lines
    ahead of scanout, so "ahead of the beam" means ahead of that conversion lead.
  - **G0 finding (constraint for G1):** the VDP loop gap is *measured* at 0.09 ms
    idle and 1.1 ms while typing. It grows to 4.8 ms on `WB-OPEN`, 7.9 ms on a
    repaint, 13.4 ms per console flood record, and **66.2 ms on `WB-CLOSE`**.
    There, the ESP console's `show()` (`VDU 23,0,E5,1`) clears the screen and
    queues about 4,600 fabgl glyph primitives inside one VDU command. A
    compositor polled from the loop would miss 3–4 frames. G1 therefore must:
    1. Draw console and surface content with the direct blitter, never fabgl
       glyph primitives. A full console redraw is then about 6.5 ms at 719
       cells/ms.
    2. Bound the work of any single VDU command, and split large redraws across
       frames as damage instead of drawing them inside the command.
    3. Start frames from the vertical-blank notification rather than a loop
       poll. G1 acceptance includes a loop gap ≤ 4 ms in every scenario above.
- **Budget:** at most 4 ms of compositing per 16.7 ms frame, about 2,900 cells at
  the measured rate, so UART parsing and the VDP task are never starved. Damage
  beyond the budget carries over to the next frame in beam order. The one
  exception is a full-screen redraw (6.7 ms), which may run in a single frame
  because G0 showed it stays ahead of the beam.
- **Exit test (G0): passed.** It showed zero torn frames, checked with an
  ESP-side frame counter and the probe. G1 repeats it as a scripted window drag
  (B10).

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
| F10, Alt alone | Activate the menu bar (Turbo Vision) or open the world menu (Smalltalk) |
| Shift+F10, Menu key | Pop-up menu for the focused pane or widget (both styles) |
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
| `74 CONFIG key value(u16)` | Compositor settings, e.g. `snap_x` (8 or 1), `style` (0 Smalltalk, 1 Turbo Vision) |

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
| **G0: gates — done** | Heap after mode 0/129/1 with pools; 4 bpp blitter microbenchmark; vertical-blank hook prototype racing the beam; vertical-blank → compositor-start latency and ISR line lead | **Passed (2026-10-19):** mode 0 leaves 36.9 KB free; blitter 719 cells/ms aligned, 325 free x; 0 torn frames; new loop-gap constraint (§5.5) | done |
| **G1: compositor core** (replaces old 1b) | Mode 0 with palette 0–3 pinned; acceptance tools updated for 16 colours; console and surfaces drawn with the direct blitter (no fabgl glyph primitives), bounded per-command work, frames started from vertical blank; windows plus cell surfaces, dirty rects, occlusion, shade table, pixel frames, 60 Hz frame loop; console and editor become two windows; `RAW` hand-back kept | B4 parity or better; B12; console acceptance unchanged; VDP loop gap ≤ 4 ms in the G0 scenarios (`WB-CLOSE` was 66 ms) | 1–2 weeks |
| **G2: window manager** | Both decoration styles, switchable at run time; move/resize/raise/min/max, keyboard WM mode, focus cycling, modal dialogs, `WIN` events, geometry in `UI-SNAPSHOT` | B10; all §8 window keys | 1–2 weeks |
| **G3: commands and menus** | Command table presented as world/pane pop-ups (Smalltalk) and as a menu bar with pulldowns (Turbo Vision); context menu, accelerators, mnemonics, palette, status line; workbench actions exposed as commands | B11; every workbench action reachable by menu, accelerator and palette | 1–2 weeks |
| **G4: widgets and client** | Widget set and layout (§7); `kernel/vios_ui.c`; `UI` vocabulary; `/dev/win`; workbench panes ported (old Stage 3) | B7 and B14; CPU RAM reclaim per §11 | 2–3 weeks |
| **G5: animation and graphics** | Tween engine, palette animation, Canvas display lists, sprites, icons via `A0` buffers | B13 | 1 week |
| **G6: pointer** | Keyboard pointer and host injection first; physical mouse in the `user` build after the owner decides (§10) | Pointer B10/B11 variants; recovery behaviour documented | 3–5 days plus wiring |

Stage 2 of the existing plan (ESP line editing) stays as it is and can run
between G1 and G2.

---

## 15. Risks

| Risk | Mitigation |
|---|---|
| Mode 0 leaves too little internal heap | **Retired by G0** (36.9 KB free with pools); fixed pools and PSRAM content keep the margin |
| Composition falls behind the beam (tearing) | **Retired by G0** (0 torn frames at full screen); line-boundary carry-over and the 4 ms budget remain |
| Long VDU commands stall the VDP loop (66 ms measured on `WB-CLOSE`) | G1 rules in §5.5: direct blitter, bounded per-command work, vertical-blank-driven frames; acceptance gate ≤ 4 ms |
| PSRAM bandwidth for big expose areas | Cell surfaces are small (9.6 KB per full screen); keep glyph tables and active window metadata in internal RAM; measure in G0 |
| ESP and CPU state diverge | Existing batch atomicity, `SYNC` epoch, CPU replay; geometry reported per gesture |
| Feature creep in ESP intrinsics turns policy into firmware | Intrinsics are limited to frame-time behaviour (§4 rule); meaning stays in CPU handlers; every intrinsic has an inspectable state through `GETP` |
| Mouse removes ZDI recovery | Two builds, boot check, explicit owner decision (§10) |
| eZ80 code growth | Reclaim the planes first; Stage gate on the measured gap ≥ 12 KB (existing rule) |

---

## 16. Owner decisions (2026-10-19)

1. **Display mode: decided.** Mode 0 (640×480, 16 colours, single buffer,
   beam-ordered composition). Mode 129 stays only as the fallback if the G0 gates
   fail.
2. **Mouse: deferred.** No physical-mouse work for now. The pointer model (§10)
   stays input-agnostic, and G6 is limited to the keyboard pointer and host
   injection until the owner revisits the hardware trade.
3. **Horizontal snap: both, by configuration.** 8 px is the default, with free x
   available (§5.2).
4. **Look: decided.** Smalltalk-style graphical UI is the default. Turbo Vision
   stays available: both are compiled in and switchable at run time, because the
   RAM cost is under 1 KB, and a build flag can drop either one (§2.1). The
   choice was made from host-rendered mockups at 640×480 in 16 colours with the
   device's own 8×8 font, in [`docs/gui/mockups/`](../../gui/mockups/) (produced
   by `tools/gui_mockup.py`).
5. **G0: approved and done** (2026-10-19, `308c464`). All gates pass. One new G1
   constraint: no VDU command may stall the VDP loop (§5.5).

---

## G0 results (measured on the board, 2026-10-19)

Full report: `docs/bring-up/vios-gui-g0-2026-10-19.md` (commit 308c464).

| Gate | Result | Verdict |
| --- | --- | --- |
| Internal heap, mode 0, with pools | 36,856 B free, largest block 29,684 B | pass (floor 24 KB) |
| Mode 129 / mode 1, with pools | 36,296 B / 113,464 B free | reference |
| 4 bpp LUT blitter, aligned (§5.4) | 719 cells/ms, full screen 6.7 ms (estimate ~2 ms) | measured |
| Free-x proxy (§5.2) | 325 cells/ms, full screen 14.7 ms | snap 8 default confirmed |
| Beam (§5.5) | frame 16,683 us; ISR counter 0..480; lead 2 lines | as modelled |
| Tear test, beam order from vblank, 60 frames each | 0 torn: full screen and 40x20 window | pass: mode 0 single-buffered; no mode 129 fallback |
| VDP loop gap (vblank -> compositor start) | idle 0.09 ms, typing 1.1, repaint 7.9, console flood 13.4, **WB-CLOSE 66.2 ms** | G1 constraint |

**G1 constraint from G0:** one VDU command can block the VDP loop for 66 ms
(the Stage 1c console `show()` queues ~4,600 fabgl glyph primitives). G1 must
draw surfaces with the direct blitter instead of fabgl primitives (a full
console redraw is ~6.5 ms at the measured rate), bound per-command work and
split large redraws across frames, and start frames from the vertical-blank
notification instead of a loop poll.

## G1 progress: portable compositor core (host-tested, 2026-10-19)

`firmware/esp32-vios/lib/vios-vds/src/vds.{h,c}` is plain C99 with no heap,
fabgl or Arduino. It implements:

- windows over shared 8×8 cell surfaces;
- a bounded damage list (32 rects; a full list grows the rect with the least
  added area);
- beam-ordered frames with a pixel budget and scanline-boundary carry-over;
- occlusion culling (painting starts at the topmost window whose body covers the
  span);
- shade-table shadows, and both decoration styles switchable at run time;
- 8 px snap or free x, globally or per window;
- an aligned 4 bpp table blitter with a generic free-x and clipped path.

Each damaged scanline span is composed in a 320-byte line buffer (desktop, then
windows bottom to top) and copied to the framebuffer once. The screen therefore
never shows an intermediate state and needs no region algebra.

The native test (`firmware/esp32-vios/tests/vds.c`, `tests/test_esp_vds.py`)
runs with the real Agon font under ASan/UBSan. Its main oracle: after any
operation sequence and any budget-limited frames, the retained framebuffer
equals a fresh full render of the same scene. It covers 3 × 3000 random
operations (open, move, resize, raise, flags, scroll, title, close, style and
snap switches, surface writes), with partial frames that really carry damage
over.

The test also checks beam order, occlusion, raise, shadow colours, title tabs,
the 2 px active border, snap and free-x glyph placement, and that the damage list
stays bounded. The random test found one real bug, now fixed: a Turbo Vision
title wider than its window drew outside the window's damage rect.

ESP32 build of the core (xtensa-esp32-elf-gcc, `-Os`): **6,012 B of code;
state 10,464 B** (8 LUT slots × 1 KB, 32 windows, damage list, line buffer).
That is within the 12 KB internal pool G0 measured against. Surface cells stay
in PSRAM.

**Still to do for G1 (on-board integration, owned by the session with the
board):**

- fabgl adapter: row pointers from `sgetScanline`; palette from `vds_style.palette`;
- vertical-blank notification driving `vds_frame(v, budget)`;
- `E4` op decoding onto this API;
- console `show()` and surfaces moved onto the blitter (no fabgl glyph
  primitives);
- the loop-gap ≤ 4 ms gate.

## Implementation status (host-tested, 2026-10-19)

On main (`93340b4`..`648d948`); none of it has run on the board yet. Board
integration is in progress in the session that owns the hardware.

| Piece | Where | State |
|---|---|---|
| Compositor: windows, surfaces, damage, occlusion, shadows, both styles, snap/free x, blitter | `lib/vios-vds/src/vds.{h,c}` | done |
| Sprites (16, 4 bpp ≤ 64×64, transparency, hotspot) and a built-in 12×16 arrow cursor | `vds.c` | done; pointer = sprite 15 |
| Text caret (block, underline, bar), blinking from local time | `vds.c` (`vds_tick`) | done |
| Overlay band, menu frame and colours, modal dimming | `vds.c` | done |
| Wire protocol: batch validation, replies, hold/commit, raw, reset, sprite upload | `vds_proto.{h,c}` | done |
| G2 window manager: focus cycling, keyboard move/resize, zoom, close requests, modal focus | `vds_ui.{h,c}` | done |
| G3 commands, menu bar + status line (Turbo Vision), world and window pop-ups (Smalltalk), cascades, mnemonics, accelerators, palette | `vds_ui.c` | done |
| eZ80 client: 32 Forth words, argument checks, atomic batches, pulled events | `kernel/vios_ui.c`, `include/vios/ui.h` | done; linked into `vios-resident` |
| fabgl adapter, vertical-blank frames, E4 hook, console on the blitter, loop-gap gate | firmware | **in progress** (board owner) |
| G4 widgets: box (column/row layout by fixed size or weight), label, button, check, radio group, input, list, progress; Tab order, keys, pointer clicks, ACTIVATE/SELECT/CHANGE events, text read-back | `vds_widget.c`, ops 80–89 | done |
| G5 tweens (window x/y/scroll, sprite x/y; linear, in-out, out, spring) | `vds_ui.c`, ops 70/71 | done |
| G6 keyboard pointer (Ctrl+F12) and pointer injection, hit testing | `vds_ui.c`, op 7B | done; physical mouse deferred |
| G4 TextView, tree, table, tabs; G5 Canvas display lists and icons | — | not started |

**Deviations from the §12 draft** (the authoritative table is the header of
`vds_proto.h`):

- **Window ops:**
  - `50 WIN id surface x y cols rows flags`: client size in cells; the title
    is sent separately with `54`.
  - `51` sets geometry. `55` rebinds a window's surface, `56` sets its client
    scroll, `57` closes it and `58` focuses it.
- **Menu ops:** `59` sets a window's pop-up menu, `60` defines a command, `61`
  a menu and `62` a command's state. Item encoding: a command id, `FFFF` for
  a separator, `FE00|menu` for a submenu.
- **Sprite ops:** `76` defines a sprite, `77` uploads pixel data, `78` moves
  it, `79` shows or hides it, and `7A` loads the built-in arrow.
- **Events are pulled, not pushed:** the ESP queues 16 events (the newest are
  kept; a loss is reported as an `OVERFLOW` event). The CPU asks with `36 max`
  and gets at most `max` events in one 0xF1 reply. This uses the existing
  request/reply path, needs no CPU event ring in the transport, and gives
  flow control.
- **Reserved ids:** window ids 27–31 and surface ids 11–15 belong to the menus,
  the menu bar and the status line, and the protocol refuses them from the CPU.
- **Palette indices:** indices 0–3 are pinned in both styles, while 4–15 belong
  to the style. Content meant to look the same in both styles uses 0–3.

**Sizes** (as of `8fbfee8`; with widgets: ESP code 31 KB in total, `vds_proto` 4.7 KB and
`vds_ui` 10.7 KB of state, both PSRAM-capable; eZ80 `vios_ui.o` 8.7 KB code and
2.8 KB rodata, leaving about 24 KB below the stack, so the planned reclaim of
the 19.2 KB `text_screen` planes is now needed before large GUI growth on the CPU):

| Side | Component | Size |
|---|---|---|
| ESP32 (`-Os`) code | `vds` | 7.9 KB |
| | `vds_proto` | 3.5 KB |
| | `vds_ui` | 7.9 KB |
| ESP32 state | `vds` (internal RAM) | 10.8 KB |
| | `vds_proto` (can live in PSRAM) | 4.2 KB |
| | `vds_ui` (can live in PSRAM) | 6.1 KB |
| eZ80 | `vios_ui.o` | 6.4 KB code, 1.6 KB rodata, 0.5 KB BSS |
| eZ80 | `NATIVES_MAX` 48 → 96 | +288 B |

That leaves about 27.6 KB below the stack.

**Tests** (all native, ASan/UBSan, run by `make test`):

- `firmware/esp32-vios/tests/vds.c`: full-render oracle over 3 × 3000 random
  operations.
- `vds_proto.c`: protocol versus API equivalence, rejection, replies, pulled
  events, 20,000 fuzzed batches.
- `vds_ui.c`: window manager, 6000 random steps.
- `vds_menu.c`: menus, palette, accelerators, 5000 random steps, plus three
  regressions from the security review.
- `tests/vios_ui_host.c`: end to end. Forth words → E4 bytes → ESP library →
  replies and events back, compared with direct API renders.

**Forth vocabulary** (stack effects in `kernel/vios_ui.c`):

- Surfaces and text: `UI-SURFACE`, `UI-TYPE`, `UI-FILL`, `UI-SCROLL`, `UI-CARET`.
- Windows: `UI-WINDOW`, `UI-TITLE`, `UI-PLACE`, `UI-RAISE`, `UI-CLOSE`,
  `UI-FOCUS`, `UI-FLAGS`, `UI-CLIENT`, `UI-VIEW`.
- Commands and menus: `UI-COMMAND`, `UI-MENU`, `UI-STATE`, `UI-POPUP`.
- Sprites: `UI-SPRITE`, `UI-PIXELS`, `UI-SPRITE-AT`, `UI-SPRITE-SHOW`, `UI-ARROW`.
- Settings: `UI-STYLE`, `UI-SNAP`, `UI-RAW`, `UI-RESET`.
- Batching: `UI-BEGIN`, `UI-COMMIT`.
- Server queries: `UI-SYNC`, `UI-EVENT`, `UI-NEXT`.
- Constants: `UI-VISIBLE`, `UI-DECOR`, `UI-SHADOW`, `UI-ACTIVE`, `UI-FREE-X`,
  `UI-MODAL`, `UI-ENABLED`, `UI-CHECKED`, `UI-STATUS`, `UI-SEPARATOR`,
  `UI-SUBMENU`, `UI-CTRL`, `UI-SHIFT`, `UI-ALT`, `UI-EV-*`, `UI-CURSOR`.

Example:

```forth
0 60 20 48 UI-SURFACE                              \ surface 0: 60x20, black on white
S" : SQUARE DUP * ;" 0 1 1 48 UI-TYPE
1 0 64 40 60 20 UI-VISIBLE UI-DECOR OR UI-SHADOW OR UI-WINDOW
S" Browser" 1 UI-TITLE  1 UI-FOCUS
S" &Save" 3 UI-ENABLED 0 UI-CTRL CHAR s UI-COMMAND  \ command 3, Ctrl+S
3 1 S" &File" 1 UI-MENU  1 UI-SUBMENU 1 S" " 0 UI-MENU
UI-CURSOR UI-ARROW  UI-CURSOR 320 240 UI-SPRITE-AT  UI-CURSOR TRUE UI-SPRITE-SHOW
UI-NEXT  \ ( -- kind id arg TRUE | FALSE ): e.g. 17 1 3 TRUE after Ctrl+S
```

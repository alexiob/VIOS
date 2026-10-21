# VIOS GUI G5: canvas display lists and icons (design addendum)

Status: **proposal, 2026-10-21, awaiting owner review.** Nothing is built yet.

This fills in the last unbuilt row of the
[GUI windows design](2026-10-19-vios-gui-windows.md) (§14 G5, "Canvas display
lists, sprites, icons via `A0` buffers"; the tween and sprite parts of G5 are
already done). It refines the toolkit design's `Canvas` and `Icon`/`Bitmap`
widgets ([toolkit §6, §7](2026-10-19-vios-gui-toolkit.md)). Where this text
differs from those drafts, the differences are listed in §9.

## 1. Goal

A window can show vector and pixel graphics: lines, rectangles, fills, ovals,
text at any pixel and icons. The CPU describes the picture once as a
**retained display list** on the ESP and then edits single items. The ESP
keeps the list and redraws from it on damage, so the CPU never streams pixels
and holds no copy of the picture.

Non-goals for G5: anti-aliasing, arbitrary polygons and flood fill, scaling or
rotating bitmaps, per-item clipping regions, CPU read-back of pixels.

## 2. Model

- **Canvas:** a rectangle of pixels, `w × h`, placed on a **cell surface** at
  surface pixel position `(x, y)`. It has a background colour, or is
  *transparent* (its background shows the surface's cells underneath).
  Because it belongs to a surface, every window showing that surface shows it,
  and it moves with the window's scroll offset like the surface's text. Its
  surface position is fixed: scrolling the *cells* (`vds_scroll`) does not
  move it (owner question 2b). **x is a multiple of 8** (as window client x
  is), so an opaque canvas row lands byte-aligned. A canvas that fills a whole window
  is just a canvas at `(0, 0)` the size of the surface.
- **Item:** one entry of a canvas's display list, addressed by an **item id**
  0–254. Items draw in id order (higher ids on top). The CPU picks ids, so it
  can leave gaps to insert later. Setting an item that exists replaces it.
- **Bitmap:** a 4 bpp image (≤ 64 × 64, one transparent colour index) in a
  shared pool. An `ICON` item draws a bitmap at a pixel position. Bitmaps are
  shared by all canvases.

Limits (ESP, PSRAM):

| Thing | Limit | Memory |
|---|---|---|
| Canvases | 8 (ids 0–7) | descriptor 32 B each, internal RAM |
| Canvas size | ≤ 640 × 480 | backing `w·h/2` B; **total backing cap 256 KB** (e.g. one full screen 150 KB + several small) |
| Items | 255 per canvas | 32 B per item, a fixed 8 KB array per canvas |
| Bitmaps | 32 (ids 0–31; 24–31 built in) | shared pool 16 KB (a 64×64 bitmap is 2 KB, a 16×16 icon 128 B) |

The library stays heap-free: as for cell memory, the adapter supplies a
canvas's item array and backing raster through a callback (PSRAM on the
device, allocated when a canvas is first created and kept for reuse), and the
bitmap pool is a fixed array in `vds_proto` (PSRAM-capable). The compositor's
internal-RAM state grows only by the descriptors (≈ 0.3 KB).

Item kinds (coordinates are s16 in canvas pixels, clipped to the canvas):

| Kind | Fields | Draws |
|---|---|---|
| 1 `DOT` | x y | one pixel |
| 2 `LINE` | x0 y0 x1 y1 | Bresenham line, endpoints included |
| 3 `RECT` | x y w h | 1 px outline |
| 4 `BAR` | x y w h | filled rectangle |
| 5 `OVAL` | x y w h | ellipse outline inscribed in the box |
| 6 `DISC` | x y w h | filled ellipse |
| 7 `TEXT` | x y n ch·n (n ≤ 20) | 8×8 font at any pixel, transparent background |
| 8 `ICON` | x y bitmap | a bitmap, its transparent index skipped |
| 9 `STIPPLE` | x y w h colour2 | filled rectangle in a 2-colour checker (the 16-colour way to shade) |

Each item also has a colour (palette index). Text with a background is a
`BAR` under a `TEXT`.

## 3. Rendering: a backing raster, rebuilt from the list

Each canvas keeps a **4 bpp backing raster** in PSRAM, the cache of its
display list. The list is the truth; the raster can always be rebuilt.

- **Edit:** an item change marks a *canvas dirty rect*: the union of the
  item's old and new bounding boxes. No drawing happens inside the `E4` op, so
  every op stays O(1) and the VDP loop-gap rule (§5.5) holds.
- **Rasterise (lazy, bounded):** at the start of each frame, before
  composition, each canvas with a dirty rect is redrawn there: fill with the
  background, then replay every item whose box meets it, clipped to it. A
  batch of 50 edits therefore costs one replay. Work is cut into row bands
  under the **same** per-frame budget as composition (the existing
  `budget_us`, about 3–4 ms): rasterising runs first and what it uses is not
  available to compose that frame. A dirty rect that does not finish carries
  over, like composition damage.
- **Damage:** only the rows rasterised this frame are damaged on screen, in
  every visible window showing the canvas's surface. The screen never shows a
  half-replayed band.
- **Compose:** `client_row()` draws the cells of a scanline as today, then
  copies the row of each visible canvas on that surface over it.
  - Opaque canvas in a snapped window: a byte copy into the line buffer,
    about the cost of the glyph rows it covers (one load and store per 8
    pixels, against a font load, table lookup and store).
  - Transparent canvas: a per-pixel key compare, roughly the sprite path.
  - Free-x window (odd client x): per-pixel nibble placement, the slower
    path G0 measured at 2.2× for glyphs.
  The acceptance check measures all three; only the first is expected in
  ordinary windows.

Why not replay the list on every composed scanline (no backing)? Moving a
window over a 200-item canvas would replay 200 items per scanline for every
exposed row, every frame; the frame budget is 3–4 ms. The raster makes a
canvas cost exactly what text costs, and PSRAM is plentiful.

**Off-screen presenter:** unchanged. It redirects only `v->rows`; canvases
draw into the line buffer like cells. Rasterising writes backing rasters only,
never framebuffer rows.

**Scroll blit:** `blit_scroll()` copies framebuffer rows when a surface's
*cells* scroll (`vds_scroll`, `vds_scroll_rect`). A canvas keeps its surface
position when the cells under it scroll, so copying its pixels would move it
on screen. A cell scroll whose region meets a visible canvas therefore falls
back to recomposing, as it already does under a sprite or a window. (A
window's scroll *offset* moves cells and canvases together, so it is exact.)

**Style switch:** canvases use palette indices, so a style switch recolours
indices 4–15 like everything else. Content meant to look the same in both
styles uses 0–3 (as for cells).

## 4. Wire protocol (inside `E4` batches)

New ops `C0`–`CC`. To keep the CPU encoder trivial, every coordinate and size
is s16 little-endian; ids and colours are u8.

| Op | Bytes | Meaning |
|---|---|---|
| `C0` | id surface x y w h bg flags | create or replace a canvas (drops its items). flags: 1 visible, 2 transparent (bg is the key) |
| `C1` | id x y | move a canvas on its surface |
| `C2` | id flags | canvas flags (show, hide, transparent) |
| `C3` | id | delete a canvas and its items |
| `C4` | id item kind colour fields… | set an item (fields per kind, §2) |
| `C5` | id item | delete an item; item 255 deletes all |
| `C6` | id item dx dy | move an item by (dx, dy) |
| `C7` | id item colour | recolour an item |
| `C8` | bitmap w h transparent | define a bitmap (pixels from the pool) |
| `C9` | bitmap offset(u16) n byte·n | bitmap pixel data, as `77` for sprites |
| `CA` | id x y | reply `CA` id item x y: topmost item whose box holds the point (255 none) |
| `CB` | id | reply `CB` id flags surface x y w h items backing-bytes |
| `CC` | id item | reply `CC` id item kind colour fields… (inspection, tests) |

Tweens: op `70` gains **target 2, canvas** (prop 0 x, 1 y): the canvas slides
over its surface without the CPU sending per-frame moves.

Pointer: a press inside a visible canvas also queues a **`CANVAS` event
(0x13)**: id = canvas, args x y (canvas pixels) and the hit item. The normal
`POINTER` event still comes too.

Reset (`34`) drops all canvases and the uploaded bitmaps.

## 5. CPU side (lean)

The CPU keeps **no display list**: only a 2-byte *pen* (current canvas and
colour). Native-word slots are scarce (`NATIVES_MAX` 120, 111–117 in use), so
the shapes share one native:

| Word | Stack | Op |
|---|---|---|
| `UI-CANVAS` | ( canvas surface x y w h bg flags -- ) | `C0` |
| `UI-CANVAS-AT` | ( canvas x y -- ) | `C1` |
| `UI-CANVAS-FLAGS` | ( canvas flags -- ) ; flags −1 deletes | `C2`/`C3` |
| `UI-PEN` | ( canvas colour -- ) | CPU state only |
| `UI-SHAPE` | ( item a b c d kind -- ) | `C4` with the pen |
| `UI-PRINT` | ( addr n item x y -- ) | `C4` kind 7 |
| `UI-ICON` | ( item x y bitmap -- ) | `C4` kind 8 |
| `UI-ERASE` | ( item -- ) ; 255 wipes the canvas | `C5` |
| `UI-NUDGE` | ( item dx dy -- ) | `C6` |
| `UI-INK` | ( item colour -- ) | `C7` |
| `UI-BITMAP` | ( bitmap w h transparent -- ) | `C8` |
| `UI-BITS` | ( addr n bitmap offset -- ) | `C9` |

Forth colon definitions on top (no native slots): `UI-DOT ( item x y -- )`,
`UI-LINE`, `UI-RECT`, `UI-BAR`, `UI-OVAL`, `UI-DISC` ( item a b c d -- ),
`UI-STIPPLE` ( item x y w h colour2 -- ) and `UI-WIPE ( -- )`, plus
constants `UI-EV-CANVAS`, `UI-TWEEN-CANVAS`, `UI-CV-VISIBLE`, `UI-CV-CLEAR`
and the built-in icon ids. `NATIVES_MAX` goes 120 → 132 (+72 B BSS).
*Estimate:* +1.2 KB eZ80 code.

Example: a bar chart that updates one bar.

```forth
2 40 18 48 UI-SURFACE                           \ surface 2, 40x18 cells
3 2 0 0 320 144 3 UI-CV-VISIBLE UI-CANVAS        \ canvas 3 fills it, white
3 0 UI-PEN  0 10 130 300 130 UI-LINE             \ item 0: axis, black
3 4 UI-PEN  1 20 60 30 70 UI-BAR                 \ item 1: a bar, colour 4
S" sales" 2 20 132 UI-PRINT                      \ item 2: caption
1 0 -20 UI-NUDGE                                 \ later: move bar 1 up 20 px
```

## 6. Icons

- **Built in (flash):** eight 16×16 icons at bitmap ids 24–31: document,
  folder, word, module, info, warning, error, disk. About 1 KB of flash.
  The GUI tools can use them in lists without uploading anything.
- **Uploaded:** ids 0–23 via `C8`/`C9` in ≤ 200-byte chunks inside normal
  batches, exactly like sprite pixels (§9 says why not `A0` buffers).
- An *icon in a window* is a small canvas with one `ICON` item, opaque or
  transparent over the surface's cells. Icons inside list and tree rows (which
  scroll their cells) are a widget feature for later, not part of G5.

## 7. Tests and acceptance

Host (native, ASan/UBSan, `tests/test_esp_vds.py`):

- Rasteriser goldens per kind, including clipping at all four edges, negative
  coordinates, zero and one-pixel sizes.
- **Oracle** (as for the compositor): after any random sequence of canvas and
  item ops, window moves, scrolls and budget-limited frames, the framebuffer
  equals a fresh full render, and every backing equals a fresh replay of its
  list.
- Protocol: op versus API equivalence, rejection of bad ids and sizes, the
  backing cap, fuzzed batches; `vios_ui_host.c` end to end from the Forth
  words.

Board (`tools/vios_gui_canvas_accept.py`, JSON under `docs/bring-up/`):

- Draw a 320×200 canvas with 200 items; pixel-sample it with `esp_gui GP`
  against the host render of the same list.
- Edit one item 100 times; report bytes sent (target: under 16 B per edit) and
  frame times (no frame over budget, compositor ≤ 4 ms).
- Drag a window over the canvas (B10 variant): 0 torn frames, same frame
  times as over text.
- `tools/vios_gui_accept.py`, `tools/vios_gui_tools_accept.py` and
  `tools/vios_board_suite.py` (9/9) still pass.

## 8. Build order

1. `vds_canvas.{h,c}`: rasteriser and list store, host goldens.
2. Compositor hooks in `vds.c` (`client_row`, damage, blit fallback, frame
   prelude) and the oracle test.
3. `vds_proto` ops `C0`–`CC`, the canvas tween, the `CANVAS` event.
4. eZ80 words in `kernel/vios_ui.c`, `vios_ui_host.c` end to end.
5. Adapter: PSRAM allocation; firmware build; board acceptance.

## 9. Differences from the earlier drafts

- **Raster cache instead of replaying fabgl primitives.** The toolkit draft
  replayed fabgl primitives through `canvas->…` and `waitCompletion`. The G1
  compositor no longer uses fabgl drawing at all (§5.5 loop-gap rule), so the
  rasteriser is in the portable library and draws 4 bpp itself.
- **Bitmaps through `E4` chunks, not `A0` buffers.** Under the GUI the VDP's
  buffer store is a separate allocator with its own ids, and the sprite path
  already proved chunked `E4` uploads. One mechanism, one reset, one
  inspection path.
- **Item ids instead of an append-only list.** "Send once, then edit" needs
  addressable items; an append-only list would need offsets.
- **Backing is mandatory, not optional.** Windows design §5.1 made the pixel
  backing an option; here every canvas has one (§3 says why).
- **Size:** "≤ 2 KB per canvas" becomes ≤ 8 KB of items plus the raster,
  since PSRAM is not the constraint; the CPU side stays at 2 bytes of state.

## 10. Questions for the owner

1. Backing raster per canvas (fast composition, ~150 KB PSRAM for a
   full-screen canvas) rather than per-scanline replay (no memory, slower
   moves). Recommended: raster.
2. Canvases live on **surfaces** (show in every window of that surface, move
   with the window's scroll offset) rather than on windows. Recommended:
   surfaces.
   - 2b. When the surface's *cells* scroll (`vds_scroll`, console style), a
     canvas stays put (recommended: simple, exact; the scroll blit falls back
     to recomposing under it), or a canvas wholly inside the scrolled region
     moves with the text (keeps the blit exact, but a canvas can scroll off
     the surface and needs a rule for that).
3. Built-in icon set: the eight above, or none in G5.
4. Bitmap upload through `E4` chunks instead of `A0` buffers.

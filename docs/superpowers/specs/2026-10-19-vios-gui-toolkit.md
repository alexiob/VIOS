# VIOS GUI toolkit and environment: design

Status: **design proposal, 2026-10-19.** Research and design only. Nothing was built, flashed, or run on the board while preparing it. Numbers labelled *measured* come from the Oct 16 and Oct 19 evidence in this repo. Anything labelled *estimate* or *target* is unverified until Stage 0/1 measure it. Save as `docs/superpowers/specs/2026-10-19-vios-gui-toolkit.md`.

Related: [text workbench](2026-10-16-vios-text-workbench.md), [console](2026-10-19-vios-console.md), [tasks](2026-10-18-vios-tasks.md), [handles](2026-10-18-vios-handles.md), [stage 6/7](2026-10-19-vios-stage6-7-proposals.md), [interaction research](../../research/vios-interaction-environment.md), [fast-display benchmark](../../benchmarks/fast-display-2026-10-16.md).

---

## 1. Summary

VIOS today draws everything on the eZ80. It composes an 80×60 desired/painted cell model in `drivers/agon-vdp/text_screen.c` and streams VDU glyph runs (`17,c` colour, `31,x,y` tab, glyph bytes) over UART2 to an unmodified VDP 1.04 text path. Each glyph then goes through `plotCharacter` → `canvas->drawChar` → the fabgl primitive queue. The console view keeps a second copy of the console in eZ80 RAM (`con_shadow`), and every console byte crosses the UART twice. The cooked line editor, the cursor blink, and console↔editor switching (a full mode reset plus repaint) all run on the CPU.

The proposal is a **retained display server on the ESP32 (VDS, "VIOS Display Server")**, plus a small **Forth toolkit (`UI` vocabulary)** on the eZ80:

- **ESP (VDS) holds the presentation state:** cell surfaces (text grids) in PSRAM, a widget tree, layout, damage tracking, and a direct 2-bpp glyph blitter. It also runs the local behaviours that must not wait on the CPU, which this doc calls *intrinsics*: cursor blink, line editing, scrolling retained text, focus moves, hit testing, and button press feedback. It sends compact **semantic events** to the CPU.
- **CPU (eZ80 Forth) keeps the model and behaviour:** documents, module sources, widget records, and handler words. All authority stays here, and every widget is created by a Forth word with source. The ESP is a cache that the CPU can always rebuild by re-running constructors or repainting.
- **Wire:** one new VDU command, `VDU 23,0,&E4,len,ops…` (a bounded batch of UI ops), plus two new ESP→CPU packet codes (`&F0` events, `&F1` replies). Console text that already reaches the ESP via `E0` is also rendered there, so it costs no extra bytes. Bulk data (documents, icons) uses the existing VDP buffered-command API (`23,0,&A0`).
- **Stage 1 is the visible speedup of the existing console and editor.** It takes the USB drain off the VDP parser, adds ESP-resident surfaces with a fast blitter, and moves the console terminal onto the ESP. The workbench model, the `WB-PAINT` observer and all Forth semantics stay as they are. Later stages add the ESP line editor (keystroke-to-glyph independent of CPU load), widgets, events, `/dev/win`, inspection halos and graphics.

---

## 2. Where the time goes today (grounded diagnosis)

| # | Finding | Evidence in code / data | Consequence |
|---|---|---|---|
| D1 | **The wire is not saturated.** A full repaint sends 2255 B in 177.5 ms of native output (≈12.7 KB/s), about 1/9 of UART2 capacity at 1,152,000 baud (≈115 KB/s). | `docs/benchmarks/fast-display-2026-10-16.md`. `send_bytes()` in `boards/agon-light-2/platform_resident.c` runs `poll()` around every ≤32-byte `ez80_uart0_tx_burst`. The idle painter is capped at `VIOS_TEXT_BATCH_BYTES`=128 per idle. On the ESP, each glyph byte is one `processNext()` → `plotCharacter()` → `canvas->drawChar()`, which queues a fabgl primitive. | Per-byte overhead dominates on both ends. Fewer, denser commands and an ESP blitter matter more than baud rate. Stage 0 must split the CPU and ESP shares. |
| D2 | **The ESP VDP parser blocks on the USB drain.** `VIOS_VDU_CONSOLE` (`E0`) is handled inside the VDP task (`vdu_sys.h` `vdu_sys_video`). It calls `vios_usb_console_write` → `usb_send`, which holds `usb_tx_mutex`, then runs `DBGSerial.flush()` (115200 baud, ≈87 µs/byte) and `delay(1)`. | `firmware/esp32-vios/include/vios_bridge.h` `usb_send`; pacing introduced for open issue #2. | Glyph VDU queued behind an `E0` frame waits for the physical USB drain: roughly 12 ms per 128-byte console record plus one tick. That puts console echo and typing latency directly in series with USB. (Hypothesis to confirm in Stage 0.) |
| D3 | **Console bytes cross UART2 twice.** `platform_console_putc` buffers each byte for `E0` (USB) and also taps it into `con_ring`. At REPL idle `con_drain`→`con_draw`→`vios_text_put` turns it into a 1-cell paint, which then goes out again as VDU positioning and glyphs. | `kernel/workbench_runtime.c` `vios_console_putc`, `con_draw`, `console_idle`. | Console throughput on VGA is at best half the link, minus positioning overhead. It also costs 4640 B (`con_shadow`) + 512 B (`con_ring`) of CPU RAM. |
| D4 | **The keystroke path is long and depends on the CPU.** PS/2 → ESP `do_keyboard` → `0x81` packet → waits for an eZ80 RTS window, which only opens at idle/getc polls → `transport.c local()` → `echo_local()`, which redraws the tail of the line with spaces/BS (O(line) bytes) → tap ring → `console_idle` → paint → VDU → ESP parse → glyph. The echo also produces an `E0` USB frame, which hits D2. | `drivers/agon-vdp/transport.c`, `platform_resident.c echo_local`, `ECHO_IDLE_POLLS`. | Latency grows with CPU work: compilation, `TASK-SLICE` budgets, transactions, observer painting. The background-task gate already shows the coupling. |
| D5 | **View switching rebuilds everything.** F1 runs `display_reset()` (`VDU 22,1` mode change + `0x86` query), then a full repaint from the CPU shadow. The redraw is deferred behind a fence because cells sent right after CLS were lost. | `workbench_runtime.c display_reset`, `con_redraw_now`. Console acceptance: ≈770 ms per editor↔console round trip, including two mode resets (measured Oct 19). | Hundreds of ms for something that should be one op. |
| D6 | **CPU composition is a real cost.** Full compose 125.7 ms, character edit 7.85 ms, newline 12.5 ms, scroll 6.0 ms (measured Oct 16). | `kernel/workbench.c` composer. | Key-to-glyph in the editor is ≥ compose + paint. Only moving presentation work to the ESP, or predicting locally, removes it from the latency path. |
| D7 | **Cursor blink costs the CPU.** Each blink phase is 2 cell repaints through the CPU idle loop, and the cursor freezes while the CPU is busy. | `con_show_cursor`/`con_hide_cursor`, `CON_BLINK_TICKS`. | Bytes and CPU work for something the ESP can do for free. |
| D8 | **Readback is pixel reconstruction.** `CHAR@` (`23,0,&83`) matches pixels against the *current* colours, which caused false "blank screen" diagnoses. | Console spec, "Constraints learned on hardware". | An ESP-resident grid gives exact, colour-independent readback. |

Composition (D6) is a CPU algorithm problem. D1–D5 and D7–D8 come from where things are placed. This design addresses placement first (Stage 1–2), then removes composition from the latency path (Stage 2b/3).

---

## 3. Goals, non-goals, constraints

**Goals**
1. A visibly faster console and editor in Stage 1, without changing workbench or Forth semantics.
2. Keystroke-to-glyph latency that is bounded and independent of CPU load for local editing.
3. Minimal UART2 traffic: content goes over the wire once, and repaint, scroll, view switch and blink cost O(1) bytes.
4. A retained widget model whose rendering lives on the ESP and whose behaviour lives in Forth with source.
5. Malleability: every widget can be inspected, rebound and edited live from Forth, the CLI and MCP, through one command vocabulary.
6. Integration with tasks and handles: `/dev/win/...` endpoints and event streams readable by tasks.
7. Measurable acceptance: frame time, keystroke-to-glyph latency, bytes per update, RAM high water.

**Non-goals (for now):** a Smalltalk-style object VM; message dispatch on ordinary Forth calls; heap images; Unicode or proportional fonts; overlapping windows before tiling works; a required pointer device.

**Hard constraints inherited from the handoff and code**
- CPU RAM headroom is ≈37 KB (BSS end `0xA5E48`). **Stage 1 must be RAM-neutral or better.** Later stages pay for themselves by reclaiming named buffers (§10).
- **No new USB frames or USB traffic classes** (open issue #2). Everything new travels on UART2 only.
- **Recovery independence.** The USB/ZDI control task (`vios-usb`, core 0) must stay reachable. No VDS op may loop unboundedly. VDS never touches GPIO26/27.
- **`WB-PAINT` semantics:** only the exact default native painter gets the fast path. Replaced hooks still run under `forth_observe` with a 4096-instruction budget. Rendering must not steal `KEY`, change DS/BASE, or run with RX enabled (the console-shadow overrun lesson).
- **Physical-key isolation from host lines** (cooked mode) must be preserved.
- **Vendor discipline:** VDP 1.04 / vdp-gl stay pinned. VIOS hooks follow the existing `VIOS_USB_MUX` / `VIOS_VDU_*` pattern with a minimal vendor diff, and new sources are recorded in `build-manifest.json`.
- **Mouse hardware does not exist.** `enableMouse()` is stubbed because GPIO26/27 carry ZDI. Everything must be keyboard-complete.
- **The framebuffer is internal RAM.** `VGAPalettedController::allocateViewPort` uses `MALLOC_CAP_INTERNAL`. Mode 1 is 75 KB; double-buffered mode 129 is 150 KB internal. PSRAM is for surfaces and assets only.

---

## 4. Architecture

```text
 eZ80 (authority, behaviour)                         ESP32 (presentation, intrinsics)
 ┌────────────────────────────────────┐              ┌──────────────────────────────────────────┐
 │ Forth: UI vocabulary, apps, handlers│              │ VDP task (core 1, Arduino loop)          │
 │  widget records (id,kind,parent,    │  UART2       │  VDU parser ─┬─ legacy VDU (unchanged)    │
 │  props, handler xts, source origin) │ 1.152 Mbaud  │              ├─ E0 console ─► USB queue ──┼─► control task (core 0) ─► USB
 │ workbench model / console REPL      │ RTS/CTS      │              │            └► Terminal surface
 │ text_screen (stage1: E4 encoder)    │ ───────────► │              └─ E4 UI batch ─► VDS        │
 │ event ring ◄── 0xF0 events          │ ◄─────────── │  VDS: surfaces (PSRAM), widget tree,      │
 │ reply slot ◄── 0xF1 replies         │              │       layout, damage, 2bpp blitter,       │
 │ key ring   ◄── 0x81 keys (forwarded)│              │       cursor blink, LineField, focus,     │
 │ /dev/win/* (vios_handles.c)         │              │       hit test, event coalescing          │
 └────────────────────────────────────┘              │  PS/2 keyboard ─► VDS key router          │
                                                     │ fabgl VGA4 framebuffer (internal RAM)     │
                                                     └──────────────────────────────────────────┘
```

### Authority table

| State | Authoritative | Copy | Notes |
|---|---|---|---|
| Module sources, workbench draft, revision | CPU | ESP TextView mirror (Stage 3) | Mirror edits are proposals that the CPU confirms (revisioned). |
| Widget identity, kind, parent, handler xts, constructor source | CPU | ESP node (no xts) | `UI-SNAPSHOT` writes Forth source that recreates the tree. |
| Widget visual props (rect, style, text, items) | CPU (set) | ESP (render) | Pushed on change only. |
| Intrinsic state (scroll offset, field cursor, list selection, focus, blink phase) | ESP | CPU, on event or pull | Semantic changes are pushed as events. Inspection pulls with `GET`. |
| Console transcript | ESP (Terminal surface) | none on CPU after Stage 1c | Harness/USB still receives the `E0` stream. |
| Cooked console line being edited | ESP (Stage 2) | — | Today it lives in `transport.c`, and the CPU never sees it until Enter either. |
| Pixels | ESP framebuffer | — | Never streamed. |

**Recovery rule.** If the ESP restarts or its VDS epoch changes (reported by `SYNC`), the CPU replays: `UI-REBUILD` re-runs constructors and the editor/console `WB-REPAINT`. The console transcript held only on the ESP is lost on an ESP restart; the USB log and harness keep it. Stage 1c documents this as a deliberate trade, in exchange for the 5 KB reclaimed.

### ESP placement

- VDS runs **inside the VDP task**, called from the `loop()` iteration next to `vios_bridge_service_vdp()`. It therefore never runs concurrently with the VDU parser, and UART2 TX (events/replies) stays serialized by the existing `vios_vdp_tx_begin/end`.
- The render step is **bounded per loop iteration** (target ≤2 ms of blitting, then return to parse), so a large repaint cannot starve UART2 RX. RX is hardware flow controlled, so the CPU waits rather than losing bytes.
- The **USB console drain moves to the control task** (Stage 1a). The `E0` handler only enqueues to the existing `console_queue`-style record queue; the `vios-usb` task on core 0 sends frames with the existing pacing (flush + tick). Pacing semantics for USB are unchanged; the parser just stops waiting on them.

### Code organization (proposed)

- `firmware/esp32-vios/lib/vios-vds/` holds portable C++ with no fabgl or Arduino dependency: op parser, surfaces, tree, layout, damage, LineField, event encoder. It is host-testable like `tests/bridge.cpp`/`runtime.h`, rendering to a fake 2-bpp framebuffer with golden images.
- `firmware/esp32-vios/include/vios_vds_fabgl.h` is a thin adapter: framebuffer row pointers (`getScanline`), `canvas->waitCompletion()`, palette, sprites, `send_packet`.
- Vendor touch points: `vdu_sys.h` (`case VIOS_VDU_UI:`), `video.ino loop()` (`vios_vds_service()`), `do_keyboard()` (route through the VDS key router when VDS owns input). In probe builds only, `plotCharacter` gets a latency probe hook for baselines.
- CPU: `drivers/agon-vdp/vds.c` (op encoder and batcher), `include/vios/vds.h`, changes to `text_screen.c` (encode `E4`), `transport.c` (packet `F0` ring; Stage 2 removes `local()`/history), and later `kernel/vios_ui.c` (widget table, dispatcher, Forth natives) and `/dev/win` in `kernel/vios_handles.c`.

---

## 5. Wire protocol

### 5.1 Framing

**CPU → ESP: `VDU 23,0,&E4,len,payload[len]`**, len 1..255. It follows the `E0`/`E3` style already in `vdu_sys.h`.
- The payload is a sequence of ops. The ESP **reads the whole payload, validates every op, and only then applies it**. A malformed batch is dropped whole: the error counter increments, a `desync` flag is set, and the next `SYNC` reports it. A partial batch is never applied and no partial VDU state is left in the parser (the same property the fast-display renderer guards today).
- CPU batches are capped at 128 bytes by default, matching today's idle-painter fairness. 255 is the hard maximum.
- `E4` is processed in-stream, so it is ordered with legacy VDU. The `23,0,&86` mode query remains a valid fence.

**ESP → CPU:** existing `send_packet(code)` puts `code|0x80` on the wire. Codes in use are `0x00–0x09` (VDP standard), `0x7C/0x7D/0x7E` (host reply FC, clock FD, host line FE). New codes:
- `0x70` → **`0xF0` UI event**, ≤30 data bytes. It must go into a **dedicated event ring** in `struct vdp_transport`, like `keys[32]`, because `replies[8]` is keyed by type and overwrites earlier packets of the same type.
- `0x71` → **`0xF1` UI reply**, ≤32 data bytes (fits `vdp_reply.data[32]`). One request is in flight at a time via `platform_vdp_request`.
- **Forwarded keys keep using `0x81`**, so `platform_keyboard_event()` and `physical_key()` stay unchanged.

**Bulk data** (document text, icons, fonts) uses the existing `VDU 23,0,&A0,bufferId;…` WRITE into a reserved VIOS buffer range (proposed `0xFB00–0xFBFF`; `0xFA00/0xFA01` scroll scripts are retired in Stage 1b). A `TLOAD`/`BLIT` op then references the buffer. VDS ops never *call* buffered programs, so a UI op cannot loop.

### 5.2 Ops (draft encoding; u8 unless noted, u16 little endian)

Coordinates are cells (8×8) unless an op says pixels. The `attr` byte keeps today's `vios_text_cell.attr` meaning (`fg | bg<<4`, palette indices), so `text_screen.c` maps 1:1.

**Surfaces and grid (Stage 1)**

| Op | Bytes | Meaning |
|---|---|---|
| `01 SURF id cols rows flags` | 5 | Create or resize surface `id` (1..15). flags: b0 terminal, b1 opaque, b2 transparent-space. |
| `02 PLACE id col row z` | 5 | Position on screen and z-order. |
| `03 SHOW id on` | 3 | Show/hide (view switch = one `SHOW` pair, no mode reset). |
| `10 PUT id col row attr n bytes…` | 6+n | Write a single-style run. |
| `11 MORE attr n bytes…` | 3+n | Continue at the end of the previous PUT on the same surface; replaces a 3-byte TAB. |
| `12 PUTA id col row n (ch,attr)…` | 5+2n | Mixed-style run. |
| `13 FILL id col row w h ch attr` | 8 | Clear or fill a rectangle. |
| `14 SCROLL id col row w h dy` | 7 | Move cells by dy (s8). Vacated cells use the surface's clear attr. ESP uses `copyRect` on the framebuffer when the region is fully visible, otherwise re-blits. |
| `15 CURSOR id col row shape blink` | 6 | Shape 0 off/1 block/2 underline/3 bar. Blinking is done locally on the ESP. |
| `16 PAL idx r g b` | 5 | Palette entry (optional; mode 1 keeps black/red/yellow/white). |
| `30 COMMIT` | 1 | End of frame: render the accumulated damage now. Without a COMMIT, damage renders after ≤33 ms so a stalled client cannot hide output. |
| `31 SYNC seq` | 2 | Reply `F1: 31 seq epoch(u16) gen(u16) errors desync heap_kb(u16)`. |
| `32 GET id col row n` | 5 | Reply `F1: 32 n (ch,attr)×n`, n ≤ 14. Exact readback that replaces `CHAR@` for diagnostics. |
| `33 STATS sel` | 2 | Reply with counters (§11.1). |
| `34 RESET` | 1 | Drop all surfaces and widgets. Epoch++. |
| `35 RAW on` | 2 | Legacy VDU owns the screen (VDS stops blitting). Off = mark everything damaged and re-blit. |

**Console and line editing (Stage 1c / 2)**

| Op | Bytes | Meaning |
|---|---|---|
| `20 TERM id n bytes…` | 3+n | Feed terminal bytes (CR, LF, BS, printable) to a terminal surface. |
| `21 CONSOLE id flags` | 3 | Bind the `E0` console stream to terminal surface `id`. b0 enable, b1 hide harness envelopes (port of `con_byte` state machine), b2 status row. |
| `22 LINEMODE on` | 2 | Stage 2: the ESP LineField owns physical printable/edit keys while the REPL awaits a line. Replaces `vdp_transport_line_mode`. |
| `23 HOTKEY vk action` | 3 | Local hotkeys, e.g. F1 → swap visible surfaces locally, then event. |

**Widgets (Stage 3+)**

| Op | Meaning |
|---|---|
| `40 NODE id(u8) kind parent` | Create a node. Kinds in §7. Up to 64 nodes initially. |
| `41 PROP id key len value…` | Set a property: rect, style, text, flags, value, item, layout params, handler mask. |
| `42 DEL id` | Delete a subtree. |
| `43 FOCUS id` | Set focus. |
| `44 TINS id off(u16) n bytes…` / `45 TDEL id off(u16) n(u16)` / `46 TLOAD id buf(u16)` | TextView document ops (authoritative edits from the CPU). |
| `47 ITEMS id first n (len,bytes)…` | List rows, paged. |
| `48 GETP id key` | Pull a property (inspection) → `F1`. |
| `49 PREDICT id on` | Enable speculative local edits on a TextView/editor surface (Stage 2b/3). |

### 5.3 Events (ESP → CPU, `0xF0`)

`F0 len: seq kind id args…`

| kind | args | Source |
|---|---|---|
| `01 KEY` | ascii mods vk | Unhandled key for widget `id`. Legacy routing uses `0x81` instead. |
| `02 ACTIVATE` | — | Button/menu/list Enter or click. |
| `03 SELECT` | index(u16) | List/tree selection changed. |
| `04 CHANGE` | rev(u16) off(u16) del(u8) n bytes ≤20 | Local edit in a TextView/LineField. The CPU applies it with WB-PATCH semantics (revision checked) or rejects it with a corrective `TINS/TDEL`. |
| `05 SCROLL` | top(u16) | Retained view scrolled. |
| `06 FOCUS` | from to | Focus change. |
| `07 POINTER` | x(u16) y(u16) buttons | Coalesced, latest wins. At most one pending. |
| `08 VIEW` | surface | Local hotkey switched views. |
| `09 INSPECT` | id | Halo/inspect gesture on widget `id`. |
| `0E OVERFLOW` | dropped | Event queue overflowed; the CPU should resync. |
| `0F ERROR` | code op_offset | Batch rejected. |

The ESP keeps a bounded outbound queue of 32 events. Pointer and scroll events coalesce; CHANGE and ACTIVATE events do not. Overflow produces `OVERFLOW`, and the CPU responds with `UI-RESYNC` (pull state, repaint).

Events go out through the same UART2 TX path as keys and obey eZ80 RTS. The **CPU drains the F0 ring only with RX paused**, at the same point where `vios_workbench_idle` polls keys.

### 5.4 Byte budgets (encoding arithmetic; Stage 0/1 measure the real values)

| Update | Today (measured/derived) | VDS encoding | Bytes |
|---|---|---|---|
| Console: one typed char at line end | `echo_local` tail + `E0` frame (≥5 B) + tap → paint (TAB 3 + colour 0–4 + glyph) | Stage 1c: `E0` byte only (the ESP terminal renders it). Stage 2: **0 B** (local LineField) | 0–5 |
| Console: 80-col output line | ≈ 2× (once in `E0`, once as VDU) + positioning | `E0` only | ≈84 |
| Console scroll | `vios_text_scroll` cached script 6 B warm / 39 B cold | Done by the ESP terminal on LF | 0 |
| Cursor blink phase | 2 cell repaints via CPU | ESP local | 0 |
| Editor char insert (36 B today) | colour/TAB/glyph runs | `PUT` (6+n) + `COMMIT` | ≈20–30 |
| Editor source scroll (57 B median today) | buffered script + exposed row | `SCROLL` 7 + exposed row `PUT` ≈86 for full width, or partial | 7 + row |
| Console ↔ editor switch (≈385 ms one way today) | mode reset + full repaint | `SHOW a 0 SHOW b 1 COMMIT` | 11 |
| Full editor repaint already retained on ESP | 2255 B | `35 RAW 0` / damage-all | 2 |
| Full editor repaint, content changed | 2255 B | ≈ same content bytes, fewer prefixes | ≈1.6–2.0 KB |

---

## 6. ESP renderer and compositor

**Surfaces.** A `cols×rows` array of `(ch, attr)` in PSRAM (2 B/cell; 9600 B for 80×60) plus per-row dirty spans `[lo,hi)`. Each surface has a screen position, z and visibility. The visible screen is a list of placed surfaces, initially non-overlapping tiles. Overlap is allowed later; the compositor then resolves occlusion per cell row.

**Damage.** Each op marks per-row spans. At `COMMIT` (or the 33 ms deadline), the compositor walks rows top to bottom, merges spans across surfaces into screen spans, and blits them. Cell granularity makes an update tear-free within a frame budget without a second framebuffer.

**Blitter.** In mode 1 (VGA4), a row of the viewport is 2 bpp, MSB first: pixel `x` lives at bits `6-2*(x&3)` of `row[x>>2]` (`VGA4_SETPIXELINROW` in `vga4controller.cpp`). An 8-pixel glyph row is exactly 2 bytes, cell-aligned. Keep a per-`(fg,bg)` LUT of 256 entries × u16 (512 B each, built lazily, 4–8 cached ≈ 2–4 KB internal). Blitting one cell is then 8 × {font byte → LUT → 16-bit store}. Estimate: a few hundred cycles per cell; a full 4800-cell screen in single-digit ms at 240 MHz. **Target, to measure in Stage 1b.** The blitter writes into `m_viewPort[y]` (`getScanline`).

**Concurrency with fabgl.** Primitives (legacy VDU drawing, sprites) execute asynchronously in `primitiveExecTask` (pinned to the quiet core) or at vsync. Before a blit batch, VDS calls `canvas->waitCompletion(false)`; after it, nothing more is needed because VDS writes nothing through the queue. Sprites in paletted modes are composited into the framebuffer with saved backgrounds, so blitting under a visible sprite would corrupt that saved background. VDS hides the pointer sprite around blits that intersect it (`hideSprites/refreshSprites`), or draws the pointer itself as a cell/overlay in Stage 5. In `RAW` mode, VDS does not touch the framebuffer at all.

**Cursor and blink.** These are an ESP-timer phase (500 ms) and an inversion at blit time. A cursor move is one op; the blink costs zero bytes and is unaffected by CPU load.

**Double buffering.** Mode 129 (640×480×4, double) needs 150 KB of internal RAM. It is **measure-first**: Stage 0 `STATS` reports `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` and the largest free block after a mode set. If it fits with margin (≥24 KB free after VDS init), VDS can flip at `COMMIT` with a two-frame damage history (buffer age 2: also re-blit the previous frame's damage into the new back buffer). Otherwise single buffering with cell-span damage is the default. It is visually adequate for text and costs no RAM.

**Graphics.** A `Canvas` widget (Stage 5) holds a retained display list (≤ 2 KB in PSRAM per canvas) of fabgl primitives (line, rect, fill, ellipse, text, bitmap). It is replayed clipped to damage through the normal canvas path, after which `waitCompletion` runs. Bitmaps are uploaded once into `0xFBxx` buffers (RGBA2222 to keep bytes down) and referenced by id.

**Fonts.** The current 8×8 CP1252 font stays (copied from `agon_fonts.h` into the LUT path). UDG redefinition gives box-drawing and halo glyphs. An 8×16 font for larger text could be added later as an ESP-only asset with no wire cost.

**Bounded work and determinism.** Per `loop()` iteration, the parser processes available bytes as today. VDS validates and applies at most one `E4` batch, then blits for at most a cell budget (initially 1200 cells ≈ a quarter screen) before returning. Applying an op is O(op size). Layout is O(nodes) with 64 nodes. No op allocates after `SURF`/`NODE` creation; pools are fixed at init.

---

## 7. Widget set

Each kind lists its ESP intrinsics (local, no round trip) and the events it sends to the CPU.

| Kind | Stage | ESP intrinsics | Events to CPU | Maps today's |
|---|---|---|---|---|
| `Surface` (raw grid) | 1 | Render, scroll, cursor, blink | — | `text_screen` planes |
| `Terminal` | 1c | CR/LF/BS, autoscroll, envelope filter, status row, cursor | — | `con_*` console view |
| `LineField` | 2 | Insert/delete, Left/Right/Home/End, Esc clear, history (4×235 B, as today), prompt | Line → **`FE` path** (same queue as host lines, so atomicity holds); `CHANGE` optional | `transport.c local()`, `echo_local` |
| `Label`, `Status` | 3 | Render | — | status bars |
| `Button`, `Check` | 3 | Focus ring, press feedback | `ACTIVATE` | F-key actions |
| `List` (paged, virtual) | 3 | Selection move, scroll within loaded page, type-ahead | `SELECT`, `ACTIVATE`, page request | module/word browser |
| `Tree`/`Outline` | 4 | Expand/collapse visuals | `SELECT`, expand request | inspector, module graph |
| `TextView` | 3 | Retained document mirror, line index, scroll, cursor, selection, **speculative printable edits** | `CHANGE` (rev-checked), `SCROLL` | workbench editor pane |
| `Tile`/`Column`/`Row` | 3 | Layout | `RESIZE` | pane layout, `WB-ZOOM` |
| `Palette` (command menu) | 4 | Filter list locally | `ACTIVATE` | Genera-style command menu |
| `Halo` (overlay) | 4 | Draw handles around target | `INSPECT`, action events | Morphic halo |
| `Canvas` | 5 | Replay display list | `POINTER` (if enabled) | graphics inspectors |
| `Icon`/`Bitmap` | 5 | Blit from buffer | — | — |
| `Pointer` | 5 | Sprite or overlay, hit test | `POINTER`, click→`ACTIVATE` | — |

Per-node ESP state is about 32–48 B internal (rect, kind, parent/child/sibling u8, style, flags, intrinsic state) plus PSRAM content.

---

## 8. Layout model

- **Units:** cells (8×8) for text-first layout, with optional pixel offsets for `Canvas`/`Icon`. Mode 1 gives 80×60 cells.
- **Tiling first (Oberon tracks / rio tiles / acme columns).** A `Column`/`Row` holds children with `{fixed n | weight w, min m}`. Layout is computed **on the ESP**, so zoom, resize or reveal costs a couple of `PROP` bytes instead of a CPU recompose plus repaint. It is deterministic: integer arithmetic, a stable remainder rule (extra cells go to the last weighted child).
- The current workbench layout maps to `Column{ Row{List modules, List words, Tree inspector} fixed 15, TextView weight 1, Status fixed 1 }`. `WB-ZOOM 1` becomes "hide the top row" (one `PROP`).
- **Overlap later (Stage 6):** floating `Window` nodes with z-order, compositor occlusion per cell row, and a window manager written in Forth on top of `PLACE`/`SHOW`.
- Layout results can be pulled (`GETP rect`), so CLI/MCP and inspectors see real geometry.

---

## 9. Event and input model

```text
PS/2 ─► fabgl keyboard ─► VDS key router
          │  local hotkeys (F1 view swap)              → VIEW event
          │  focused widget intrinsic consumes?        → maybe CHANGE/ACTIVATE/SELECT event
          │  LineField (LINEMODE on) → line on Enter   → FE packet (host-line queue)
          └─ otherwise forward                         → 0x81 key packet (unchanged CPU path)
host (harness) ─► control channel `UI-INJECT` ─► same router (testing, LLM pointer)
```

**CPU dispatch.**
1. `vios_workbench_idle` already pauses RX and polls keys. In the same window it now also drains the `F0` ring.
2. For each event, look up the widget record and call its handler: `( id arg -- )` for the event kind.
3. The default native handlers (workbench browser/editor) take the fast path, the same rule as `WB-PAINT-DEFAULT`: exact binding only.
4. Any Forth handler runs under `forth_observe(name, 4096, …)`, so DS, BASE and the compile state are isolated and bounded.
5. A failing handler marks the widget faulted (red frame via `PROP style`), stores `LAST-ERROR`, and never blocks rendering or input.

**Handlers do not run in background tasks**, because natives that re-enter the VM are refused there (`FORTH_NATIVE_TASK` rule in `forth/system.c`). Tasks instead consume events as a **stream**: a widget whose handler slot is empty queues its events to `/dev/win/N/event`, and a task reads them with `READ`/`READ-WAIT`. This gives two app styles: callbacks (Morphic/HyperCard) and event loops (acme/rio). Both are inspectable.

**Priorities.** Physical console input and host control come first, then UI events, then background tasks (`platform_console_quiet` also treats pending F0 events as "not quiet"). The idle painter stays bounded.

**Pointer policy.**
- Keyboard pointer mode (ESP intrinsic, e.g. Ctrl+arrows with Space = click) and host-injected pointer events from the harness are always available.
- A physical PS/2 mouse is an **owner decision gate**: it needs alternate ESP pins that do not conflict with ZDI, a wiring review, and a change to `setupKeyboardAndMouse()`.
- Every widget must be fully keyboard-operable.

**Speculative echo (Stage 2b, mosh-style).**
- When enabled for the editor surface or TextView, the ESP immediately shows printable inserts, BS and Delete within the current line at the predicted cursor, styled as "unconfirmed" (underline attr). It still forwards the key (`0x81`) as today.
- The CPU composes as usual. Its authoritative `PUT`s for that row replace the prediction. On mismatch (a remapped key, a full buffer, a Forth key hook), the prediction disappears within one CPU frame.
- Predictions expire after 500 ms without confirmation.
- The workbench model and its revision semantics are untouched.

---

## 10. Malleability and inspection model

**Widgets are Forth-made.** Each widget has a CPU record of ≈24 B: id, kind, parent, generation, name index, handler slots (xts for ACTIVATE/SELECT/CHANGE/KEY), and origin (module + source-map entry of its constructor). The live tree is the result of running constructor words, so "the source of this widget" is `SEE`-able and editable in the workbench. Saving it uses the existing module stage → activate → save path. Nothing new is persisted as an image.

**Example (proposed vocabulary):**

```forth
: GREET-CLICK ( id arg -- ) 2DROP ." hello from the Agon" CR ;
: GREETER ( -- id )
  ROOT COLUMN DUP >R
  R@ S" VIOS greeter" LABEL DROP
  R@ S" Say hello" BUTTON  ' GREET-CLICK OVER ON-ACTIVATE DROP
  R> ;
GREETER CONSTANT G
G UI.           \ print record: kind, rect, props, handlers (with names), origin module
G INSPECT       \ open inspector tile on it (halo on screen)
UI-TREE         \ whole tree, one line per node
```

Rebinding is explicit and visible. `' NEW-CLICK btn ON-ACTIVATE` stores an xt. Redefining `GREET-CLICK` does **not** retarget the stored xt, which matches VIOS's "no magic retargeting" rule. The inspector shows the bound word's name and generation, and flags a stale binding if a newer word with the same name exists. For live retargeting, handlers can be `DEFER` words (`DEFER GREET-ACT … ' GREET-CLICK IS GREET-ACT`), the existing service-boundary idiom.

**Halo (Morphic) and Inspect.**
- `Ctrl+I`, or Alt+click with a pointer, makes the ESP hit-test the focused or pointed widget and send `INSPECT id`.
- A Forth `INSPECT` word opens an inspector tile: props pulled via `GETP`, handler names, origin, and ESP intrinsic state.
- The halo overlay offers actions as events: edit handler source (opens the workbench on the origin module at the handler's source-map offset), move/resize (writes `PROP rect`), hide, delete, and "copy as source" (emits the constructor call).

**Plan 9 namespace (rio/acme style), via `kernel/vios_handles.c` built-in devices next to `/dev/tasks`, not the 4-slot user `DEVICE` registry:**

| Path | Read | Write |
|---|---|---|
| `/dev/win` | Snapshot list: `id kind parent name rect` | — |
| `/dev/win/new` | — | `kind parent name` → creates; read back gives id |
| `/dev/win/N/ctl` | Props as text | Text commands: `rect 0 0 40 20`, `text Hello`, `show 0`, `focus`, `style 2` |
| `/dev/win/N/text` | Content (label text, list items, TextView doc via CPU authority) | Replace content |
| `/dev/win/N/event` | Event lines: `activate`, `select 3`, `change 12 0 1 'x'` | — |
| `/dev/ui/tree` | `UI-SNAPSHOT` source text | — |

The ctl command language **is the same vocabulary** as the Forth words (`UI-RECT`, `UI-TEXT`, …), and the harness gets matching MCP tools (`ui_tree`, `ui_get`, `ui_set`, `ui_event_inject`). Keyboard, REPL, handles, CLI and LLM therefore operate on the same objects with the same errors, as handoff stage 1 requires ("no second divergent UI model").

**Persistence.** `UI-SNAPSHOT` writes Forth source that reconstructs the current tree (Smalltalk fileOut, not an image). An app is a module whose `AUTORUN` or constructor builds its tiles. A HyperCard-like "card" is just a constructor word. A "stack" is a module.

**Safety.**
- Handlers are bounded observers. Faults are visible per widget.
- ESP intrinsics keep the UI responsive even when the CPU is busy or halted: the cursor blinks, fields accept typing, retained text scrolls.
- ZDI/USB recovery is unchanged. `UI-RESET` (op `34`) plus `WB-REDRAW` is the display recovery path, and `RAW 1` hands the screen back to legacy VDU for user graphics experiments.

---

## 11. Measurement and benchmarks

### 11.1 Instrumentation (Stage 0, no behaviour change)

**ESP is the timing authority.** It owns `esp_timer_get_time()` (µs), sees the key first and draws the glyph. The CPU's 16-bit 288 kHz timer wraps at 227 ms, so it cannot measure end to end.

- **Counters** (`STATS`, `F1` reply; also printed by a control-channel command so the host can read them without CPU involvement):
  - UART2 RX bytes split by class (legacy VDU glyph, VDU control, `E0`, `E4`, `A0`) and TX bytes (keys, events, replies).
  - VDP parser busy µs, and µs blocked inside `vios_usb_console_write` (confirms or refutes D2).
  - Blit cells, blit µs, `COMMIT` count, max frame µs.
  - Event queue high water, dropped events.
  - Free internal heap and largest free block; PSRAM free.
- **Latency probe:** the host arms marker character `c`. On the ESP, t0 = the moment the PS/2 decoder yields `c` (or a control-channel `KINJ` injection at the same boundary, so tests need no human). t1 = the first glyph write of `c`: in `plotCharacter` for the legacy path, in the blitter for VDS. A histogram (min/median/p95/max) is kept for N samples. Scanout (≤16.7 ms) is reported separately.
- **CPU:** the existing `WB-RENDER-STATS`, `WB-COMPOSE`, `WB-FLUSH-FENCED`, plus `UI-STATS` (events dispatched, observer µs via the existing timer, batch counts).

### 11.2 Benchmark suite

Add `tools/vios_ui_bench.py` and extend `tools/vios_fast_display_bench.py` with `--renderer vdu|vds`. Each run records cold and warm medians and p95, with the display-only discipline from the handoff.

| ID | Scenario | Metric |
|---|---|---|
| B1 | Console flood: `WORDS` + 500 lines of `.` output while the console is visible | VGA-visible console bytes/s; UART2 bytes per console byte; drops |
| B2 | Typing: 40 injected chars at 20 and 100 cps into the console line | Keystroke-to-glyph median/p95 (ESP probe) |
| B3 | B2 while two `SPAWN` tasks compute (`TASK-SLICE` 1024) and the host runs `evaluate` traffic | Same, under load; console drop counters |
| B4 | Editor fixture (1969 B, from Oct 16): char edit, newline, scroll, full-width scroll, full repaint | Fenced ms, bytes, CPU compose ms |
| B5 | View switch console↔editor ×20 with 30 output lines each (Oct 19 acceptance shape) | ms per one-way switch; content preserved |
| B6 | Editor typing with speculative echo on/off (Stage 2b) | Keystroke-to-glyph in editor |
| B7 | Widgets: 200-item word list scroll and select; button ACTIVATE → Forth handler → label update | Local intrinsic latency; round-trip latency; bytes |
| B8 | USB integrity: 30 full screen inspections at 1.152M (Oct 16 shape) plus a 30-min mixed soak | Zero corruption; USB loss rate not worse than baseline |
| B9 | Memory: CPU BSS end / gap; ESP internal heap free / largest block after mode set and VDS init | Bytes |

### 11.3 Acceptance targets (targets, not measurements)

| Metric | Baseline (source) | Stage 1 target | Stage 2 target | Stage 3 target |
|---|---|---|---|---|
| Console keystroke-to-glyph, idle CPU (p95, excl. scanout) | measure in Stage 0 (path D4) | ≤ 10 ms | ≤ 3 ms | ≤ 3 ms |
| Same under B3 load | measure in Stage 0 | ≤ 30 ms | ≤ 3 ms (independent of CPU) | ≤ 3 ms |
| Console bytes on UART2 per console byte | ≈2 + positioning (derive in Stage 0) | 1.0 | 1.0 | 1.0 |
| Console flood VGA throughput | measure in Stage 0 | ≥ 3× baseline | — | — |
| View switch, one way | ≈385 ms (Oct 19, ≈770 ms round trip) | ≤ 30 ms | — | ≤ 30 ms |
| Editor full repaint, fenced | 202 ms / 2255 B | ≤ 60 ms (content changed); ≤ 20 ms / ≤ 4 B (retained) | — | — |
| Editor char edit, fenced | 9 ms / 36 B | ≤ 6 ms / ≤ 30 B | display ≤ 3 ms with prediction | ≤ 3 ms |
| Editor source scroll, fenced | 11 ms / 57 B median | ≤ 6 ms / ≤ 7 B + exposed row | — | — |
| Cursor blink traffic | 2 cells per 500 ms via CPU | 0 B | 0 B | 0 B |
| CPU RAM delta (BSS + code) | ≈37 KB headroom | ≤ 0 net (target −4 KB) | −1.2 KB more | −15 KB or better |
| USB console integrity (B8) | Oct 16 pass | pass, no regression | pass | pass |
| Ordinary VM benchmark | `bench/forth_bench.c` | unchanged ±1% | unchanged | unchanged |

If Stage 0 shows the baseline is better than assumed, the targets are restated relative to it before Stage 1 is accepted.

---

## 12. Memory budget

### 12.1 eZ80 (≈37 KB headroom today)

| Item | Now | Stage 1 | Stage 2 | Stage 3 |
|---|---:|---:|---:|---:|
| `con_shadow` 58×80 | 4640 | **−4640** (1c) | | |
| `con_ring` | 512 | **−512** (1c) | | |
| `con_*` code | ~2 KB | −~1.5 KB | | |
| Transport `local[235]` + `history[4][235]` + lengths | ≈1185 | | **−1185** | |
| `text_screen` `painted` plane | 9600 | keep (1b) | optional −9600 once SYNC/GET verify (1b+) | −9600 |
| `text_screen` `desired` plane | 9600 | keep | keep | −9600 (editor → TextView, browser → List) |
| `workbench.c view.editor[38][80]` + `rows/attrs[15][80]` | 3040 + 2400 | keep | keep | −5440 (ESP owns presentation) |
| VDS encoder/batcher code + 128 B batch buffer | — | +≈1.5 KB | | |
| F0 event ring (16 × 32 B) | — | | +512 | |
| Widget table 64 × 24 B + names 512 B | — | | | +2048 |
| `vios_ui.c` code (natives, dispatcher, /dev/win) | — | | | +≈6–8 KB |
| **Net** | | **≈ −5 KB** | **≈ −0.7 KB** | **≈ −15 KB** |

Stage 3 keeps a fallback "legacy grid surface" so custom `WB-PAINT` painters still work. If that fallback needs `desired`, the Stage 3 reclaim drops to ≈ −5 KB. That is a decision point.

### 12.2 ESP32

| Item | Where | Size |
|---|---|---|
| Mode 1 framebuffer (existing) | internal | 75 KB + line buffers |
| Mode 129 second buffer (optional) | internal | +75 KB, measure first |
| VDS core: 64 nodes × 48 B, damage spans, parser buffer 256 B, event queue 32 × 32 B, LUT cache 2–4 KB | internal | ≈ 9–11 KB |
| Surfaces: up to 16 × 9.6 KB (full-screen worst case) | PSRAM | ≤ 154 KB |
| TextView doc mirror + line index (4 KB draft today; room for 16 KB) | PSRAM | ≤ 20 KB |
| Terminal scrollback (optional, e.g. 500 lines) | PSRAM | 40 KB |
| Canvas display lists, icons | PSRAM | budgeted per asset |
| Code | flash | +40–80 KB (app0 705 KB of 1.25 MB) |
| USB console records moved to control task (Stage 1a) | internal | reuse existing `ConsoleRecord` queue shape (8 × 258 B) |

The internal-heap margin is the ESP-side risk. Stage 0 measures it, and VDS init refuses to start (falls back to legacy VDU) if free internal RAM after mode set would drop below a 24 KB floor.

---

## 13. Staged roadmap

Each stage is a reviewable vertical slice: host tests first, firmware and CPU built at matched baud, app0 backed up before flashing (per handoff §8), and acceptance JSON stored under `docs/bring-up/`. Old renderer paths stay selectable until the new path passes, so A/B benchmarks run on one image.

### Stage 0: instrument and baseline (≈1–2 days)
- ESP: counters, latency probe (`plotCharacter` hook in probe builds), `KINJ` key injection at the PS/2 decode boundary via the control channel, heap report.
- CPU: no change except reading `F1` (STATS) for the bench.
- Run B1–B5, B8, B9 on the current pair. Decide D1/D2 shares (CPU send overhead vs ESP per-glyph cost vs USB blocking).
- **Exit:** a baseline JSON plus a short note stating which of D1–D4 dominate. Targets in §11.3 are re-based if needed.

### Stage 1: visible speedup of the existing console and editor
- **1a. Unblock the VDP parser.** In the `E0` handler, enqueue a `ConsoleRecord` to a queue drained by `vios-usb` (core 0), keeping flush + tick pacing there. Queue full → apply backpressure by blocking the parser briefly (bounded wait), never drop silently; count it. Host test in `tests/bridge.cpp`. **Accept:** B8 passes; parser-blocked µs ≈ 0; B2 improves if D2 was significant.
- **1b. VDS surfaces + E4 + blitter.**
  - `text_screen.c vios_text_render` encodes `PUT/MORE/PUTA/FILL/SCROLL/COMMIT` to surface 2 (editor) instead of `17/31`/glyph runs. `render_scroll` is replaced by `SCROLL`, retiring buffers `0xFA00/0xFA01`. The bottom-right-cell guard is no longer needed for VDS and is kept for the legacy path.
  - The portable model, the `WB-PAINT` hook and observer rules are untouched: the default painter still calls `vios_text_render`.
  - `WB-VIEW` stays model-based. A new `WB-VERIFY` compares `GET` readback with `desired`, replacing `CHAR@` checks in acceptance tools.
  - `display_reset()` (mode change) is used only by `WB-REDRAW`. `WB-REPAINT` sends a damage-all instead.
  - A runtime selector `WB-RENDERER 0|1` allows A/B runs.
  - **Accept:** B4/B5 targets; byte-exact `GET` agreement on all fixtures; custom `WB-PAINT` wrapper test (DS 123 / BASE 16, as Oct 16) still passes on DIRECT and TOKEN.
- **1c. Console terminal on the ESP.**
  - `CONSOLE 1 flags` binds `E0` to the terminal surface. The `con_byte` envelope filter is ported to C++, with shared test vectors run against both the CPU and ESP implementations before the CPU copy is deleted.
  - Status row and dropped counter come from ESP counters. Cursor and blink are local.
  - F1 becomes a local hotkey: `VIEW` event; the CPU updates its notion of focus and keyboard routing.
  - `vios_console_putc` tap, `con_ring`, `con_shadow` and `con_*` are removed. `WB-CONSOLE?` reports via `STATS`.
  - **Accept:** B1/B2/B5 targets; envelope hiding identical to the Oct 19 acceptance; −5 KB CPU RAM.
- **Stage 1 exit:** the §11.3 Stage 1 column, measured on the board, with the user's physical check of console typing feel and the F1 switch.

### Stage 2: local line editing (latency independent of the CPU)
- **2a.** ESP `LineField` replaces `transport.c local()`, `recall()`, `echo_local()`. `LINEMODE` replaces `vdp_transport_line_mode`. A completed line enters the **same bounded line queue as host lines**, which preserves physical/host atomicity; it is delivered as `FE` bytes. History (4 × 235) moves to the ESP. The REPL echo still arrives via `E0`. **Accept:** B2/B3 Stage 2 targets; `vios_keyboard_mixed.py` and `vios_console_accept.py` pass; −1.2 KB.
- **2b.** Speculative echo for the editor surface (`PREDICT`), off by default. **Accept:** B6; zero residual mispredictions after confirmation in a scripted remap test.

### Stage 3: widgets, layout, events, `UI` vocabulary, `/dev/win`
- ESP: nodes, layout, List, Label, Button, Status, Tile, TextView mirror, `F0` events.
- CPU: `kernel/vios_ui.c` widget table, dispatcher (observer-bounded), Forth words, `/dev/win/*` built-in devices, harness tools.
- Port the workbench panes: browser → List (paged `ITEMS`), inspector → Tree, editor → TextView. Patches use `TINS/TDEL` from the existing revisioned model, and local edits come back as `CHANGE` with WB-PATCH semantics.
- **Accept:** B7 plus B4 parity or better; reclaim per §12.1; every existing `wb_*` MCP tool unchanged in behaviour.

### Stage 4: inspection and malleability tools
`INSPECT`, halo, `UI-SNAPSHOT`, "copy as source", stale-binding display, command palette, `ui_*` MCP tools.
**Accept:** edit a button handler from the halo → stage → activate → rebind → click, all without leaving the GUI. Repeat headlessly via MCP.

### Stage 5: graphics and pointer
Canvas, icons via `A0` buffers, dependency-graph view, keyboard pointer, host pointer injection.
**Owner decision gate:** physical mouse wiring.

### Stage 6: windows and apps
Overlapping windows managed by a Forth window manager; per-app tasks consuming `/dev/win/N/event`; double buffering if Stage 0 shows the memory fits.

---

## 14. Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| ESP internal heap too small for VDS plus framebuffer and tasks | VDS fails to init or mode change fails | Stage 0 heap measurement; fixed pools; PSRAM for content; refuse-and-fallback to legacy VDU at init; no mode 129 unless measured. |
| Direct framebuffer writes race fabgl primitives or sprites | Corrupted pixels, sprite background damage | `waitCompletion()` before blit batches; hide intersecting sprites; `RAW` mode for legacy drawing; golden-image host tests plus on-board `GET`/pixel spot checks. |
| ESP-side state diverges from CPU (lost batch, ESP restart) | Wrong screen | Batch atomicity; `SYNC` epoch/generation/desync; CPU replay (`UI-REBUILD`/`WB-REPAINT`); UART2 has RTS/CTS and no observed loss (the loss was on USB). Keep `painted` until `GET` verification proves stable. |
| Moving the cooked editor and console to the ESP shifts authority | Behaviour drift, harder headless testing | Shared test vectors; harness `:events` and `ui_get` expose ESP state; console transcript still goes to USB; document the transcript-loss-on-ESP-restart trade. |
| Event bursts overrun the CPU 16-byte UART FIFO | Lost frames (as in the console-shadow incident) | Events only over RTS-gated UART2; CPU drains only with RX paused; ESP coalescing and a 32-event queue with OVERFLOW → resync. |
| Handler code wedges the CPU | UI unresponsive | Observer budget (4096); faulted-widget marking; ESP intrinsics keep the display alive; ZDI/USB recovery unchanged. |
| Vendor drift (VDP 1.04 pin) | Merge pain, unknown regressions | Isolated `lib/vios-vds`, three small hook points, manifest entries, keep the legacy path selectable. |
| Speculative echo shows wrong glyphs | User confusion | Off by default; unconfirmed style; 500 ms expiry; corrected by the next authoritative `PUT`. |
| Code size on the eZ80 for Stage 3 | Exceeds headroom | Stage 1–2 reclaim first; Stage 3 is gated on measured gap ≥ 12 KB; Forth-side words kept thin, policy in Forth modules loaded on demand. |
| USB loss issue #2 interacts with moving the drain to core 0 | Integrity regression | Keep identical pacing and mutex; B8 soak is a gate for 1a. |

---

## 15. Owner decisions requested

1. Approve Stage 0 and Stage 1 (1a → 1b → 1c) as the next milestone, including ESP app0 updates under the existing backup procedure.
2. Accept that after 1c the console transcript is held on the ESP: it is lost on an ESP restart, but still logged over USB.
3. Accept that the cooked line editor moves to the ESP in Stage 2.
4. Reserve VDP buffer range `0xFB00–0xFBFF` and packet codes `0xF0/0xF1` for VIOS.
5. Decide on pointer hardware later (Stage 5 gate); confirm "keyboard-complete, host pointer for testing" until then.

---

## 16. Critical files for implementation

- `drivers/agon-vdp/text_screen.c` (+ `include/vios/text_screen.h`): `E4` encoding replaces the VDU runs and buffered scroll scripts.
- `kernel/workbench_runtime.c`: console view removal, F1/VIEW handling, `display_reset`/`WB-REPAINT`, event dispatch at idle.
- `drivers/agon-vdp/transport.c` (+ `include/drivers/vdp_transport.h`): `F0` event ring, `F1` replies, Stage 2 removal of `local()`/history.
- `boards/agon-light-2/platform_resident.c`: `send_bytes` batching, `echo_local` retirement, console tap removal, quiet gate including events.
- `firmware/esp32-vios/vendor/agon-vdp/video/vdu_sys.h` and `video.ino`, plus `firmware/esp32-vios/include/vios_bridge.h`: `VIOS_VDU_UI` hook, VDS service in `loop()`, USB console drain moved to the control task.
- `kernel/vios_handles.c`: `/dev/win/*` and `/dev/ui/tree` built-in devices (Stage 3).
- New: `firmware/esp32-vios/lib/vios-vds/` (portable VDS core plus host tests in `firmware/esp32-vios/tests/vds.cpp`), `drivers/agon-vdp/vds.c`, `kernel/vios_ui.c`, `tools/vios_ui_bench.py`.

---

## Stage 0 results (measured on the board, 2026-10-19)

Firmware `backups/app0-2026-10-18/firmware-gui-stage0.bin` (A/B trial, confirmed).
New ESP control commands, safe while the CPU runs: `T`/`T0` counters, `PA<hh>`/`PR`
key-to-glyph probe, `KI<hex>` key injection at the PS/2 boundary. Harness tools:
`esp_stats`, `esp_probe`, `esp_inject`. Benchmark: `tools/vios_ui_bench.py`.
Data: `docs/bring-up/vios-ui-baseline-2026-10-19.json`.

| Measure | Result | Reading |
| --- | --- | --- |
| B1 flood, 1000 numbers (3944 B console output) | REPL done 0.57 s; VGA settled 1.14 s; **493 glyphs drawn** | The VGA console view shows ~1/8 of a flood. D3 (CPU tap ring, 512 B) dominates throughput. |
| B1 VDP parser blocked in USB console writes | **36 % of wall time** (57 % on a 300-line run), max 13.5 ms per write | D2 confirmed: glyph drawing waits for the USB drain. |
| B2 typing, idle CPU, key-to-glyph | median 4.4 ms, p95 5.5 ms (20 and 100 cps) | Already under the Stage 1 target (≤10 ms). |
| B3 typing with two busy tasks | 20 cps: median 30 ms, p95 38 ms; 100 cps: median 11 ms, p95 17 ms | Over the Stage 1 target (≤30 ms p95) at low rates: the key waits for CPU slices (D4). |

**Exit note:** D2 and D3 dominate. Stage 1a (USB drain off the parser) and
1c (console terminal on the ESP) give the visible gains. Idle typing latency
is not the problem; typing under load is, which Stage 2 (ESP line editing)
fixes and Stage 1c improves.

## Stage 1a and 1c results (board, 2026-10-19)

| Measure | Stage 0 baseline | 1a | 1c |
| --- | --- | --- | --- |
| VDP parser blocked in USB console writes (flood) | 36 % | 0.1 % | 1 % |
| Flood glyphs drawn on VGA (1000 numbers, 3944 B) | 493 | 492 | **3978 (all)** |
| Glyphs per second | 434 | 957 | **7058** |
| Idle key-to-glyph median / p95 | 4.4 / 5.5 ms | 3.0 / 3.5 ms | **1.0 / 1.3 ms** |
| Two busy tasks, 20 cps, median / p95 | 30 / 38 ms | 29 / 36 ms | 26 / 33 ms |
| Editor <-> console round trip (with 30 lines) | 775 ms | — | 350-450 ms |

1a: E0 console records go to USB from the control task through a 16-record
queue (`vios_bridge.h`). 1c: `firmware/esp32-vios/include/vios_console.h`
(portable terminal, tests `tests/console.cpp`) and `vios_console_vdp.h`
(fabgl renderer) draw the console from the E0 bytes; the CPU tap, 512-byte
ring and 4.6 KB shadow are gone, the CPU only sends VDU 23,0,E5 (hide/show/
clear). Views switch without a VGA mode change (WB-REDRAW still resets it).
Control command `CR<hh>` / harness tool `esp_console` read the console text
exactly; `tools/vios_console_accept.py` uses it plus a `CHAR@` pixel check.
A physical keystroke's echo is now flushed immediately (it was held up to 16
idle polls, invisible before because the CPU drew echo from its tap); host
lines still coalesce. Typing under load stays CPU-bound until Stage 2.
Not done from Stage 1: 1b (ESP surfaces for the editor) and the local F1
hotkey. A 30-minute soak gate was passed by 1a; 1c's soak is in progress.

## Editor repaint tuning (board, 2026-10-19, after the compose speedup merge)

Full editor repaint of the plain fixture (1775 cells, 2269 VDU bytes):

| Build | CPU render+send | Total | ESP parse |
| --- | --- | --- | --- |
| merged main (compose 37 ms) | 178 ms | 198 ms | 154-175 ms |
| + ESP glyph-state cache | 176 ms | 196 ms | 120 ms |
| + 16-byte UART TX FIFO fills | 170 ms | 189 ms | — |
| + pointer-walk render loop, static batch buffer | **102 ms** | **127 ms** | — |

Character edit 8 ms, newline 14 ms, scroll 10 ms (were 9/17.5/12.5).
- ESP: `plotCharacter` re-sent clip/pen/brush/paint state before every glyph;
  the state is now kept while consecutive printable glyphs arrive and dropped
  on any other VDU byte, a line wrap, the cursor flash or ESP console drawing.
- CPU: `ez80_uart0_tx_burst` fills the 16-byte TX FIFO per THRE check (CTS
  overshoot <= 15 bytes; the ESP raises RTS with 64 bytes of headroom).
- CPU: `vios_text_render` walks pointers and settles counters once per span;
  its 128-byte batch buffer is static, so locals stay within IX's +-128
  displacement (eZ80 cycle model: 141 -> 58 ms).
The ESP's glyph drawing (~68 us per glyph through the fabgl primitive queue)
is now the bound for full repaints; Stage 1b's direct blitter targets it.

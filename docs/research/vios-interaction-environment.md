# VIOS interaction environment: three directions

Research/design proposal, 2026-10-16. No UI implementation or hardware testing was performed for this study. The brief is a self-contained, malleable Forth development environment inspired by Smalltalk and Lisp machines, usable locally and headlessly, with a dependable serial recovery route.

**Selected direction:** the user chose **hybrid workbench: text first, graphics where useful**. This settles the interface direction; the staged implementation below remains a proposal, following resident-system hardware acceptance.

**Recommendation: a keyboard-first hybrid workbench, built in text-first stages.** Use tiled panes for a module/word browser, source editor, inspector and workspace; add graphics where they explain something better. Keep a plain serial CLI and independent supervisor recovery. The useful inheritance from Smalltalk and Genera is the ability to navigate, inspect, edit, test and act on the running system through consistent tools.

## What to borrow

| Environment | Valuable idea | VIOS adaptation |
| --- | --- | --- |
| Smalltalk | Integrated browser, incremental editing, source debugger and inspectable runtime | Browse module → word → exact source; inspect words, variables, deferred bindings and resource use without leaving the editor |
| Smalltalk/Pharo | Workspaces/playgrounds evaluate selected expressions; inspectors offer contextual exploration | Evaluate a selected Forth expression, show stack effects/results, follow a selected execution token or address; reuse one editor component across tools |
| Genera | Typed presentations associate displayed output with its meaning; command completion/help and multiple interaction methods share a command system | A displayed module, word, pathname or boot slot is a typed reference with valid contextual actions, selectable by keyboard and eventually pointer |
| Genera | Dynamic listener, retained output, editor integration and contextual help | A bounded transcript whose entries can be inspected or reused as command arguments; jump from diagnostics to source and documentation |

The original [Xerox Smalltalk-80 brochure](https://archive.computerhistory.org/resources/access/text/2024/02/102805419-05-01-acc.pdf), printed pages 1–4, explicitly describes the browser, incremental changes, integrated editors, debugger and process support. The project-maintained [Pharo by Example](https://files.pharo.org/books-pdfs/pharo-by-example/2009-PharoByExample-2009-EN.pdf), chapter 6, documents workspace-driven inspection and live-object exploration. Borrow their workflow while respecting VIOS's different runtime.

The primary [Symbolics Programming the User Interface manual](https://bitsavers.trailing-edge.com/pdf/symbolics/software/genera_8/Programming_the_User_Interface.pdf), printed pages 1610–1618, describes a command loop, presentations, input contexts, completion and help. The same operation can be entered as text or selected from displayed data. The [Symbolics editing manual](https://www.bitsavers.org/pdf/symbolics/software/genera_8/Editing_and_Mail.pdf) describes evaluating/compiling changed definitions from Zmacs. These are strong precedents for a Forth environment whose tools operate on the system directly. OpenGenera is the later hosted Genera system described in [Symbolics' technical history](https://www.symbolics.biz/genera.html); the proposal borrows its interaction model, not its machine emulator or Lisp runtime.

[CLIM's command documentation](https://www.lispworks.com/documentation/lw81/clim/clim-ch11-1.htm) provides a later, explicit separation between command execution and interaction gestures. Adapt that separation with small records and tables; implementing CLIM, CLOS, Morphic or a second object VM is unnecessary.

### Important semantic differences

Smalltalk's image and Genera's saved worlds preserve a rich running object system; [Symbolics describes worlds and its condition/restart facilities](https://www.symbolics.biz/genera.html). VIOS currently persists checked **source modules and boot selections**, then recompiles. It does not persist arbitrary heap objects, live stack frames, open devices or continuations. A saved workspace should therefore initially mean text, navigation state and explicit configuration, not a suspended computation.

VIOS has untyped 24-bit cells, shared data/return stacks, native-address memory access and explicit `DEFER` bindings. A UI “object” can initially be a small descriptor such as `(kind, stable identity, generation)`, without changing Forth value representation or adding message dispatch to ordinary calls. A new ordinary word definition does not retarget existing compiled calls; changing an explicit deferred service does. Show that distinction in the inspector and activation preview.

A source-level debugger that can edit and resume an interrupted computation needs more VM work: capture error state before stack cleanup, distinguish return addresses from values placed on the return stack, map token/direct instruction offsets to source, and define safe suspension and resumption. Those capabilities are not supplied by drawing a debugger window. Start with diagnostics and snapshots; later add stepping to a checked debug engine, with native execution still the normal fast path. Genera-style restart choices must only appear when the operation actually supplies a safe retry/abort contract.

## The actual display and input envelope

The design basis is the local VIOS derivative of **Agon VDP 1.04**, upstream commit `61888abd51a04ce6acf282df5e8300ed7125c434`, and **vdp-gl 1.0.3**, commit `48a0b9af72d13180b6962a6429a2ce85ca4954e8`. See the local [vendor manifest](../../firmware/esp32-vios/vendor/manifest.json) and [firmware profile](../../firmware/esp32-vios/README.md). Newer Agon documentation can describe capabilities absent from this pin.

The [Olimex board specification](https://www.olimex.com/Products/Retro-Computers/AgonLight2/open-source-hardware) identifies 512 KiB external CPU SRAM, VGA and an ESP32 graphics/audio coprocessor. Video memory lives on the ESP32 side; the eZ80 sends VDU commands over a UART. ESP32 PSRAM is not directly addressable CPU RAM.

### Modes implemented by the pinned source

The default is `legacyModes=false`. The [pinned mode switch](https://github.com/breakintoprogram/agon-vdp/blob/61888abd51a04ce6acf282df5e8300ed7125c434/video/graphics.h#L525-L648) implements the following. These are **source-supported modes**, not measurements that every mode fits the current integrated firmware or works with an attached monitor.

| Mode(s) | Pixels | Colours, in mode order | Default 8×8 text grid |
| --- | --- | --- | --- |
| 0 / 1 / 2 | 640×480 | 16 / 4 / 2 | 80×60 |
| 3 / 4 / 5 / 6 | 640×240 | 64 / 16 / 4 / 2 | 80×30 |
| 7 | 640×480 backing display | Teletext emulation | 40×25, special layout |
| 8 / 9 / 10 / 11 | 320×240 | 64 / 16 / 4 / 2 | 40×30 |
| 12 / 13 / 14 / 15 | 320×200 | 64 / 16 / 4 / 2 | 40×25 |
| 16 / 17 | 800×600 | 4 / 2 | 100×75 |
| 18 | 1024×768 | 2 | 128×96 |

Legacy selection remaps 0–3 to 1024×768×2, 512×384×16, 320×200×64 and 640×480×16. Double-buffered cases are **129, 130, 132–134, 136–143**; they correspond to the matching low-number modes plus 128. There is no implemented double-buffered 640×480×16 case 128. Query the resulting mode/dimensions/colours after a change: allocation failure restores a prior mode.

The [pinned font](https://github.com/breakintoprogram/agon-vdp/blob/61888abd51a04ce6acf282df5e8300ed7125c434/video/agon_fonts.h) is 8×8, code page 1252, and VDU character redefinition can provide border/indicator glyphs. FabGL's other bundled fonts are not automatically exposed by the current VDP protocol. Do not assume Unicode, proportional fonts or a selectable 8×16 font. The regular display is raster graphics with text drawn into it, not a separate hardware text plane. Keep a semantic screen model; `CHAR@` reconstructs characters from pixels and is unsuitable as authoritative editor storage.

The VDP supplies text/graphics viewports, cursor placement, scrolling, colours, drawing primitives, bitmaps and sprite commands. Source reserves 256 sprite records and legacy bitmap IDs, with practical limits set by memory and rendering cost. This is software sprite composition, not 256 free hardware sprites. The basic bitmap-upload command receives RGBA8888, four bytes per pixel; buffered bitmap formats include RGBA2222. See [sprite protocol source](https://github.com/breakintoprogram/agon-vdp/blob/61888abd51a04ce6acf282df5e8300ed7125c434/video/vdu_sprites.h). Use uploaded icons sparingly and reuse them.

### Memory: the framebuffer is not free PSRAM

[vdp-gl's paletted framebuffer allocator](https://github.com/avalonbits/vdp-gl/blob/48a0b9af72d13180b6962a6429a2ce85ca4954e8/src/dispdrivers/vgapalettedcontroller.cpp#L124-L130) requests **internal** ESP32 RAM, with DMA line buffers in addition. Its 64-colour controller uses one byte per pixel in DMA-capable memory. Buffered assets can use PSRAM, but that does not remove the internal framebuffer/DMA limit. Extra firmware tasks, queues and fragmentation affect mode availability.

Calculated framebuffer payloads, excluding scanline tables, DMA descriptors, fonts, sprites and allocation overhead:

| Example | One buffer | Two buffers where implemented |
| --- | ---: | ---: |
| 640×480, 2 colours | 37.5 KiB | 75 KiB |
| 640×480, 4 colours | 75 KiB | 150 KiB |
| 640×480, 16 colours | 150 KiB | No corresponding pinned mode |
| 640×240, 16 colours | 75 KiB | 150 KiB |
| 320×240, 64 colours | 75 KiB | 150 KiB |
| 1024×768, 2 colours | 96 KiB | No corresponding pinned mode |

Prefer the current default mode 1, 640×480×4, single buffered, for the initial workbench. Mode 2 gives the same text capacity with less framebuffer storage. A 16-colour mode is optional polish after measurement; the editor needs readable selection and focus more than many colours. High-resolution modes increase text density but require display legibility and available-heap checks.

### Keyboard and mouse

The Olimex [manual](https://github.com/OLIMEX/AgonLight2/blob/b7f2a21c282813efd8cdf26573eb5126876432a8/DOCUMENTATION/AgonLight2-user-manual.pdf) explicitly requires a **PS/2-capable keyboard**, even though the socket is USB-A. It is not a USB HID host. The current user setup has no attached display or keyboard; local interaction cannot be visually or physically accepted until the user attaches them.

The pinned [PS/2 controller](https://github.com/avalonbits/vdp-gl/blob/48a0b9af72d13180b6962a6429a2ce85ca4954e8/src/comdrivers/ps2controller.cpp#L1200-L1214) defaults to keyboard GPIO33/32 and mouse GPIO26/27. VIOS uses GPIO26/27 for ZDI boot/recovery wiring. There is no stock dedicated mouse connector to assume. A PS/2 mouse would require a separately reviewed wiring/pin and initialization arrangement that preserves ZDI, or another future input adapter. The first UI must not require a mouse or enable it on these pins. The current `setupKeyboardAndMouse()` calls the library's default `begin()`, which creates both ports; audit that initialization too, rather than assuming `mouseEnabled=false` prevents all pin activity. Host-injected selection/key events are a separate possibility, not proof of local mouse hardware.

The firmware sends virtual-key/modifier/down information, but the current CPU console path primarily exposes character bytes. A keyboard-first editor needs a bounded event API retaining navigation keys, modifiers and key-up state, with the existing character CLI as a compatibility adapter. Test actual keyboard mappings later; do not assume a PC terminal's escape sequences are the local PS/2 protocol.

## Three options

These RAM figures are **incremental design estimates**, not measured implementations. They exclude the existing kernel and ESP32 framebuffer. All options keep the same module/service model and serial CLI.

| Option | Interaction and implementation | Estimated additional eZ80 state | Strengths | Costs |
| --- | --- | ---: | --- | --- |
| **A. Text workbench** | Full-screen character cells, tiled browser/editor/stack/transcript; VDU renderer locally and optional terminal renderer remotely | 16–28 KiB | Smallest scope; portable; fast dirty text runs; usable without pointer | Limited diagrams and spatial tools; local VDU and remote ANSI need separate render adapters |
| **B. Hybrid workbench — recommended** | Same keyboard-first panes plus graphics inspectors, dependency diagrams and typed contextual selections; no overlapping windows initially | 32–48 KiB | Smalltalk/Genera workflow without a large desktop; useful locally and remotely; gradual graphical growth | Needs semantic view model, dirty regions and small presentation registry; more engineering than A |
| **C. Graphical desktop** | Overlapping draggable windows, menus, richer graphical inspectors and optional mouse | 64–128 KiB, plus optional ESP32 cached assets | Rich spatial environment; closest visual resemblance to historical systems | Window damage/occlusion, larger state, more redraw traffic, unresolved mouse route; largest recovery and testing burden |

For scale, one 80×60 character/attribute plane costs 9,600 bytes; two cost 19,200. A 4 KiB source editor, 2 KiB undo, 4 KiB transcript and roughly 4 KiB pane/selection metadata put a double-plane hybrid near 33 KiB before code and spare capacity. Option A can use one plane and dirty-row tracking. Option C's range assumes retained views and rebuilding damaged areas, not CPU-side full pixel framebuffers.

The accepted resident `build/vios-resident.map` places BSS end at `0x0800E6`, stack bottom at `0x0AF000`: a **calculated 187.8 KiB gap** (192,282 bytes) in that build. This is not a promised UI heap. Compiler limits remain 128 words, 8 KiB managed data and 8 KiB retained definition text; module sources have separate bounded storage. A substantial all-Forth UI cannot simply be added inside those limits. Measure occupancy, then explicitly budget larger arenas or keep compact rendering/editor storage in native modules with a Forth control layer. Never silently consume the recovery stack or assume unused physical SRAM is mapped into the current safe profile.

### Bandwidth and rendering cost

The pinned integrated profile uses **115200, 8N1** for both USB and CPU/VDP UARTs: a theoretical 11,520 bytes/s per link/direction before flow control, framing and processing. USB framing is additional overhead; console and graphics share the CPU transmit link. These are arithmetic lower bounds, not achieved benchmarks:

| Update | Payload assumption | Minimum wire time |
| --- | ---: | ---: |
| Full 80×60 text body | 4,800 bytes | 417 ms |
| One text line plus positioning | About 83 bytes | 7.2 ms |
| Ten changed 80-column lines | About 830 bytes | 72 ms |
| Cursor/selection move with a few commands | 10–40 bytes | 0.9–3.5 ms |
| Upload 320×240 RGBA8888 bitmap | 307,200 bytes | 26.7 s |
| Upload same size RGBA2222 buffered asset | 76,800 bytes, excluding setup | 6.7 s |

A filled rectangle or line is a few command bytes followed by ESP32 work; its wire cost does not scale with filled area. Actual primitive latency, queueing and contention must be measured. No option should stream whole screen bitmaps per frame or repaint every pane at video refresh rate. Double buffering can improve visual atomicity; it does not increase the serial bandwidth or make two retained pixel buffers cheap.

## Recommended architecture

```text
PS/2 events       serial CLI / host MCP
     \                 /
      command registry + single VM owner
                 |
       system inspection/action APIs
       words · sources · modules · services
       devices · resources · boot selections
                 |
      view model + bounded presentation records
          /                         \
  VDU text/graphics             CLI records
  dirty spans/rectangles        terminal/host views
```

Use one underlying inspection/action API. The local UI, CLI and MCP should obtain the same identities, dependency information, source, errors and operation results. Do not scrape human-formatted output to build the local browser. Proposed additions, not existing APIs, include paginated word metadata/source lookup, deferred-binding enumeration, read-only resource/device snapshots and structured diagnostics.

A presentation record can contain a type (`word`, `module`, `service`, `file`, `boot-slot`), stable name/generation, display bounds and allowed actions. Start with 128 records at about 16 bytes each, a 2 KiB estimate. Selection highlights a record; a command palette offers Browse, Inspect, Source, Test or other applicable actions. Tab/arrow navigation and an explicit numeric/reference form in the CLI provide the same access without a pointer. Resolve identities again before action after activation or rollback; cached raw execution tokens can become stale when dictionary arenas are reclaimed.

Keep native VM arithmetic/calls/branches unchanged. UI interaction pays explicit service-call costs; ordinary application code does not acquire object dispatch, redraw checks or per-call observer hooks. Put dense text-span emission, bounded buffer editing/search and packet handling in portable C where measurement justifies it. Put policies, tool composition, commands and replaceable views in Forth. Keep board display/input adapters separate for future architectures.

All tools share the single current VM owner. Workspaces initially contain independent **text buffers**, not independent suspended Forth tasks; make the current data stack and BASE visible. Inspector rendering must preserve application stacks, BASE and compilation state. Avoid rendering by evaluating arbitrary convenience Forth words on a user's active return stack. UI actions enter a serialized command queue at safe interpreter boundaries.

### Initial tools

1. **Browser:** modules and service groups → words → source/metadata. Show definition origin, native/Forth kind, stack-effect documentation when supplied, code sizes and direct/deferred binding behavior.
2. **Editor:** one bounded 4 KiB module draft, selection evaluation, search, small undo and clear dirty state. Stage/Test, Activate, Save and Boot Trial are distinct actions with their real transaction scope shown.
3. **Inspector:** data stack, selected cell/variable, deferred target, module dependencies/generations, device status and boot records. Raw addresses are explicit trusted-system operations, never automatically dereferenced because an integer happens to look like a pointer.
4. **Workspace/listener:** bounded transcript, editable input, repeat command and selected-expression evaluation. Results can carry typed references rather than losing their meaning in printed text.
5. **Diagnostics:** error reason, source origin and resource usage; later a checked-engine stack/source debugger. Do not label current error cleanup as resumable debugging.

An initial 80×60 layout, with dimensions adapted at runtime:

```text
+------------------- VIOS -----------------------------------------------+
| Modules / Services | Words               | Inspector                   |
| console           | CONSOLE-EMIT        | deferred -> EMIT             |
| clock             | CLOCK-MS           | module console / version 2   |
| display           | ...                | source CRC / dependencies    |
+-------------------+--------------------+-----------------------------+
| Source: console   [modified]                                           |
| : LOG-EMIT ... ;                                                       |
| ' LOG-EMIT IS CONSOLE-EMIT                                             |
+----------------------------------------------------------------------+
| Workspace / transcript                          DS: 3 cells  BASE: 10  |
| > ...                                                                 |
+----------------------------------------------------------------------+
| Browse  Inspect  Stage/Test  Activate  Save  Commands  Serial rescue     |
+----------------------------------------------------------------------+
```

The current two-generation limit is visible. Repeated edits cannot promise unlimited live recompilation: the third generation currently requires rollback or a restart/compaction path. Preserve drafts before that boundary. Existing compiled callers may retain old implementations; a dependency browser should explain why. Source persistence and explicit activation remain the authority; UI layout/history can be separate bounded text files, never a dump of pointers/stacks.

## Serial access and recovery are architectural requirements

The CLI must remain usable when no local UI is started, no monitor/keyboard is attached, the chosen mode fails, or a UI module is disabled. Keep console framing/logging separate from VDU rendering; changing `DISPLAY-WRITE` must not redirect away the only rescue console. Core rescue commands and their transport must not depend on replaceable UI words or a damaged screen model.

For a stalled eZ80 UI, the independent ESP32 control channel can halt/snapshot/restart the CPU. That is recovery control, not a second magically running Forth REPL inside the halted CPU. To keep the normal CLI responsive during UI activity, implement bounded UI steps, bounded render batches, cancellation checks at evaluator safe points and a timeout returning to the core CLI. Unchecked infinite loops or a blocked native primitive need supervisor interruption/restart; preserve the checked debug profile for cancellable evaluation.

If the ESP32/VDP itself wedges, the existing architecture cannot guarantee that an in-band USB command will still run. A stronger guarantee needs an independently scheduled bounded rescue dispatcher, watchdog/failure handling and ROM-loader/physical-reset recovery. UI rendering must not take ownership of supervisor transport or ZDI pins. This distinction must be tested rather than hidden behind “always accessible.”

## First milestone and decision gates

**First milestone: the browser/editor/inspector/workspace flow of option B, rendered as option A.** Keep mode 1 unless a queried capability requires mode 2. Use character-aligned panes and dirty text spans; allow simple rectangles for focus/selection. Defer overlapping windows, mouse hardware, pixel-heavy widgets and resumable debugging.

Before implementation, agree the shared inspection API, cancellation/recovery contract and explicit memory budget. Then implement in this order:

1. Add structured, paginated word/service/module inspection and command metadata; make CLI and MCP use them first. Verify stack/BASE preservation and stable identity invalidation.
2. Build a host-testable view model, bounded editor and renderer command log. Test navigation, buffer/undo exhaustion, escaping control bytes, partial input and draw-batch limits without hardware.
3. Add local text rendering and keyboard events, while keeping the ordinary serial CLI active. Validate headlessly using screen-character/pixel replies and command logs; this proves protocol behavior, not legibility.
4. Exercise edit → stage/test → activate a deferred service → inspect → save → restart → inspect, with failed tests and capacity errors preserving the running service and unsaved draft.
5. Verify recovery with UI evaluation loops, blocked/missing VDP replies, input floods and failed mode changes. Measure key-to-update latency, redraw byte counts, CPU/ESP32 RAM high water and ordinary VM benchmark impact.
6. With the user's physical help, attach VGA and a PS/2-capable keyboard; check readability, actual key mappings and local editing. Only then choose richer graphical views and investigate an optional pointer route.

Proposed acceptance targets, **not measured claims**: incremental selection/edit updates under 100 ms when they fit within roughly 500–800 transmitted bytes; UI state at or below 48 KiB additional eZ80 RAM; no changed VM hot-path instructions; no dropped control requests during bounded redraws; a documented supervisor route back to a core CLI after a stalled UI. Full-screen refresh latency will be appreciably above the 417 ms text-only wire minimum.

The user selected B, the text-first hybrid. It preserves a path to graphical inspection while delivering a useful development system before window management and pointer hardware become prerequisites.

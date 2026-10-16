# Display-only rendering investigation — Agon, 2026-10-16

The remaining display bottleneck is real after removing USB reporting from the measured interval. The largest opportunities are the internal UART speed, VDP viewport operations, and the number of control bytes produced by the renderer. No production firmware was changed during this investigation.

## Hardware measurements

Physical Agon Light 2, eZ80 at 18.432 MHz, VIOS resident payload 105,706 bytes, pinned VDP 1.04, mode 1: 640×480, 80×60 characters, four colours. Internal UART and USB console are currently both 115200 baud, but are separate links.

| Operation | Samples | Display command bytes | Median ms |
|---|---:|---:|---:|
| Full screen, already composed | 7 | 4098 | 560.0 |
| Full composition and screen painting | 7 | 4098 | 686.0 |
| Source scroll, already composed, current renderer | 8 | 307 | 52.5 |
| VDP viewport scroll only, with framebuffer fence | 7 | 5 | 14.608 |
| VDP viewport scroll + exposed line + viewport restoration, with framebuffer fence | 7 | 60 | 17.059 |
| Framebuffer character-query baseline | 7 | — | 8.507 |

The source is the unchanged 1,969-byte WBM1P draft. Current-renderer scrolling alternates source lines 39/40 with the editor focused. The VDP prototype scrolls the 54×38 editor rectangle upward from the initial view, with browser focus and no editor cursor highlight. These are different scenes: the prototype demonstrates the transport/offload opportunity, not a completed threefold workbench speedup. Byte counts also differ from the preceding benchmark's cursor/inspector scene.

The 60-byte prototype contains colour setup, `VDU 28` viewport selection, `VDU 23,7,0,3,8` scrolling, a cursor position, the exposed source line, and restoration of the full text viewport. Blank trailing cells are supplied by scrolling. It does not update VIOS's desired/painted shadows, row caches, cursor highlight or inspector. Those are necessary before integrating this path into the workbench.

`WB-FLUSH` measures on the device and prints its record only after its timer stops. There is no USB console output during its timed interval. Preparatory commands and echoes occur before it; result records and host round trips occur afterward. VGA text is not mirrored to USB in VIOS. The unavoidable eZ80-to-ESP32 serial display link remains in the measurement.

For precomposed samples, painting is temporarily suspended by binding `WB-PAINT` to the existing `DECIMAL` word. The observer isolates that word's BASE write and leaves no stack values. The complete desired view is composed before restoring the native paint hook and measuring `WB-FLUSH`. No definitions are added. Full composition-plus-paint samples use `WB-REDRAW WB-FLUSH`; mode reset/clear and its status record precede the timed interval.

The native painting callback alone accounts for 499.972 ms of the 560 ms full flush, and 43.122 ms of the 52.5 ms scroll flush. The remaining time includes composition/cache checks, observer isolation, loop dispatch and clock queries. It is not USB reporting time. Full flush timing ends at the ordered VDP clock response; unlike the explicitly fenced prototype, it does not include an explicit framebuffer-completion fence or physical VGA scanout latency.

The VDP prototype uses `TIMER@` around one contiguous native `VTYPE` call and `CHAR@`. `CHAR@` waits for queued drawing before reading framebuffer pixels. Every sample verifies the newly exposed glyph, with a separate check that the upper row moved. The scroll-only variant uses five individual Forth `VDU` calls and also includes colour setup and `CHAR@` in its interval. The 16-bit timer runs at 288 kHz; these short samples are below its 227.56 ms period. The fence baseline is reported separately rather than subtracted to claim an exact drawing cost.

## What the original Agon stack does

The stock VDP at our exact pinned commit defines `UART_BR` as **1152000**, while the VIOS fork currently defines **115200**. Original MOS tries 1152000 with a 384000 fallback and enables hardware flow control and UART receive interrupts. Its output path uses assembly UART routines. The conservative bring-up rate remains in VIOS and should be revisited.

Sources: [pinned VDP baud definition](https://raw.githubusercontent.com/breakintoprogram/agon-vdp/61888abd51a04ce6acf282df5e8300ed7125c434/video/agon.h), [MOS startup](https://raw.githubusercontent.com/breakintoprogram/agon-mos/main/main.c), [MOS assembly UART](https://raw.githubusercontent.com/breakintoprogram/agon-mos/main/src/serial.asm).

The VDP is a serial graphics terminal: the eZ80 sends characters and drawing commands; the ESP32 draws and generates VGA. Text in our current mode is rasterized into the ESP32 framebuffer. There is no shared framebuffer mapped into eZ80 RAM. Stock newline/viewport scrolling is performed by the ESP32 rather than by resending every changed glyph.

Sources: [VDP overview](https://github.com/AgonPlatform/agon-vdp), [pinned scroll command](https://raw.githubusercontent.com/breakintoprogram/agon-vdp/61888abd51a04ce6acf282df5e8300ed7125c434/video/vdu_sys.h), [pinned graphics implementation](https://raw.githubusercontent.com/breakintoprogram/agon-vdp/61888abd51a04ce6acf282df5e8300ed7125c434/video/graphics.h).

## Recommended implementation order

1. Raise the internal UART rate, first validating 384000 and then 1152000. Coordinate both firmware ends and startup, preserve RTS/CTS and headless recovery, and stress concurrent keyboard/console traffic. USB can remain at 115200. At 8N1, 4098 bytes require at least 355.73 ms at the present rate versus 35.57 ms at 1152000. This is a wire-time bound, not a tenfold application-speed promise.
2. Integrate viewport scrolling and drawing only exposed rows. Shift desired/painted shadows and source-row caches together, repair cursor styling, and fall back to ordinary repaint when pending changes cannot safely be represented by a scroll. The pinned 1.04 implementation needs an explicit eight-pixel movement; later documentation's zero-means-one-character convention must not be assumed.
3. Reduce command overhead. This run's source scroll changed only 41 glyph cells, but emitted 38 seven-byte span prefixes: 266 of 307 bytes were positioning/colour commands. Track output state within bounded batches and merge short unchanged gaps when repainting them costs fewer bytes than starting another span.
4. Optimize the native UART and trusted painter paths. The current sender services RX twice per output byte. Use a bounded, flow-controlled native burst writer, with a small eZ80 assembly hot path if measurement justifies it. Keep RX service and command boundaries intact. The painter observer currently saves/restores all 256 stack cells even when DS is empty; profile a trusted default-painter fast path while retaining isolation and instruction budgets for replaceable Forth hooks.
5. Cache repeated drawing sequences on the ESP32. The pinned VDP already has buffered commands. A small optional VIOS backend for runs, fills and scrolls could maintain an ESP-side character map: 80×60×2 = 9600 bytes. Keep the live system and inspection model authoritative on the eZ80, with explicit invalidation/resynchronization and ordinary VDU fallback.

Sources: [VDU commands](https://agonplatform.github.io/agon-docs/vdp/VDU-Commands/), [buffered commands](https://agonplatform.github.io/agon-docs/vdp/Buffered-Commands-API/). Modern documentation covers extensions beyond our pinned firmware; integration must check the actual implementation.

One useful layout option is a maximized, full-width editor. The existing FabGL vertical-scroll implementation swaps scanline pointers and copies the margins to preserve pixels outside a partial-width viewport. A full-width viewport avoids those margin copies. This is a source-backed opportunity, not a measured VIOS layout improvement. See [the pinned implementation](../../firmware/esp32-vios/vendor/vdp-gl/src/displaycontroller.h#L1766).

FabGL also contains a separate text-only controller with lower RAM use, but it does not support Canvas graphics. That could be an optional console backend; it is a poor default for the selected hybrid text/graphics workbench. Double buffering can improve presentation but does not remove command transmission cost. The primary recommendation is to keep the current graphics-capable mode and exploit its existing coprocessor operations first. See [the pinned text-controller contract](https://raw.githubusercontent.com/avalonbits/vdp-gl/48a0b9af72d13180b6962a6429a2ce85ca4954e8/src/dispdrivers/vgatextcontroller.h).

## Evidence and preservation

All 43 samples passed. The complete draft, dirty flag, cursor, browser selection, focus, dictionary count/catalog revision and original closed state match before and after. DS remains empty and BASE remains decimal. Receive-drop count is zero before and after. The 60-byte prototype buffer used free RAM at 0xA0000, above BSS end 0x97D39 and below the reserved stack at 0xAF000; its original bytes were backed up and restored with readback verification. Both production firmware hashes remain unchanged. No flash, SD, boot-slot or production source writes were performed.

- [All samples and device responses](render-display-only-2026-10-16.json)
- [Original state capture](../bring-up/vios-render-research-before.json)
- [Flush and scroll-only experiment](render-display-only-2026-10-16.py)
- [Contiguous scroll/exposed-line experiment](render-display-scroll-prototype-2026-10-16.py)

The experiment scripts are records for this specific existing draft/state, not general benchmark drivers. Run from the repository root using `PYTHONPATH=. .venv/bin/python <script>` through the existing TCP harness. They do not open another serial port. The second script extends the JSON produced by the first.

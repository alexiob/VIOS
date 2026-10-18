# VIOS VGA console view and local line editor

Requested 2026-10-18 by the user after the keyboard investigation
([keyboard record](../../bring-up/vios-keyboard-2026-10-18.md)): the local
keyboard + VGA must be usable without a host.

## Behavior

- Boot opens the console view (home view). F1 toggles console <-> editor;
  closing the editor (WB-CLOSE / editor close key) returns to the console.
  Commands: `WB-CONSOLE`, `WB-CONSOLE-CLEAR`, `WB-CONSOLE?`.
- Rows 0..57 are a scrolling terminal (CR, LF, BS; hardware viewport
  scroll); row 58 is a status bar; the bottom-right cell stays reserved.
- Console output is tapped at `platform_console_putc` into a 512-byte RAM
  ring (overflow counted and shown) and interpreted only at REPL idle into a
  circular 58x80 shadow; drawn only while visible, so output produced while
  the editor is shown appears when returning.
- Harness framing is hidden: a `@NONCE ` / `!CRC32XX ` prefix at line start
  or right after the `vios> `/`compile> ` prompt, and bare `@NONCE` frame
  lines. Host and LLM commands therefore read as ordinary commands.
- Block cursor at the terminal position, 500 ms blink, solid while output
  or typing arrives.
- Local line editor (transport, cooked mode): insert at cursor, Left/Right,
  Home/End, Backspace, Delete, Esc/Ctrl-C clear, Up/Down recall of the last
  4 distinct lines. The echo is a diff renderer using only printable bytes,
  spaces and BS, so the same echo works on the USB stream and the view.

## Constraints learned on hardware

- Bounded RAM work (interpreting output) must run only with RX paused; an
  early version drained console output with RTS asserted while the editor
  was open and overran the UART (lost host frames, recovered through ZDI
  without reboot). The idle hook now drains after the keyboard poll pauses
  RX, and the shadow is circular so scrolling copies nothing.
- VDP 1.04 `CHAR@` matches glyphs against the *current* text colours. The
  inverse cursor leaves them inverse, so diagnostic readback must set
  `3 COLOUR 128 COLOUR` first. This caused several false "blank screen"
  readings before it was understood; the display itself was correct.
- The redraw after reopening is deferred to the next idle behind a VDP mode
  query fence. It was added while chasing the CHAR@ artifact; it is kept as
  a cheap ordering guarantee after the mode reset.

## Results 2026-10-19

[Acceptance](../../bring-up/vios-console-acceptance-2026-10-19.json):
framebuffer shows command echo, results and markers with envelopes hidden;
20 editor/console round trips with 30 output lines each (~770 ms per round
trip including two mode resets), content preserved, no console drops.
CPU image 130837 -> ~131 KB; BSS end `0xA1668` (~55 KB below the stack).
Physical checks of the cursor blink, line editing keys and history still
need the user at the keyboard.

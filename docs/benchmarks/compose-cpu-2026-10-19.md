# VIOS workbench composition CPU cost — 2026-10-19

Why a full workbench composition cost about 126 ms on the eZ80, and what
changed. All figures here are modelled from the linked target code. Nothing
in this note was measured on the board; the hardware follow-up is at the end.

## Method and model

`tools/verification/compose_cycles.py` runs the linked `build/vios-resident.elf`
in the repo's eZ80 semantic interpreter (`tests/test_vios_native_c.py`, with
the few extra instructions these paths need). It runs `forth_init`, loads the
plain 1,969-byte fast-display fixture into a `WBFAST` draft, and times
`vios_workbench_compose` only. Render is not run: after each frame the painted
plane is set to the desired plane, as a full drain would leave it.

Cost model: one cycle per bus byte (opcode bytes fetched plus data bytes read
or written) at 18.432 MHz. CS0 is set for 0 wait states (`CS0_CTL=0x08`, eZ80
bus mode). Calibration: for the 2026-10-16 code the model gives **118.3 ms**
for a full compose after `WB-REPAINT`; the board measured **125.7 ms** (within
6%). Source scroll calibrates similarly (5.76 modelled vs 5.995 measured).
Incremental edit positions may differ from the hardware bench's, so compare
old with new within the model, and treat the model as a ranking tool, not a
replacement for the board.

## Where the 126 ms went

Old code, full compose after `WB-REPAINT` (2,181,161 cycles):

| Function | Cycles | Share | Cause |
|---|---:|---:|---|
| `vios_text_begin_frame` | 768,179 | 36.8% | 4,800 cells × ~160: per-cell compare of `painted` against blank through IX spill slots |
| `vios_text_put` + `dirty` + `__sshl` | 832,014 | 38.1% | ~1,530 cells × ~505: two out-of-line `dirty()` calls per cell, each with frame setup and a shift helper |
| `editor_row` | 258,301 | 12.4% | per-character diff walk against a blank cache, then ~6 `put` calls per row |
| everything else | ~322,000 | 14.8% | `row_end`, `__imulu`, string helpers, inspector rows |

The library copies (`memcpy`, `memset`, `memcmp`) move only ~15 KB in
total and use `ldir`/`cpi`; they were never the problem. `-Oz` code itself
was not the problem either: see "Rejected" below.

## Changes (commit 823e285)

| Change | Before | After | Size |
|---|---:|---:|---|
| `begin_frame`: blank row 0, `memcpy` it to rows 1–59, count non-blank painted cells with a pointer-walk helper | 768,203 | 161,867 with painted content | `text_screen.o` +179 B `.text` for all three text_screen changes |
| …and skip the count while painted is known blank (new 1-byte `painted_blank`, set by reset, cleared by render spans and scrolls) | 768,203 | 30,733 after `WB-REPAINT` | +1 B BSS |
| `vios_text_put`/`vios_text_clear`: pointer walk, one dirty delta per row instead of `dirty()` before and after each store | 505 per cell, 565 per call | 158 per cell, 538 per call | (included above) |
| `editor_row`: on a full frame (cache invalid, cells already blank) emit one span to the last glyph instead of the diff walk | 325 `put` calls per full frame | 103 | `workbench.o` +141 B `.text` |

Image: `.text` 152,832 → 153,152 B (+320); BSS +1 B; `__bss_end`
0xA8395 → 0xA84D6. The stack begins at 0xAF000, so 0x6B2A (~27 KiB) remains.

## End to end (modelled)

| Frame | Old ms | New ms |
|---|---:|---:|
| Full, after `WB-REPAINT` | 118.34 | **33.01** |
| Zoom on (full, painted non-blank) | 119.56 | 39.41 |
| Zoom off (full, painted non-blank) | 119.38 | 40.22 |
| Unchanged | 1.08 | 1.08 |
| TAB to editor | 3.59 | 2.19 |
| Character insert / delete | 6.71 / 6.70 | 3.62 / 3.62 |
| Newline insert / delete | 20.40 / 20.41 | 14.88 / 14.89 |
| Source scroll up / down | 5.76 / 15.98 | 5.20 / 13.38 |

In every one of these 11 frames the composed desired plane hashes the same
for the old and new image. `make test` passes (234 tests), including the
wire-framebuffer oracle in `tests/workbench_screen_native.c`.

## Rejected

- `-O2` or `-Os` on `text_screen.o`: `put` and `clear` change by only +4 to
  +8 bytes and keep the same IX spills, while `vios_text_render` grows by
  about 1 KB. The spills are architectural: the loop has five live 24-bit
  values and the eZ80 has four pointer registers besides IX.
- Restructured C loops (8-bit counters, byte pointers): the compiler still
  spills; no gain.

## Remaining and follow-ups

- **`vios_text_put` is 55% of the new full frame** (103 calls × 538 plus
  ~1,750 cells × 158).
  A hand-written eZ80 store loop (HL desired, IY painted, IX text, B count,
  C attr, D delta) is about 60 cycles per cell in the same model, saving
  ≈170k cycles ≈ 9 ms (33 → ~24 ms). Not done: the full repaint is
  dominated by 177 ms of output, incremental frames touch few cells, and the
  host ASan/UBSan oracles could not cover an asm path. If added, keep it as
  module-level `__asm__` under `#if defined(__ez80__)` in `text_screen.c`
  (so the stand-alone `text_screen.o` link test still works) with a C
  fallback and a `LinkedC` test against a Python oracle.
- **Newline is the slowest incremental compose** (14.9 ms modelled): 47% is
  `editor_row`'s per-character diff walk over shifted rows. A
  first/last-difference span would skip most of it.
- `row_end` (9.5%) scans attributes per character to find spans.

## Hardware follow-up (owner)

With this commit deployed at 1,152,000 internal baud and the TCP host
service owning the board (replaces the unsaved workbench draft):

```
.venv/bin/python tools/vios_fast_display_bench.py --baud 1152000 --fixture plain --samples 7 --output docs/benchmarks/fast-display-<date>.json
```

Compare the `compose_ms` medians against 2026-10-16: `full_precomposed`
125.715, unchanged 1.278, character edit 7.851, newline 12.486, source
scroll 5.995. Expected direction: full near 35 ms, character edit about
half, the rest flat to slightly lower. The `--fixture mixed` run (old full
compose 139.611 ms) exercises tabs and CRLF.

# VIOS source-row optimization — 2026-10-16

Source scrolling and newline edits retain the same screen output while using
bulk projection of tab-free ASCII prefixes and pointer-based sparse glyph
differences. Cursor style repair happens outside the per-byte difference loop.
Tabs, CR/LF/CRLF, horizontal clipping and invalidation retain their semantics.
No additional BSS or changes to UART scheduling/flow control.

The manifest records the accepted CPU/app0 hashes, tests and hardware evidence.
Source and documentation archives are exact snapshots. The preceding immutable
release is `artifacts/vios-incremental-2026-10-16`. See
`docs/benchmarks/source-rows-2026-10-16.md` for timings and measurement limits.

Host review compares against two frozen historical models, including 30,000
mixed operations and 24,959 boundary cursor cases with all 4,800 cells/styles
checked. Review scripts/logs are in `review/` and never access hardware.

`restore-and-control.py` is the release acceptance recipe: it restores the
specific host-preserved starting draft. Preserve newer user work before running
it again. The normal CLI is `.venv/bin/python tools/vios_cli.py`; MCP stdio uses
`/usr/bin/nc 127.0.0.1 8765` through the existing single serial owner.

Application rollback requires stopping the serial owner, then writing only
app0 at offset0x10000 with the verified backup named in the manifest. CPU flash,
ESP bootloader/partition table and SD source boot slots were not changed.

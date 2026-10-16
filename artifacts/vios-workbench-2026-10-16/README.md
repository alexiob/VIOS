# VIOS accepted text-workbench milestone — 2026-10-16

The accepted CPU/app0 pair is `vios-accepted.bin` (98,270 bytes) and `firmware-accepted.bin` (656,512 bytes). `manifest.json` checks all hashes. App0 embeds exactly the RAM-verified CPU and unchanged 46-byte native SRAM bootstrap. CPU flash, ESP bootloader/partition table/app1 and confirmed SD boot A generation 7 were preserved.

The 80×60 workbench provides browser, source editor and live inspector. CLI and 19 typed MCP workbench tools share one board owner. The default dictionary capacity is 512, configurable per build; 391 live words and high execution tokens were exercised on the physical CPU. Two screen planes, dirty-row index, source draft and undo are static; no target JSON/heap.

Measured native rendering: populated repaint 503.274ms/4123B, unchanged 0 B, one-character edit 4.215 ms / 32 B, selection 4.760 ms / 39 B, source scroll 50.809 ms / 370 B. These exclude composition; full composition remains 175.056 ms and is the next optimization. Render methods and host latency are separately recorded in docs/benchmarks/workbench-2026-10-16.md. The user confirmed visible VGA inspector and apparent keyboard operation.

Evidence includes 163 passing Python/native tests, 146 linked-engine execution probes, 355 real hardware requests, 8 framebuffer glyph comparisons, 50 maximal source requests during repaint,three-engine benchmarks,source persistence and 14 resident-startup checks. The actual MCP stdio proxy and pending KEY/USB continuation were checked with panes open. `screen-model.png` reconstructs captured WB-VIEW cells with the pinned Agon font; it is not a camera photograph.

`source-code.tar.gz` contains the exact build/source/vendor files, checked by `source-sha256.json`. `documentation-evidence.tar.gz` preserves positive and negative development evidence. `diagnostics` retains the failed RX-window candidate and the full-scan renderer baseline. Existing source/firmware archives remain untouched. Toolchains are external dependencies; versions are recorded in the manifest.

Previous app0 backup: backups/esp32/pre-vios-workbench-app0-2026-10-16.bin (630,544B), SHA256 0a064beaba2be05f07960a9e05da1bb5a161af8ac7799c55fc6f4c3459e2a129. The application-only rollback command is in docs/bring-up/vios-workbench.md. It does not erase SD source packages or source-boot bundles.

The running TCP harness is 127.0.0.1:8765; share it with `.venv/bin/python tools/vios_cli.py` or an MCP stdio client using `/usr/bin/nc 127.0.0.1 8765` . One process owns USB serial. See docs/bring-up/vios-workbench-live-session.json. Boot intentionally still selects original demo A; WBM1P is a saved package loaded into the current RAM session, not an implicit boot-slot update.

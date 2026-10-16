# VIOS accepted resident milestone — 2026-10-16

Use `firmware-accepted.bin` and `vios-accepted.bin`. Earlier `firmware*.bin` and `vios-resident.bin` files in this directory are preserved development/diagnostic candidates. `manifest.json` identifies every file and its digest.

The accepted ESP32 application is 630,544 bytes; update only app0 at 0x10000 after stopping the serial-owning harness. It embeds the 72,292-byte CPU image and 46-byte native SRAM bootstrap. CPU flash was not programmed. The original ESP32 bootloader, partition table and app1 were preserved. The application-only restore image remains at `backups/esp32/pre-vios-resident-app0-2026-10-16.bin`; the runbook supplies its verified command.

`source-code.tar.gz` contains 280 build/source files, including the pinned VDP/library source needed to build, metadata and licenses. `source-sha256.json` verifies those workspace paths. Toolchains and host environments are external dependencies with pinned versions. `documentation-evidence.tar.gz` preserves documentation, test/build logs and serial evidence, including negative attempts. Native linked-code checks can be reproduced with `make test verify-forth-engine`.

`sd-startup/VIOS` is a read-only host backup of the accepted demonstration startup: five exact-source packages, both source bundles and both journal records. Each format CRC was verified before saving; SHA256 values are in the manifest and backup report. Preserve any existing SD contents before using these files for recovery. The accepted demonstration is slot A (11/12); slot B contains v2 (22/23).

The running harness uses `--backend resident --tcp 127.0.0.1:8765`; the human CLI is `.venv/bin/python tools/vios_cli.py`. A standard MCP stdio client can launch `/usr/bin/nc 127.0.0.1 8765` to share that owner. See the live-session record and harness runbook. No source module or file writes are needed for attachment.

Actual hardware checks cover source integrity, all engines, graphics replies, SD save/reload, live rebinding/LIFO rollback, A/B confirmation/revert, physical cold power-on, stuck-KEY/VDP recovery and one automatic watchdog reset. Three consecutive watchdog failures are fixture-tested only. Display/audio protocol readback does not establish physical legibility or audible output. A/B source bundles share one FAT filesystem; ESP firmware A/B rollback is not implemented.

The selected text-first hybrid workbench remains a design proposal. No UI panes/editor are included in this milestone.

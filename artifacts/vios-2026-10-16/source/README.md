# VIOS / Agon Light 2

VIOS now runs a resident Forth interpreter and compiler on the physical Agon Light 2. It uses native 24-bit cells, eZ80 assembly token and direct-threaded engines, and a standard-C semantic reference. The Agon build starts in **DIRECT** mode; all three engines remain available for comparison. The compiler emits both native representations and retains definition source for `SEE` and live development.

The 2026-10-16 hardware milestone covers compilation, calls, control flow, nested loops, memory, execution tokens, deferred rebinding, and error recovery. In the measured checked-and-profiled build, direct threading took **11–14% less time than token threading** across five tested workloads. These results describe this implementation and these workloads; see the [benchmark report](docs/benchmarks/README.md).

Build with the provisioned AgonDev and Xtensa toolchains and Python environment:

```sh
make vios esp32-probes test
```

For a persistent headless session, launch the host harness and call its `deploy` tool explicitly:

```sh
.venv/bin/python tools/vios_harness.py --port /dev/cu.usbserial-22430
```

It exposes MCP over stdio, or loopback TCP with `--tcp 127.0.0.1:8765`. JSON and networking run on the host; the Agon executes Forth over raw UART. Follow the [harness runbook](docs/bring-up/vios-harness.md) for client configuration, tools, state handling and debug access. The [Forth runbook](docs/bring-up/vios-forth.md) covers architecture, examples and direct hardware verification.

Deployment is RAM-only and readback-verified. The temporary ESP32 bridge replaces VDP while running; graphics/audio, SD persistence and independent cold boot remain future work. The original firmware is backed up, and this Forth milestone does not program processor flash. Resetting removes the RAM session. [Wiring, prerequisites and recovery](docs/bring-up/ram-console.md) document the existing bring-up route.

- [Forth milestone and limitations](docs/bring-up/vios-forth.md)
- [Measured benchmark results](docs/benchmarks/README.md)
- [Measured artifact sizes and hashes](artifacts/vios-2026-10-16/manifest.json)
- [Forth implementation ledger](docs/bring-up/vios-forth-progress.md)
- [Hardware reference](docs/research/hardware.md)
- [Original eZ80 backup metadata](backups/ez80/original-2026-10-16.json)

This workspace is not currently a Git repository. Archived references retain their notices and licenses. ESP32 ZDI protocol code derives from the MIT-licensed Agon recovery project; see `tools/esp32-ram/NOTICE.txt`.

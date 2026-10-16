# VIOS headless harness

The host harness exposes the running Forth system, source modules, boot selection and bounded hardware debug through MCP stdio or loopback NDJSON TCP. JSON, schemas and networking run on the Mac. The Agon executes Forth; the resident ESP32 application multiplexes console/control/status over USB serial while retaining its VDP. Integrated hardware acceptance is recorded in the [resident ledger](vios-resident-progress.md).

The current interface has 48 tools, including 19 `wb_*` tools for shared live
browsing, exact source editing, inspection, stage/activate/save and display
snapshots. See the [workbench runbook](vios-workbench.md). Full screen reads use
one bounded native snapshot command and reject inconsistent surrounding state;
physical editing during a read can produce a clean conflict instead of mixed data.

## Start and attach

```sh
make vios-resident
.venv/bin/python tools/vios_harness.py --backend resident --tcp 127.0.0.1:8765
```

Call `connect` to attach to installed resident firmware. This does not deploy a CPU image or upload a ROM helper. Initial attachment permits 120 seconds because opening this board's serial driver can reset it. `deploy` is an explicit development/recovery operation that replaces CPU RAM and verifies every byte; it is unnecessary for ordinary resident startup. Construction, MCP initialization and tool listing do not open hardware.

One harness process owns `/dev/cu.usbserial-22430` and keeps it open across clients. All operations share one lock. Do not run a second serial terminal or esptool against that port. Disconnecting a TCP client leaves the board/session intact. An initial `connect` assumes an idle REPL; an unknown program waiting for `KEY` requires explicit recovery rather than speculative input.

For MCP stdio, omit `--tcp`. Configure absolute paths, including the resident backend:

```json
{
  "mcpServers": {
    "vios": {
      "command": "/Users/alex/Documents/Projects/iob.dev/repos/agon-light-2/.venv/bin/python",
      "args": ["/Users/alex/Documents/Projects/iob.dev/repos/agon-light-2/tools/vios_harness.py", "--backend", "resident"]
    }
  }
}
```

Defaults for the image and log resolve relative to this checkout. `--image`, `--port` and `--log` override them. Protocol output uses stdout; diagnostics use stderr. The earlier RAM-only bridge remains available through `--backend ram` (the compatibility default); see its [historical runbook and measurements](vios-harness-ram-profile.md). `--fast-console` concerns that raw bridge; resident source uses atomic framed requests.

For a human console through the existing TCP harness:

```sh
.venv/bin/python tools/vios_cli.py
```

The CLI supports ordinary Forth lines, compilation/pending state, explicit KEY input, inspection and recovery. See [CLI usage](vios-cli.md).

To let a standard MCP stdio client share that same running TCP harness with the human CLI, use this tested macOS configuration:

```json
{
  "mcpServers": {
    "vios": {
      "command": "/usr/bin/nc",
      "args": ["127.0.0.1", "8765"]
    }
  }
}
```

Netcat only connects stdio to the existing newline-delimited TCP stream. It opens no serial port. Closing that MCP client leaves the shared board running. The earlier direct Python stdio configuration owns the board itself and therefore requires the TCP harness to be stopped first. Actual-board [stdio bridge evidence](vios-resident-mcp-stdio-proxy.json) covers discovery and tool execution through this configuration.

## Protocol and tools

MCP uses the **2025-11-25** lifecycle and newline-delimited stdio JSON. Loopback TCP uses the same JSON-RPC methods with one UTF-8 JSON object per line; it is a custom transport, not MCP Streamable HTTP. TCP accepts only `127.0.0.1` binds. It has no authentication. There is no board TCP/IP stack, native USB Ethernet or mutating UDP RPC.

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"my-agent","version":"1"}}}
{"jsonrpc":"2.0","method":"notifications/initialized"}
{"jsonrpc":"2.0","id":2,"method":"tools/list"}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"connect","arguments":{}}}
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"evaluate","arguments":{"source":": SQUARE DUP * ;\n7 SQUARE . CR"}}}
```

There are 29 tools. Schemas returned by `tools/list` are authoritative for argument bounds.

| Tools | Purpose |
| --- | --- |
| `connect`, `reboot`, `deploy` | Attach, intentionally restart, or explicitly replace RAM |
| `evaluate`, `await_result`, `console_input`, `abort` | Stateful source execution and explicit interactive input |
| `words`, `inspect_word`, `stacks`, `resources`, `benchmark` | Dictionary/source metadata, stacks, capacities and checked benchmarks |
| `module_stage`, `module_activate`, `module_discard`, `module_rollback` | Transactional source replacement |
| `modules`, `module_source`, `module_save`, `module_load`, `load` | Exact source/hash inspection and native SD persistence |
| `boot_status`, `boot_trial`, `boot_confirm`, `boot_revert` | A/B source-bundle trial and selection |
| `debug_registers`, `memory_read`, `memory_write`, `resume` | Bounded ZDI inspection/write/readback and state-preserving resume |

Notifications never invoke board tools. Results include a JSON-encoded text content item and `isError`. Language/runtime failures are tool errors; malformed RPC or arguments return JSON-RPC errors. Frames are limited to 1 MiB. Module metadata/dependencies are validated before transmission.

## State, integrity and recovery

Source is printable ASCII; tabs become spaces. Blocks are limited to 65536 characters. Resident source lines are limited to **235 bytes**, leaving room for `@NONCE !CRC32 SOURCE` within the target's 255-byte line. The raw profile permits 245 bytes with its shorter envelope. The whole block is validated before any line is sent.

Resident transport assembles each complete line, checks its length/CRC and acknowledges enqueueing before forwarding it. The CPU checks source CRC independently before execution. One completion nonce identifies each result. The next source line is sent only after completion. Definitions, selected engine, numeric base and stacks persist across calls. Source retained by the compiler excludes transport metadata.

An unfinished definition returns `compiling: true`; continue it with `evaluate`, or cancel at the idle compile prompt with `abort`. Inspection commands refuse to interfere with an unfinished definition. A language error stops the remaining source block.

A timeout returns `pending: true` without closing the port or discarding received bytes. Call `await_result` to continue waiting. A program consuming `KEY` gets only explicitly supplied bytes:

```json
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"evaluate","arguments":{"source":"KEY . CR","timeout":0.2}}}
{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"console_input","arguments":{"hex":"41"}}}
{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"await_result","arguments":{"timeout":5}}}
```

`console_input` accepts 1..16 bytes; callers must know the program's expected input. Ctrl-] remains reserved by the compatibility API. New source is refused while execution is pending. `remaining_source` reports unsent lines; it is never silently retried. A partial transmission, missing mutating acknowledgement or output-integrity failure leaves state uncertain.

Normal `reboot` refuses pending execution, compilation or uncertain state. Explicit `reboot` with `{"force":true}` abandons that state and loses unsaved RAM changes, then waits for resident startup over the same serial handle. The CLI spells this `:recover`. A missing restart acknowledgement is ambiguous and is not automatically retried. Recovery does not erase SD packages or program eZ80 flash.

Debug operations pause the CPU and restore its prior running/paused state. `resume` continues the existing PC; fresh deployment enters `040000`. Reads permit `040000..0BFFFF` in CRC-checked chunks. Writes permit at most 256 bytes below `0AF000`, with independent readback. Those range checks do not make arbitrary writes safe for running code/data.

## Persistence, cost and evidence

`module_stage` → `module_activate` → `module_save` separates testing, live replacement and persistence. Dependencies pin source CRCs; `module_source` returns exact text plus host-computed SHA256. `boot_trial` creates a pending source bundle; reboot tests it before confirmation. These are source packages, not suspended VM images. Both source slots share the SD filesystem. See [resident architecture and limits](vios-resident-system.md).

MCP/JSON/TCP add no target parser or network stack. Target instrumentation, source retention, native services, storage and the resident transport do consume RAM/code; their combined footprint belongs to this resident build, not the earlier 19,921-byte VM. Normal native VM dispatch does not parse protocol messages or perform generic device dispatch.

Run host/native checks with `make test verify-forth-engine`. Actual-board acceptance uses `tools/vios_resident_acceptance.py`; its explicit boot phase requires a clean unconfirmed baseline and changes startup. Evidence is retained alongside failed diagnostic attempts in the [progress ledger](vios-resident-progress.md). Earlier raw-bridge latency and size results remain in the [historical profile](vios-harness-ram-profile.md); do not apply those numbers to the resident transport.

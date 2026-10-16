# VIOS headless LLM harness

`tools/vios_harness.py` exposes VIOS through MCP stdio or a loopback TCP
JSON-RPC connection on the Mac. `tools/vios_board.py` owns the USB serial
connection and serializes all board operations. The Agon runs its native Forth
and a small ASCII request envelope; JSON, MCP and TCP run on the host.
The host layer adds **zero bytes to the CPU image**. Core word metadata and
the request envelope, and the ESP32 debug resume change, are separate target changes.

## Start and connect

Build VIOS and the RAM loader first:

```sh
make vios esp32-probes
.venv/bin/python tools/vios_harness.py --port /dev/cu.usbserial-22430
```

Stdio is the default. Optional `--fast-console` sends each envelope plus CR in
one serial write instead of the default per-character pacing. It is opt-in
while its 245-byte line/long-definition hardware stress and latency measurements
are evaluated. It does not change source framing or queue input while busy.

An MCP client should launch the Python executable and
script with absolute paths; the default image, ESP32 loader and serial log
are resolved relative to this checkout, independent of the client's working
directory. Optional `--image` and `--log` override those paths.

For example, a client accepting an `mcpServers` configuration can use:

```json
{
  "mcpServers": {
    "vios": {
      "command": "/Users/alex/Documents/Projects/iob.dev/repos/agon-light-2/.venv/bin/python",
      "args": ["/Users/alex/Documents/Projects/iob.dev/repos/agon-light-2/tools/vios_harness.py"]
    }
  }
}
```

Construction, initialization and listing tools do not access hardware. Call
`deploy` explicitly to load the ESP32 RAM bridge, identify/halt the eZ80,
verify RAM mapping, upload the VIOS binary, read back every byte, and enter
VIOS. Repeated tool calls keep that **one existing serial port open**. Calling
`deploy` again explicitly starts a fresh VIOS image through the same port.
Closing/reopening the USB serial port can reset the board; run one harness
process for the board and keep it alive. `close()` releases its session once.
Esptool and deployment progress go to stderr; stdout contains protocol JSON
only. The protocol implementation uses Python's standard library, while live
USB deployment reuses the project's existing esptool/pyserial environment.

For a host TCP client instead of MCP stdio:

```sh
.venv/bin/python tools/vios_harness.py --tcp 127.0.0.1:8765
```

Each TCP connection uses one UTF-8 JSON object per newline and follows the
same initialization and tool methods below. Multiple connections share one
board/session and operation lock. TCP mode runs until interrupted; disconnecting
a client does not reset the board. Only `127.0.0.1` binds are accepted. This is
a custom NDJSON transport, not MCP Streamable HTTP, native USB/IP, or an Agon
network stack. TCP provides ordered, reliable control; no mutating UDP RPC is
implemented. This local endpoint has no authentication; expose it only through
an intentionally configured trusted tunnel if remote access is required.

## Protocol and tools

The server implements the MCP **2025-11-25 initialization lifecycle**. Its
version is explicit; it does not claim the different 2026-07-28 lifecycle.
The implementation follows the official
[lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle),
[stdio framing](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)
and [tools](https://modelcontextprotocol.io/specification/2025-11-25/server/tools)
specifications. A client requesting another version receives `2025-11-25`
and can disconnect if it cannot support it.

The server advertises 14 tools. A minimal exchange consists of these request lines:

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"my-agent","version":"1"}}}
{"jsonrpc":"2.0","method":"notifications/initialized"}
{"jsonrpc":"2.0","id":2,"method":"tools/list"}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"deploy","arguments":{}}}
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"evaluate","arguments":{"source":": SQUARE DUP * ;\n7 SQUARE . CR"}}}
```

Requests return their original string/integer ID. Notifications without an ID
are ignored and never invoke board tools. `ping` returns an empty result.
The server advertises only tools. Tool results contain a text content item
with JSON-encoded structured data plus `isError`; language/runtime failures
are tool errors, while malformed RPC requests/arguments return JSON-RPC errors.
Malformed JSON is `-32700`, invalid requests `-32600`, unknown methods `-32601`,
and invalid arguments/unknown tools `-32602`. Frames are limited to 1 MiB.

| Tool | Arguments and returned information |
| --- | --- |
| `deploy` | No arguments; verified readback digest, mapping, identity, boot output. Explicitly resets VIOS state. |
| `evaluate` | `source` string; optional `timeout` seconds per line, default 15, maximum 300. Returns output, `ok`, `compiling`. |
| `await_result` | Optional `timeout` seconds, default 15, maximum 300. Continue waiting for a pending evaluation without transmitting anything. |
| `console_input` | `hex` encoding 1..16 bytes for a pending KEY/read; Ctrl-] (`0x1D`) is reserved and rejected. |
| `words` | No arguments; raw output plus dictionary names. |
| `inspect_word` | `name`; SEE source and metadata: execution handle, kind, token/direct addresses and byte counts. |
| `stacks` | No arguments; `.S` output plus stack depth/value text, preserving the user's numeric base. |
| `resources` | No arguments; resource output and parsed fixed-decimal key/value fields. |
| `benchmark` | No arguments; checked BENCH transcript, waits up to 120 seconds. |
| `abort` | No arguments; Ctrl-C cancels an unfinished definition at the idle REPL. Refuses to queue input while execution is pending. |
| `debug_registers` | No arguments; status and AF/BC/DE/HL/IX/IY/SP/PC. Briefly pauses and restores prior running state. |
| `memory_read` | Integer `address`, optional `count` default 256 (1..65536). Reads only `0x040000..0x0BFFFF`, in <=256-byte CRC-checked chunks. Returns hex and CRC32. |
| `memory_write` | Integer `address`, `hex` containing 1..256 bytes without separators. Writes only `0x040000..0x0AEFFF`; checks acknowledgement and independent readback. |
| `resume` | No arguments; resume the existing PC with debug command `Y`, preserving interpreter state. |

## Source framing and state

Source accepts printable ASCII, with tabs converted to spaces. CR/LF are line
separators; each source line is limited to **245 bytes**, leaving ten bytes for
its request envelope within the REPL's 255-byte line capacity. The entire source
block is validated before any line is sent. Blocks are limited to 65536 characters.
Definitions, stacks, selected engine and BASE persist between tool calls.

The host sends exactly one `@XXXXXXXX SOURCE` line with a fresh 8-digit
hexadecimal nonce. The target removes that envelope before evaluating or
preserving source. After evaluation it emits one completion frame:

```text
\r\n@XXXXXXXX\r\nok\r\nvios> 
\r\n@XXXXXXXX\r\nerror\r\nvios> 
```

An unfinished definition ends with `compile>` instead. Original language error
text appears before the error marker. The host derives success from the marker
status, preserving all ordinary output and removing the envelope from its echoed
source. No synchronization input is queued while the program runs, so `KEY`
cannot consume a hidden nonce. Each completed source line is acknowledged before
the next source line is sent.

Incomplete definitions return `compiling: true`; send more source with
`evaluate` or use `abort` at the idle compile prompt. Other interactive query
tools refuse to interrupt a definition. A language error stops the remaining
source block. Host requests are serialized.

On a timeout, `evaluate` returns `ok: false`, `pending: true` (MCP `isError: true`)
without closing the port or discarding any received bytes. `await_result` resumes
reading that same completion frame, including markers split across timeouts.
For a program waiting for `KEY`, feed exactly the intended bytes with
`console_input`, then call `await_result`. Example calls after deployment:

```json
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"evaluate","arguments":{"source":"KEY . CR","timeout":0.2}}}
{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"console_input","arguments":{"hex":"41"}}}
{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"await_result","arguments":{"timeout":5}}}
```

This supplies the character `A` (decimal 65). Each input call is limited to the
16-byte receive FIFO; the caller must supply the program's expected input and
avoid accumulating unconsumed typeahead. Ctrl-] is intercepted by the ESP32 bridge
and therefore cannot be sent as console input.

A timeout does not queue later source lines. `remaining_source` returns any
unsent lines for the caller to submit explicitly after the pending line finishes.
A language error likewise stops the block. New `evaluate` calls are rejected
while a previous line is pending. `abort` refuses pending execution because
Ctrl-C would be consumed as an ordinary KEY value rather than unwinding the VM.
Use explicit console input and `await_result`, or debug inspection and explicit
redeployment for a wedged program. A partial serial write marks evaluation
unsynchronized and requires redeployment.

A failed DEBUG or RESUME acknowledgement, or an incomplete deployment, leaves
the bridge mode unknown.
All board operations, including redeployment through that ambiguous connection,
are rejected: restart the harness process to reload the ESP32 ROM/RAM loader.
This is distinct from an ordinary evaluation timeout, where mode and pending
completion remain known and the same connection is retained.

## Debug and verification

Debug operations send three Ctrl-] bytes, wait for the bridge's `DEBUG` reply,
then issue read-only `S`/`V` or bounded RAM `W`. A previously running board is
resumed using `Y`; an already paused board remains paused. This restoration
also runs when a debug read/verification fails. `X` is reserved for deployment
at the image entry, not ordinary debug resume. There is no flash-write path.
RAM writes are a trusted systems feature and can change running code/data.

`tests/test_vios_harness.py` covers one-session persistence, no extra loader
reset, multi-line compilation and envelope framing, source validation before
writes, language errors, debug pause restoration, verified RAM writes and range
bounds, KEY input and partial-marker recovery, failed mode-transition guards,
JSON-RPC lifecycle/framing/results, and two ephemeral loopback TCP
clients sharing the backend. These tests use fakes and do not open hardware.
Run them with:

```sh
.venv/bin/python -m unittest discover -s tests -p test_vios_harness.py -v
make test
```

Thirteen harness tests and the full 30-test suite passed during this revision.
The real board also passed both transports, including live definitions and
deferred rebinding, multiline compilation, BASE/stack-neutral inspection, error
recovery, pending KEY input, CPU register inspection, CRC-checked RAM write and
restore, and continued Forth execution after debug resume. `Y` preserves UART
FIFO state; only fresh `X` deployment resets it.

## Measured cost and latency

The tested CPU image is **19,921 bytes**, with **75,494 bytes of BSS** and a
separately reserved 4 KiB native stack. This intentionally includes both native
engines, the C reference, both code arenas and source retention. Within the
conservative 448 KiB deployment region, about 351 KiB remains outside those
reservations. A target profile can later remove unused backends/arenas.

MCP, JSON and TCP add **zero target bytes**. Tagged requests, SYNC, fixed-decimal
word metadata and true stack peaks together increased the earlier 18,929-byte
VM image by 992 bytes; that build delta includes all those shared instrumentation
changes, rather than isolating the envelope alone. Metadata/peak storage added
about 518 bytes of BSS. Request parsing uses the existing 256-byte line buffer
plus a nine-byte nonce on the native stack. Normal execution dispatch never
parses JSON or network packets. The ESP32 helper is also RAM-only; its image and
source identity are in the [artifact manifest](../../artifacts/vios-2026-10-16/manifest.json).

Median end-to-end request times, nine samples each, on this Mac/USB/board:

| Transport/profile | Short evaluation | Word inspection | Resources |
| --- | ---: | ---: | ---: |
| TCP, paced console | 124.2 ms | 185.5 ms | 131.2 ms |
| MCP stdio, paced console | 128.4 ms | 186.4 ms | 129.7 ms |
| TCP, `--fast-console` | 12.6 ms | 24.0 ms | 23.0 ms |
| MCP stdio, `--fast-console` | 12.1 ms | 23.3 ms | 22.5 ms |

Evaluation source was `1 2 + DROP`; inspection used `HARNESS-SQUARE`. These
include host JSON, UART transport/echo, REPL execution and completion framing;
they are not VM dispatch times. Fast mode writes the whole envelope at once.
It passed 50 repeated 245-byte source lines plus a maximum-length definition
on each transport, as well as the full control test. It is recommended for this
tested raw bridge; the paced default remains available for conservative bring-up
and other firmware paths. Add `--fast-console` to the launch/config args above.
115200 baud sets a wire ceiling of 11,520 bytes/s before framing; hex debug
transfers use two characters per data byte and conservative ZDI timing.

MCP's host overhead is small relative to this serial exchange. A custom binary
target protocol would mainly help bulk transfer and streaming; it would add
decoder/state/versioning costs now. This milestone uses small tagged ASCII
requests and host MCP for discoverable tool schemas. TCP provides reliable,
ordered control over the same USB serial bridge; a target TCP/IP or UDP stack
would consume CPU/RAM without making the CH340 a USB network interface.

Evidence: [paced TCP](vios-harness-tcp-hardware.json),
[paced MCP](vios-harness-stdio-hardware.json),
[fast TCP](vios-harness-fast-hardware.json), and
[fast MCP](vios-harness-stdio-fast-hardware.json).
Repeat explicitly on the attached board with:

```sh
.venv/bin/python tools/vios_harness_hardware.py --fast-console --output /tmp/vios-tcp-check.json
.venv/bin/python tools/vios_harness_hardware.py --transport stdio --fast-console --output /tmp/vios-mcp-check.json
```

These commands create fresh RAM sessions. Stop the existing board-owning server
first. Future OS device/filesystem/scheduler services should expose native Forth
words and structured inspection through this boundary; full OS services and
native networking are subsequent work.

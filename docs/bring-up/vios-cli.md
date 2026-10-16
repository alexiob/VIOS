# Plain-text VIOS console

Run `.venv/bin/python tools/vios_cli.py` to attach to the shared harness at
`127.0.0.1:8765` (`--port` selects another loopback TCP port). The resident
[harness](vios-harness.md) should already own and connect the serial session.
The client sends MCP initialization and `connect`, then accepts Forth directly;
it has no serial-port, deployment or firmware-upload code. Closing the client
leaves the shared harness and board session intact.

Enter up to 235 printable ASCII bytes per source line. Unfinished definitions
change the prompt to `compile> `; finish with `;` or use `:abort`. Local commands
are `:help`, `:status` (resources), `:words`, `:await`, `:input HEX`, `:abort`,
`:reboot`, `:recover` and `:quit`. `:reboot` requires a synchronized idle prompt and explicitly
restarts resident firmware. While execution is pending, new source is blocked:
for example, after `KEY .`, use `:input 41` and then `:await` to supply `A`.
`:input` accepts 1–16 bytes; hex `1D` is reserved. Failed or uncertain operations
are never retried automatically; `:await` can resume a known pending evaluation,
while `:recover` explicitly abandons pending, compiling or uncertain execution
and restarts through the independent resident supervisor. **Unsaved RAM work is
lost**; persisted SD boot sources remain the startup authority. It uses the same
raw serial connection, sends the framed restart command once, and requires its
acknowledgment before a fresh empty attach. A missing acknowledgment leaves the
session uncertain; it never triggers an automatic retry. `:recover` is also
available after a failed board attach when the TCP harness remains reachable.
Normal `:reboot` does not override pending or uncertain state.
`--timeout` sets the evaluation/await deadline
(default 15 seconds); `--rpc-timeout` sets the TCP response deadline (default
130 seconds, allowing resident boot). No JSON input is needed.

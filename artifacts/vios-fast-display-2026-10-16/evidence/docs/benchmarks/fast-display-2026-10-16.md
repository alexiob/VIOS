# VIOS fast display — Agon Light 2, 2026-10-16

The new renderer uses the ESP32's viewport scrolling, sends fewer VDU commands,
and uses a bounded native UART writer at a matched internal rate. The tested
1,152,000-baud pair is faster than 384,000; USB remains at 115200. The eZ80 still
owns the live model and both screen shadows. No second ESP character grid or
new platform-independent VM dispatch layer was added.

## Device measurements

Physical Agon Light 2, eZ80 18.432 MHz, pinned VDP 1.04/FabGL, mode 1:
640×480, 80×60 cells, four colours. Timed output goes to VGA only. Console
echo, result records and host round trips are outside the measured intervals.

`WB-COMPOSE` uses the 288 kHz CPU timer without putting a clock value on the
inspected data stack. Native output time accumulates per bounded callback.
`WB-FLUSH-FENCED` additionally waits for an ordered framebuffer character query
before its end timestamp; it includes a cheap second composition and the fence.
Do not read its value as wire time alone. Ordinary `WB-FLUSH` retains its
previous drain contract. Full composition remains below the 16-bit timer wrap.

Plain-source fixture: 1,969 bytes, 50 comment lines plus a definition. Full
paint: seven samples; each edit/scroll: eight alternating operations; unchanged:
three samples. All figures below are medians, including initial cache misses.

| Operation | CPU composition ms | Native output ms at 1.152M | Fenced flush ms at 384k | Fenced flush ms at 1.152M | Display bytes at 1.152M |
|---|---:|---:|---:|---:|---:|
| Full repaint, already composed | 125.715 separately | 177.535 | 232 | 202 | 2255 |
| Unchanged | 1.278 | 0 | 3 | 3 | 0 |
| Character insertion/deletion | 7.851 | 3.052 | 9 | 9 | 36 |
| Newline insertion/deletion | 12.486 | 5.936 | 16 | 14.5 | 80 |
| Source scroll | 5.995 | 3.286 | 13 | 11 | 57 |
| Full-width source scroll | 6.786 | 3.160 | 10.5 | 10 | 57 |

Composition and fenced flushing are measured separately: for example, the
source-scroll path is about 17 ms combined, not 11 ms including composition.
Full composition plus fenced painting is about 328 ms. Actual interactive input
also has scheduling and keyboard repeat latency; these are not end-to-end key
latency measurements. The full-width improvement is small in these samples;
do not extrapolate a large general graphics speedup from it.

The previous display-only baseline measured 560 ms/4098 bytes for a precomposed
full repaint and 52.5 ms/307 bytes for a source scroll. It used the same mode
and source family but a different dictionary/inspector state. Full repaint
comparisons therefore include changed command encoding and scene content,
not just UART rate. The old native source composition was 19.323 ms and newline
composition 25.264 ms. The new values are 5.995 and 12.486 ms respectively.

The mixed tab/CRLF fixture also passed at 1.152M: source composition 6.755 ms,
newline composition 12.495 ms, fenced scroll 11 ms, fenced full-width scroll
10 ms, fenced newline 15 ms, full composition 139.611 ms. Previous mixed
composition measured 34.476 ms for scrolling and 39.816 ms for newline edits.

### Cold and warm scrolling

Two reserved VDP buffers, `0xFA00` and `0xFA01`, cache upward/downward viewport
scripts. A cache miss clears the old buffer before writing its 19-byte payload
and calling it: 39 display bytes. A warm call is six bytes. The payload
explicitly establishes colours, performs the pinned firmware's eight-pixel
movement, and restores the full viewport. Geometry changes and renderer reset
invalidate the relevant cache.

The plain-source normal-scroll first two samples cost 92 and 55 bytes including
glyph/cursor repairs, versus 59 and 22 bytes for the corresponding warm
directions. The six warm samples have a 40.5-byte median and 10.5 ms fenced
median. Full-width warm samples have a 9.5 ms fenced median. These are
alternating directions with different exposed-line content, not a single fixed
cost per scroll. Raw scripts remain the fallback when the output budget cannot
hold a cache transaction; no incomplete VDU command is emitted.

## Implementation and malleability

- Native TX accepts at most 32 bytes, checks CTS/TX readiness, records
  clear-on-read overruns, and returns partial/zero progress. RX service and
  backpressure remain between bursts. Idle painting stays bounded to 128 bytes.
- The compositor shifts source caches, desired cells and painted cells together,
  then projects exposed/damaged/cursor rows. Dirty or ambiguous regions use the
  normal reconstruction path. Row copies replace expensive per-cell moves.
- Colour/cursor state is reused within a batch; short clean gaps are repainted
  when cheaper than another positioning sequence. Cross-batch raw VDU state is
  not assumed.
- Only the exact original native default painter uses the direct path. Replaced
  deferred hooks and shadowed names retain observer isolation and instruction
  budgets; Forth remains able to replace rendering behavior.
- `WB-ZOOM 0|1` / `WB-ZOOM?` and local **Ctrl+Space** toggle an 80-column editor;
  source, selection and VM state survive. Browser/inspector stay above it.
- `WB-REPAINT` reconstructs a damaged screen without the normal VGA mode change
  when dimensions/mode agree. `WB-REDRAW` retains the full mode-reset recovery
  route. A fence timeout is reported, not accepted as a completed frame.

The buffer IDs are reserved to VIOS. Raw VDU code that changes them or damages
the viewport must call `WB-REPAINT`/`WB-REDRAW` to invalidate and reconstruct
renderer state. The new scroll operation lives in the display driver API;
the portable model keeps its ordinary repaint fallback. The C reference and
both linked Forth engines remain tested.

## Footprint

| Item | Previous accepted | New | Change |
|---|---:|---:|---:|
| Resident binary | 105706 B | 111301 B | +5595 B |
| CPU BSS | 254031 B | 255060 B | +1029 B |
| ESP app0 binary | 663952 B | 669568 B | +5616 B |
| Gap from BSS end to reserved stack | 94919 B | 88295 B | −6624 B |

New BSS ends at `0x99719`; the reserved 4 KiB stack begins at `0xAF000`.
The expanded 38×80 source cache accounts for 988 B of the BSS increase.
ESP buffered scripts allocate two 19-byte payloads plus existing VDP container
overhead; no 9600-byte ESP screen grid is allocated. USB pacing adds no ring.

## USB integrity investigation

The initial unpaced 384k candidate failed a large screen inspection. A repeated
view-only test failed again after nine complete 60-row reads. Raw logs show
contiguous encoded bytes missing before host decoding (five bytes in the first
failure, 103 in the second). Correct copies replay through every possible host
read split. Because the ESP encodes/CRCs the CPU console content first, this
locates loss on the USB hop; it does **not** identify whether the ESP UART,
CH340 or host driver caused it.

Console frames now drain UART TX and yield one FreeRTOS scheduler tick while
holding the existing USB mutex. The mutex releases between frames; control and
event frames have no extra delay. The pinned `flush()` is TX-only. A scheduler
tick is not a guaranteed full millisecond of silence. Normal 128-byte console
records serialize control for roughly 12 ms of wire time plus that tick;
maximum frames bound this to about 22 ms plus a tick. This trades a little bulk
USB throughput for a verified scheduling boundary, without adding display wire
bytes. It is a tested mitigation, not a proven physical root-cause repair.

With pacing, 384k passed 30 consecutive full inspections: 639540 console bytes
in 91.97 seconds, followed by debug-register/header CRC checks and resumed
evaluation. Failed candidates, raw logs and successful qualification remain in
the evidence; none was silently retried or accepted after corruption.

At 1.152M another 30 full inspections passed (640680 console bytes), along
with an actual CLI calculation, all 48 MCP tools advertised by the real owner,
debug pause/resume, and CRC-checked RAM content equal to the first 256 bytes of
the accepted image. A real Forth wrapper around the default painter preserved
DS value 123 and BASE 16; TOKEN and DIRECT rendering both completed. Maximal
235-byte CLI lines, repaint/zoom interleaving and a pending `KEY` continuation
also passed in both rate benchmarks. Aggregate console/FIFO/keyboard drops
remained zero. These are bounded qualification runs, not a long-duration soak.

## Recovery qualification

All 25 fault-recovery checkpoints passed at the final 1.152M rate. An explicit
supervisor restart recovered a pending Forth `KEY`, then a deliberately infinite
VDP buffered-command loop. Independent USB boot/identity queries and halted CPU
register/header access still worked during the graphics wedge. The boot watchdog
reported a reset after 30.184 seconds and reattached through a fresh wrapper
without an additional forced restart. Transient RAM markers disappeared, a
fresh VM computed 42, and confirmed slot A / CLEAN / generation 7 survived.

The frozen historical `GO`/`RESUME` acknowledgement strings remain compatible
with existing recovery clients; actual internal baud is in build metadata.
ESP bootloader, partition table, app1, eZ80 flash and SD boot selection were not
written. The previous working app0 was freshly read and matched its saved
SHA-256 before application-only updates. The original restore image and both
matched rate variants are retained.

A three-minute physical-keyboard observation ran 328 status samples with
redraw/maximal CLI traffic and zero drop growth. It observed no external cursor
movement or zoom, and the requested operator action was not received. Its
`failed` assertion records this unmet manual test precondition, not a proven
keyboard regression. Physical keyboard responsiveness remains **unverified in
this run**; linked parsing/routing/Ctrl+Space tests passed. The workbench is left
open for the pending physical check after restoration.

## Restoration and release

All four retained live modules and the exact 1969-byte dirty `WBM1P` draft
match their pre-deployment sources. BASE is decimal, engine DIRECT, DS empty,
and confirmed startup remains A / CLEAN / generation 7. Original cursor,
selection, focus and closed state were first restored and checked. The editor
was then intentionally reopened with editor focus, so the pending keyboard
check can use VGA immediately. The shared TCP/serial owner remains available.
This is source/module restoration, not an old RAM-image restore: unretained
ad hoc entries disappeared across the deliberately destructive recovery tests
(dictionary 86 before, 81 after).

The [new release manifest](../../artifacts/vios-fast-display-2026-10-16/manifest.json)
records both matched rate variants, the default 1.152M pair, original app0
restore image, complete inspectable source snapshot, evidence and archive
hashes. The [restore instructions](../../artifacts/vios-fast-display-2026-10-16/RESTORE.md)
retain application-only offset `0x10000`. The previous frozen release's 25
manifest-listed files were hash-checked unchanged. Verification is read-only:

```sh
python3 tools/freeze_fast_display.py --verify-only
```

## Reproduction and evidence

Use the single resident harness; benchmark/control clients never open a second
serial session. Back up live drafts before these commands: the benchmark
replaces its disposable RAM draft, and recovery deliberately loses RAM.

```sh
make vios-resident VIOS_INTERNAL_BAUD=1152000
VIOS_INTERNAL_BAUD=1152000 /Users/alex/.platformio/penv/bin/pio run -d firmware/esp32-vios -e esp32-vios
.venv/bin/python tools/vios_fast_display_bench.py --baud 1152000 --samples 7 --output results.json
.venv/bin/python tools/vios_fast_display_controls.py --baud 1152000 --output controls.json
```

Build CPU then ESP sequentially: the embed script rejects a mismatched rate
sidecar. Running host tests concurrently can relink the default-rate CPU.
The 384k variant uses the same two build settings with `384000`.

- [384k benchmark](fast-display-384000-2026-10-16.json)
- [1.152M benchmark](fast-display-1152000-2026-10-16.json)
- [Mixed-source benchmark](fast-display-1152000-mixed-2026-10-16.json)
- [1.152M inspection, CLI and MCP controls](../bring-up/vios-fast-display-controls-1152000.json)
- [Recovery fault tests](../bring-up/vios-fast-display-recovery-1152000.json)
- [Physical keyboard observation](../bring-up/vios-fast-display-keyboard-1152000.json)
- [Automated acceptance and explicit manual-test limit](../bring-up/vios-fast-display-acceptance.json)
- [Exact restoration](../bring-up/vios-fast-display-restored.json) and
  [live editor readiness](../bring-up/vios-fast-display-live-ready.json)
- [Earlier display-only baseline](render-display-only-2026-10-16.md)
- [Independent review and USB addenda](../bring-up/vios-fast-display-review.md)
- [Host suite](../bring-up/vios-fast-display-host-tests.txt) and
  [146 linked native probes](../bring-up/vios-fast-display-engine-tests.txt)
- [Execution ledger](../bring-up/vios-fast-display-progress.md)

# VIOS full VDP firmware

This project retains the upstream VDP 1.04 VGA, graphics, sprites, fonts, PS/2
keyboard and audio implementation, and adds a framed USB control path
and an embedded eZ80 RAM bootstrap. It replaces the temporary RAM-only ESP32
bridge for this profile. The existing `tools/esp32-ram` recovery loader is
unchanged.

## Build

From the repository root, first build the intended integrated CPU image, then
build this firmware. `make vios-resident` also builds `build/agon-stage0.bin`,
`build/agon-crc.bin` and `build/agon-flash.bin`, which `scripts/embed_vios.py`
embeds and requires; `make viosboot` builds the SD boot stub
`build/viosboot.bin` (installed on the card, not embedded):

```sh
make vios-resident
make viosboot
~/.platformio/penv/bin/pio run -d firmware/esp32-vios -e esp32-vios
```

Resident and ESP builds default to an internal rate of 1,152,000. To select
384,000 for both ends, run:

```sh
make vios-resident VIOS_INTERNAL_BAUD=384000
VIOS_INTERNAL_BAUD=384000 ~/.platformio/penv/bin/pio run -d firmware/esp32-vios -e esp32-vios
```

Use `1152000` in both commands for the default rate, or `115200` for the
historical internal rate. Make rebuilds the resident initializer when the rate
changes, including consecutive builds within one second. The CPU binary's
`.baud` sidecar must accompany a custom `VIOS_IMAGE`; the ESP pre-build rejects
a missing or mismatched rate before embedding it. Historical raw monitor,
standalone Forth and storage-probe builds keep their 115,200 default.

The first PlatformIO run may download the pinned toolchain/framework packages.
The complete vendored source is available locally; the build uses it directly.

- `vendor/agon-vdp`: `breakintoprogram/agon-vdp` commit
  `61888abd51a04ce6acf282df5e8300ed7125c434` (v1.04).
- `vendor/vdp-gl`: `avalonbits/vdp-gl` commit
  `48a0b9af72d13180b6962a6429a2ce85ca4954e8` (1.0.3).
- PlatformIO `espressif32@6.3.0`, Arduino ESP32 2.0.9,
  GCC `8.4.0+2021r2-patch5`, ESP32Time 2.0.0.

The archived source URLs and archive SHA256 values are in
`vendor/manifest.json`. A pre-build script validates and embeds
`build/vios-resident.bin` into flash-backed constant data; it does not reserve an
extra copy in ESP32 SRAM. The generated `include/vios_image.json` records the
exact embedded CPU path and bytes. Set `VIOS_IMAGE=/absolute/path/image.bin`
to explicitly embed another validated RAM image. The raw reference
`build/vios.bin` remains separate. A successful image build writes
`build-manifest.json` with firmware/CPU image hashes, dependencies and build
source hashes. Rebuilding is necessary after any CPU image change.

Output: `.pio/build/esp32-vios/firmware.bin`. Its maximum is one
**0x140000-byte application slot** (`app0` or `app1`). The build also creates
bootloader and partition binaries as normal PlatformIO intermediates; they are
never installed. Only the hardware-owning agent deploys reviewed firmware.
Install it A/B with `tools/vios_firmware_ab.py` (Update 2026-10-21): it backs up
`otadata` and the target slot, writes the inactive slot on trial, and the
firmware confirms itself once the eZ80 reports healthy or falls back to the
previous slot (see the manual's
[Installing](../../docs/manual/VIOS-MANUAL.md#installing) chapter). The
bootloader and partition table are never written, and normal installs never
write the eZ80 flash (it holds MOS 3.0.2, written once on 2026-10-19).

## Frozen wire protocol

USB UART0 operates at **115200, 8N1** independently of the selected CPU/VDP
UART2 rate. The internal link defaults to **1152000, 8N1**; `VIOS_INTERNAL_BAUD`
selects 115200, 384000 or 1152000 for matching resident and ESP builds.
UART2 uses the original GPIO2 TX / GPIO34 RX / GPIO13 RTS / GPIO14 CTS pinout.
It enables both RTS and CTS hardware flow control. CPU sends obey ESP RTS,
and ESP sends obey the eZ80 RTS receive windows. Hardware acceptance of each
selected rate is measured separately; USB control and ZDI recovery do not
depend on that rate. The build manifest records USB and internal rates
separately. GO/RESUME retain their frozen `USB/UART bridge 115200` recovery
acknowledgements for shared raw/resident host compatibility; that historical
text identifies the USB transport rather than the selected internal rate.

Each USB frame is COBS encoded and ends with `00`. The decoded bytes are:

| Field | Encoding |
| --- | --- |
| version | `01` |
| channel | `01` console, `02` control, `03` status/event |
| sequence | unsigned 16-bit little endian |
| payload length | unsigned 16-bit little endian, 0..240 |
| payload | exact byte count above |
| CRC | IEEE CRC32, little endian, over all preceding decoded bytes |

Maximum decoded length is 250 bytes; maximum encoded frame including delimiter
is 252 bytes. CPU console output and status streams use sequence zero.
Host channel 1 sequence zero is raw KEY input. Nonzero sequence is an atomic
source-line transaction: concatenated payload starts with total line length
(u16 little endian, 1..256), then CRC32 (u32 little endian), then exactly that
many line bytes including the final CR. Fragments share one sequence and
expire after two seconds. The firmware buffers the entire line and rejects
interleaving, invalid length/ASCII/CRC, embedded terminators and paused input
without forwarding a prefix. It replies on channel 1 with the same sequence
`LINE OK\r\n` only after verification, then forwards FE packets to the CPU.
The host never retries after an uncertain acknowledgement. A valid complete
line is acknowledged only after acceptance into an eight-record bounded queue;
full queues return `LINE ERR queue full` and execute nothing. UART2 packet
transmission belongs to the VDP task, so USB control never interleaves bytes
with a VDP response. Resident tagged
source additionally carries the CPU-checked source CRC, protecting the UART
hop as well as USB. The source limit is 235 bytes for this profile. A control response
uses the request sequence on every response fragment. Unknown versions,
invalid channels, bad COBS/length/CRC and oversized frames are discarded.
A partial USB frame expires after two seconds and waits for the next delimiter.
ROM boot text is separated from framed output by an initial zero byte.

Channel 2 is a bounded ASCII command stream. A request ends with LF and may
span multiple frames sharing its sequence. Maximum accumulated request and
reply are 600 bytes; fragments from another sequence are rejected while one
command is incomplete. Partial commands expire after two seconds. Replies use
CRLF, fragmented into <=240-byte frames with the same sequence.

Supported control commands preserve the existing host adapter's vocabulary
(this list covers the original commands; later ones such as `FR`, firmware
rollback, are in `include/vios_bridge.h`):

- `B`: read-only `BOOT STARTING`, `BOOT READY`, or `BOOT HALTED` status.
- `I`: ID/revision/status; `2`: acknowledges the fixed conservative ZDI timing.
- `H`: halt then reply with register snapshot; `S`: snapshot while paused.
- `J`/`K`: instruction-store diagnostics in the separate probe profile only;
  unavailable in production.
- `D`: return the exact CS0 map verified by native stage0, while paused. It uses
  cached native readbacks and injects no CPU instructions into a running image.
- `Vaaaaaacccc`: read 1..256 bytes in `0x040000..0x0BFFFF`, including CRC.
- `WaaaaaaccccXXXXXXXX<datahex>`: CRC-checked RAM write, 1..256 bytes only in
  `0x040000..0x0AEFFF`; the host independently reads back the result.
- `X`: enter the RAM image at `0x040000`, validating its MOS header.
- `Y`: resume the existing PC and UART state. It does not clear either UART FIFO.
- `Z`: explicit ESP restart, acknowledging `RESTARTING` and flushing USB first.
  This cold boots the flash-embedded CPU image without a host RAM upload.

Normal CPU-to-VDP bytes still go through the original VDU state machine.
Private extensions are recognized only at a VDU command boundary, so bitmap,
font and audio payload bytes cannot accidentally become console/control data:

| Direction | Bytes and meaning |
| --- | --- |
| CPU → ESP | `23,0,E0,len,payload` emits up to 240 console bytes as USB channel 1 and into the ESP's own console view. |
| ESP → CPU | `FE,len,payload` delivers USB console bytes, alongside standard VDP return packets. |
| CPU → ESP | `23,0,E1` requests monotonic ESP32 `millis()` as return packet `FD,4,<LE32>`. |
| CPU → ESP | `23,0,E2,0` arms a 30-second CPU boot deadline; flag 1 disarms and reports ready. |

The portable codec is shared with host tests at `include/vios/wire.h` and
`drivers/vios-wire/wire.c`. JSON/MCP/TCP remain host services.

Legacy unframed USB keyboard injection is superseded by channel 1; PS/2 input
is retained. Legacy console mirroring, if explicitly enabled by VDU, emits
framed channel 1 output. The old raw USB Intel HEX entry reports an explicit
status error in this profile; framed bounded `W`/`V`/`X` replaces it. Allowing
that old raw serial reader to run would steal COBS input. Its upstream source
is retained, as is the separate recovery RAM loader. Ordinary VDU commands,
including graphics and audio, remain built. The legacy VDP updater entry is
disabled in this profile; firmware replacement is an explicit host operation. This board profile
initializes only `PS2Preset::KeyboardPort0`. PS/2 mouse access is disabled:
GPIO26/27 belong exclusively to ZDI, and mouse enable reports an explicit
unsupported event. Legacy terminal entry also reports unsupported because its
second UART reader would steal multiplex packets. VDU 14 paging continues
without waiting for physical SHIFT; the VIOS UI owns paging. CPU VDU parsing
and keyboard transmission stop while debug-paused, preserving any incomplete
VDU packet until `Y` resumes. VGA rendering and audio remain active.

## Cold boot and recovery

Update (2026-10-21): this section describes the dev boot with the ZDI wire
fitted. Without the wire the ESP reports `BOOT user mode`, lets the eZ80 run
MOS, and MOS's boot file runs the SD stub `viosboot`, which loads the
embedded resident over the UART (or the SD copy). The ESP boots into the GUI
desktop (mode 0, 640×480, 16 colours). See the manual's
[Untethered boot](../../docs/manual/VIOS-MANUAL.md#untethered-boot) section.

After initializing VDP graphics/audio/keyboard, setup skips the old indefinite
MOS handshake. GPIO26/27 use the tested ZDI transport (1 us edges since 2026-10-19, `VIOS_ZDI_EDGE_US`; 10 us before) and
whole-transaction critical sections. Boot verifies CPU ID/revision, halts,
selects ADL and injects the single-byte DI. The original pure IN0 discovery
reads RAM_ADDR_U/RAM_CTL to locate enabled on-chip SRAM. Hardware evidence in
`docs/bring-up/vios-zdi-discovery.serial.jsonl` showed the first result after
changing the IN0 port can be the previous A value. Discovery therefore primes
each port and requires its next two readings to match; all six samples are
reported before any memory write. Disagreement aborts without writing SRAM.
It then clears IS0 to NOP.
The discovered 8 KiB window must not overlap the VIOS external payload/stack.

The 46-byte position-independent `build/agon-stage0.bin` is written into that
internal RAM and read back independently. Its metadata at base+256 is cleared
and verified before entry. With HL pointing there, native CPU instructions
configure CS0, read back `04/0B/08/01`, write those bytes and completion magic
`A5` last, then spin. After a 2 ms run, ZDI halts and verifies all five bytes.
A channel-3 event reports `NATIVE RAM <base> CS0 04 0B 08 01 DONE A5`.
Only then is the main VIOS image written to external `0x040000` in <=256-byte
chunks, independently read back and compared, and entered. Stage0 source,
generated header and image hashes are recorded in the manifest. Boot does not
use ZDI LD/OUT instruction injection to configure external RAM; normal debug
map queries use the cached native result.

Identity, internal RAM, code-readback or completion/map errors stop before
executing unverified VIOS code and leave USB control available. CPU flash is
neither erased nor programmed. This path uses the existing/reset on-chip RAM
mapping (including the mapping initialized by the installed MOS before halt);
it makes no MOS calls and does not reset the CPU through ZDI. Fully resetting
the CPU before bootstrap remains a separate unverified step.

Cold boot and fresh `X` arm the CPU boot deadline. `E2,1` from successful
startup or a safe recovery REPL disarms it and resets the failure count.
`B` reports ready only after this explicit CPU health message; the earlier
`VIOREADY` event reports RAM verification, not completed CPU/SD startup. Host
attach waits up to 120 seconds for `B` readiness before probing the REPL.
A timeout halts the CPU and restarts the ESP, clearing any stuck VDP parser
before the embedded image is loaded again. CPU-side SD startup then chooses
its confirmed source slot. The failure counter uses RTC_NOINIT storage with
magic/count/complement validation, retained across software resets; power,
external and brownout resets clear it. Explicit host `Z` clears it as well. Three consecutive failed boot
attempts total leave the CPU halted with USB debug available. Explicit debug
pauses suspend the deadline until `Y` resumes; healthy long-running VM programs
have no runtime watchdog here. CPU/SD trial-confirm-revert policy belongs to
the VIOS startup code, not the ESP transport.

ZDI register/memory tools preserve CPU PC. Normal debug map queries do not
inject instructions; the instruction-store probes are separate diagnostics.
SD remains physically connected to the eZ80 SPI bus.

## Independent headless recovery

A dedicated USB/control task starts immediately after framed USB initialization,
before graphics, audio or CPU bootstrap. Until an atomic ownership handoff, it
accepts only `B` (STARTING) and `Z`; other control and console input is rejected.
It does not touch ZDI or boot-health fields while setup owns them. Thus explicit
ESP restart remains available even during a stuck graphics initializer or the
bounded initial RAM upload. After bootstrap returns, the task on core 0 at
priority 3 owns the only UART0 reader, all ZDI access and the boot watchdog. The main VDP task alone owns UART2 parsing and transmission. A bounded
8 × 256-byte queue carries accepted console records to it. USB writers share a
mutex over each complete COBS frame, so console/status/control producers cannot
corrupt one another. A second mutex covers each UART2 packet through physical
TX completion; halt waits for this bounded packet boundary. Legacy raw USB,
mouse and updater entry paths cannot create another reader or ZDI owner.

CPU pause state and VDP parser-busy state use atomics. Boot-health messages
cross through an atomic mailbox and are applied by the control task. Parser
byte and bulk reads preserve their position and exclude time spent paused from
their timeout. `Y` resumes the existing CPU and packet stream. Fresh `X` refuses
an active parser or pending UART2 input with an explicit error directing the
host to `Z`; it does not release a new CPU into an old payload parser.

Buffered VDU loops and renderer/audio waits may still occupy the VDP task and
delay ordinary console input. They do not own the independent USB/control task.
`H`, register/RAM tools and explicit `Z` remain serviceable while that task is
busy. `Z` acknowledges, flushes USB, clears the failure count and restarts the
ESP, abandoning the current parser and CPU job. The watchdog also uses this
whole-ESP recovery path, rather than recursively booting a new CPU inside an
old parser. Full debug access begins after initial bootstrap; restricted B/Z begins before
VDP setup. Both require normal FreeRTOS/UART interrupt operation; they do not claim recovery from arbitrary
hardware failure or globally disabled interrupts. Queue-full input is rejected,
and a VDP TX lock that fails to clear makes halt return an error while `Z`
remains independent of that lock. Dynamic costs include the 6144-byte control
task stack, queue storage and mutexes, beyond static linker RAM figures.

## Evidence and limits

The untouched pinned VDP baseline compiled locally before integration: 36852
bytes static RAM and 614305 flash program bytes. Initial integrated builds
remain comfortably inside the preserved application slot; exact current image
size and embedded hash are in the build manifest. These linker numbers exclude
runtime framebuffer/DMA, PSRAM and task/driver allocations. The USB RX driver
adds a 2048-byte buffer; usable graphics modes still require hardware checks.
No VGA/audio/keyboard/cold-boot or rollback success is inferred from compiling.

`tests/test_wire.py` builds the exact portable codec with UBSan and validates
the standard CRC vector, all payload-byte values, zero-rich/maximal payloads,
malformed/truncated/corrupt frames, capacity bounds and independent Python
header/CRC decoding. An additional ASan/UBSan test executes the actual
low-level GPIO ZDI driver, decodes its register and instruction-store bit
transcripts, and checks transaction exclusion and prior interrupt-state restoration. A second standalone ASan/UBSan probe executes the actual
firmware bridge against byte/register-level dependencies. A bounded interpreter
executes the exact native stage0 bytes and checks internal RAM/code boundaries,
relocation, readbacks, strict mapping/magic completion before external writes,
missing completion, corrupted code, bad/overlapping/disabled mappings, and
embedded image readback failure. Other checks cover PC/AF preservation, bounded writes/CRC rejection, fragmented
600-byte replies and request-sequence isolation, paused boot deadlines and the
three-failure halt policy, including retained-counter validation. Native tests
exercise a saturated input queue and stuck parser while control read/write and
restart still progress, and concurrent USB producers preserve complete COBS
frames. It also checks atomic line fragmentation, missing
chunks, corrupt line CRC, interleaving, paused rejection and restart ACK. Board flash/boot, shared USB control with rendering, SD
startup and rollback tests are performed separately by the hardware owner.

Upstream notices and full sources are retained. Agon VDP is MIT; vdp-gl/FabGL
carries its GPL/commercial licensing terms in `vendor/vdp-gl/LICENSE`. The
VIOS glue/codec is MIT. Distribution of the combined firmware must retain the
applicable upstream terms and corresponding sources.

## Minimal ZDI diagnostic profile

`pio run -d firmware/esp32-vios -e esp32-vios-probe` builds a separate diagnostic
application at `.pio/build/esp32-vios-probe/firmware.bin`. Its manifest is
`build-manifest-probe.json`; `embedded_boot_enabled` is false. The generated CPU
image remains a recorded build input, but the diagnostic does not bootstrap it.
After framed USB setup, this profile only configures ZDI pins, halts the CPU,
selects ADL and injects DI. It skips UART2, PS/2, audio, VGA, CS0 writes and RAM
upload. Its main loop only polls USB controls. `B` reports halted; `I/H/S/D/J/K`
provide identity, CPU state, map reads and burst-versus-split instruction tests.
This profile isolates the Arduino GPIO/ZDI path from VDP subsystem startup.
The normal profile remains `esp32-vios`; no host RAM upload is implied by either
building or flashing the diagnostic. CPU mapping stays unchanged until an
explicit control operation or a later normal firmware boot.

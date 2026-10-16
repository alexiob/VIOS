# VIOS native SD/FAT32 storage

This subsystem provides synchronous file persistence without MOS or VDP calls.
The filesystem is vendored ChaN FatFs R0.14b, backed by a pure-C SDHC/SDXC SPI
protocol driver and small eZ80 I/O routines. Its public interface is
`include/platform/storage.h`. Hardware integration and board verification are
owned by the main milestone; the checks below do not open a serial port or any
physical disk.

## Board wiring and controller

The Rev B netlist and MOS SPI source agree on these onboard connections:

| Signal | eZ80 port / package pin | microSD socket pin |
| --- | --- | --- |
| Clock | PB3 / U1 pin 91, through R5 | 5 |
| MOSI | PB7 / U1 pin 95, through R6 | 3 |
| MISO | PB6 / U1 pin 94, through R43 | 7 |
| Active-low card select | PB4 / U1 pin 92 | 2 |

PB2/U1 pin 90 is the SPI peripheral's `/SS`, exposed as `GPIO_CS` on GPIO
header pin 25. It must remain high in master mode: driving it low causes a mode
fault. The SD socket's select is the separate PB4 signal. The SPI bus is also
exposed on GPIO/UEXT; this initial driver owns it exclusively while operating.

`arch/ez80/spi.s` uses GPIO DR/DDR/ALT1/ALT2 ports `0x9A..0x9D`, SPI BRG low/high
at `0xB8/0xB9`, control/status at `0xBA/0xBB`, and transmit/receive at `0xBC`.
It clears power-down bits 5 and 3 in `CLK_PPD1=0xDB`, preserving unrelated bits.
PB2/PB4 are GPIO outputs initially high; PB3/PB6/PB7 select their peripheral
function. Other port-B pins remain unchanged. `SPI_CTL=0x30` enables a polling
master with CPOL=0/CPHA=0. SPI status bits 7/6/4 indicate completion/collision/
mode fault; errors are reported and transfer polling has a finite iteration cap.

At 18.432 MHz, the SPI rate is `clock / (2 × divisor)`. Initialization uses
divisor 32 (**288 kHz**), then divisor 3 (**3.072 MHz**) after card negotiation.
The eZ80 specification requires master divisors of at least 3. MOS's existing
SPI source starts directly at divisor 3; VIOS deliberately starts more slowly.
The controller register definitions are also present in AgonDev's
`include/ez80f92.h` and `.inc` files.

Local primary references:

- `docs/research/sources/AgonLight2/HARDWARE/AgonLight2_Rev_B/AgonLight2_Rev_B.net`,
  nets MICROSD_CLK/CS/MISO/MOSI, package definitions and R43-to-socket net.
- `docs/research/sources/AgonLight2/DOCUMENTATION/EZ80F92.txt`, pages 37 and
  130–139: power gating, PB2 constraint, baud equation, SPI flags/registers.
- `docs/research/sources/agon-mos/src/spi.asm`, pin configuration and mode.

## Layer boundaries and limits

`drivers/sd-spi/sd.c` initializes SD v2 high-capacity cards using startup clocks,
CMD0, CMD8, CMD55/ACMD41, CMD58 and CSD/CMD9. It checks the voltage/echo, OCR
power/capacity bits and CSD format. Capacity and every block address are native
**32-bit values**, independent of the 24-bit Forth cell. SDSC, MMC and capacities
requiring a 2^32-sector count are explicitly rejected. There is no format,
partition, erase or trim operation.

Reads use CMD17 with a 512-byte data block and verify CRC16. Writes use CMD24,
send data plus CRC16, validate the response token, and wait for the busy interval
to end. Commands/response polling, data-token waits, initialization and write
busy waits have finite limits. `sd_sync` waits for the card to finish. Card
transport errors invalidate readiness; reinitialize before retrying.

Timeouts read the existing free-running timer 0 without reconfiguring it. A
32-bit accumulator sums successive 16-bit downcounter differences, allowing
500 ms write waits and 1 s initialization waits across counter wraps. Polling
also has finite fallback counts. Timer initialization remains the caller's
responsibility; no interrupt tick is required. Per-byte SPI fault/timeout exits
are bounded even if the peripheral stops responding.

`drivers/fatfs/diskio.c` presents one block device with sector bounds and real
status/sync operations. FatFs discovers the existing MBR partition; the service
then requires FAT32. The prepared Agon card is recorded in the bring-up ledger
as MBR, a 31 GB FAT32 `AGON` partition, with existing bootstrap files. This code
neither repartitions the card nor assumes those files may be overwritten.

FatFs uses one static volume, one static file object and one shared 512-byte
sector window. Long names, exFAT, RTC, heap and reentrancy are disabled. Paths
are bounded to 127 printable ASCII bytes and component names use 8.3 format.
The wrapper is synchronous and must not be called concurrently. Upstream's
integer assumptions require care on this CPU: `UINT` is explicitly 32-bit in
the VIOS C99 port; disk offsets, FAT arithmetic and file lengths stay 32-bit.
See `drivers/fatfs/PROVENANCE.md` and the retained source licenses.

## File API behavior

Every operation returns 1 for success, 0 for failure; `storage_error()` provides
a fixed string or the underlying SD error. `storage_init()` mounts without
modifying the filesystem. It does not create a directory automatically. The
caller can create `/VIOS` with `storage_mkdir`; an existing directory succeeds,
while an existing regular file fails.

`storage_read_file(path, buffer, capacity, &size)` sets size to zero before any
work and on every failure. It rejects an oversized file before copying, then
reads the exact file length. An I/O failure can leave partial buffer contents;
size remains zero and those bytes must not be consumed. Empty files permit a
null buffer with capacity zero. Write/sync/read failures invalidate the mount
when transport or filesystem state is unreliable; `storage_init()` is the
explicit recovery boundary.

`storage_write_file` creates or truncates the **named path**, checks short
writes, synchronizes and closes. It is not power-fail atomic. The caller owns
overwrite policy: use a fresh temporary filename, verify it, then use explicit
`storage_rename`/`storage_remove` for a staged save. FAT rename and replace
sequences are not a journal or a general atomic commit guarantee. Preserve
existing boot files and use the VIOS directory for milestone persistence.
`storage_sync`, `storage_mkdir`, `storage_rename` and `storage_remove` complete
the initial whole-file API. Native file handles can be layered on later with an
explicit bounded handle table.

Persist source text for `LOAD`/`SAVE`; native threaded dictionaries contain
absolute pointers and are not relocatable file formats. Source evaluation,
file-size limits appropriate to the resident compiler, deferred bindings and
save/reload policy belong to the Forth integration layer.

## Verification and integration

Run the host-only tests from the checkout root:

```sh
.venv/bin/python -m unittest discover -s tests -p test_storage.py -v
make test
```

The three tests exercise:

- **Actual FatFs on regular disk images**, under ASan/UBSan: MBR/FAT32 mount,
  preserved original boot file, multi-sector/cluster binary roundtrip,
  capacity rejection without copying, rename, truncation, empty file, directory
  idempotence, deletion, full-volume short write, FAT16 rejection and injected
  read/write/sync failures. Independent checks compare both FAT copies and the
  original file bytes. No disk-image formatter or mounted host volume is used.
- **Actual C SD driver against a byte-level card model**: command arguments,
  capacity, read/write contents, CRC rejection, data response rejection, stuck
  data token, busy timeout across timer wraps, initialization timeout and SPI
  failure. The model validates CRC16 against the standard `123456789` vector.
- **Actual assembled SPI disassembly semantics**: slow/fast divisors, GPIO
  preservation, active-low select, receive-store, status faults and finite poll
  exit. This caught an IN0 flag-clobbering select bug before hardware use. The
  instruction harness is not a cycle-accurate CPU/peripheral emulator.

Add these to the VIOS build (main agent owns Makefile integration):

```text
C:   boards/agon-light-2/storage.c
     drivers/fatfs/ff.c
     drivers/fatfs/diskio.c
     drivers/sd-spi/sd.c
ASM: arch/ez80/spi.s
```

The target also uses the existing `arch/ez80/timer.s` and its initialized timer.
All storage sources cross-compile with the project warning/error settings.
Standalone linking against pure compiler/string helpers resolves without MOS
or VDP symbols. Host checks establish logical behavior; physical card identity,
SPI behavior, read-only mount/list, preservation of existing contents, new-file
write/readback and save/reload recovery still require measured board evidence.

Primary upstream sources: [pinned MOS/FatFs source](https://github.com/AgonPlatform/agon-mos/tree/8336409351ee5314e02801a7b72a4f1bb5282519/src_fatfs),
[FatFs porting guidance](https://elm-chan.org/fsw/ff/doc/appnote.html), and
[SD Association specifications](https://www.sdcard.org/downloads/pls/archives/).

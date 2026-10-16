# Agon Light 2 hardware reference

Research date: 2026-10-16. This document distinguishes board documentation from observations of the connected unit. It is sufficient to plan first bring-up; it does not claim that every peripheral has been exercised.

## Connected unit: observed state

### Verified bring-up result, 2026-10-16

- eZ80 ZDI product ID `0007`, silicon revision byte `AA`; running status `18` (ADL/MADL), debug-halted status `98`.
- Full 128 KiB eZ80 flash read twice with per-block CRC32 and matching byte-for-byte results. SHA-256 `e396bfff189dfcd6c064f81349f4af9c80f4033c9015eb6ea12f870679eb6569`. Image contains `Agon Quark MOS Version `; exact MOS version is not yet established. Flash is enabled at zero (`FLASH_ADDR_U=00`, `FLASH_CTRL=28`); internal SRAM at `B7E000` (`RAM_ADDR_U=B7`, `RAM_CTL=80`).
- Full original ESP32 4 MiB flash backup verified; approved app0-only replacement with vendor VDP 1.04 completed. Bootloader, partition table and eZ80 flash were not replaced.
- SD was inventoried, backed up and reformatted with user approval to MBR/FAT32, 31 GB `AGON`, then populated with headless bootstrap files. It has been returned to the board.
- The C monitor has executed in eZ80 external RAM at `040000`, with a separate ESP32 RAM helper providing USB/UART bridging at 115200. Commands, line editing, overflow handling and RAM restart passed. External RAM CS0 registers: lower `04`, upper `0B`, control `08`, bus mode `01`.
- This proves the loaded RAM runtime and console. Independent cold boot, normal MOS/VDP console bootstrap, and the Forth VM remain future work. See [the runbook](../bring-up/ram-console.md).

### Initial observations, retained for diagnosis

- Host: macOS on arm64. `/dev/cu.usbserial-22430` exists.
- Initially occupied by `tio /dev/cu.usbserial-22430 -b 115200`, PID 9636. The user authorized taking it over. That process had already exited when checked again.
- Opening at 115200, 8 data bits, no parity, 1 stop bit succeeded. An initial listen and carriage return produced no bytes.
- After the user reset the board, the listener captured an ESP32 ROM boot banner and a Bulgarian GPIO diagnostic application. See [the raw boot log](serial-boot-2026-10-16.log).
- The diagnostic reports initialization complete, then errors in all-high/all-low GPIO tests. It names GPIO39, GPIO35, GPIO36, GPIO37, GPIO38 and GPIO27. This resembles a manufacturing test. Its identity, expected test fixture, and whether it also contains working VDP functionality are unconfirmed. The messages alone do not establish physical shorts or a faulty board.
- No MOS prompt or MOS/VDP version string has been captured. ESP32 boot output proves the host-to-ESP32 serial path; it does not prove eZ80 execution or a usable loader.
- User reports no display or keyboard attached, that the board was tested previously, and, correcting an earlier statement, **a microSD card IS installed**. At this initial observation its contents and filesystem were unknown.
- At this initial observation no target executable had been transferred and no flash or SD contents had been changed. Later authorized changes and successful execution are recorded above.

## Primary sources collected

The [source manifest](sources/manifest.json) records URLs, source commits, byte counts and SHA-256 checksums. Local snapshots include:

1. [Olimex product page](https://www.olimex.com/Products/Retro-Computers/AgonLight2/open-source-hardware).
2. [Olimex hardware/software repository](https://github.com/OLIMEX/AgonLight2), pinned at `b7f2a21c282813efd8cdf26573eb5126876432a8`: user manual Rev. 1.9, revision history, Rev. A/B schematics, netlists, schematic source and BOM PDFs; CPU, ESP32, SRAM, PSRAM and oscillator datasheets.
3. [Zilog eZ80 CPU manual UM0077](https://www.zilog.com/docs/um0077.pdf) and Olimex's copy of the eZ80F92/F93 product specification PS015317-0120.
4. [Agon MOS source](https://github.com/AgonPlatform/agon-mos), pinned at `8336409351ee5314e02801a7b72a4f1bb5282519`: startup, UART, SPI, timer, GPIO, vectors and configuration references.
5. [Agon VDP source](https://github.com/AgonPlatform/agon-vdp), pinned at `c7ac293d2aa81ddfa693390549bcd909069c8fc3`: serial routing, keyboard packets, console mode, hex loading and ZDI.
6. [Agon documentation](https://agonplatform.github.io/agon-docs/), source pinned at `f9806bd3cbff6ed5d1c08bef1d51fed11764b86b`: memory map, executable format, MOS API, firmware updates and VDP system commands.
7. [Agondev](https://github.com/AgonPlatform/agondev), source pinned at `fdea41068567a56c84da3f03912c3059d9b1d835`: C toolchain, linker, startup, standard headers and macOS installation instructions.
8. [Hexload](https://github.com/AgonPlatform/agon-hexload), pinned at `a5217d36aa846cad1b1a4d53fdf881b35d3b0c20`, and [recovery](https://github.com/AgonPlatform/agon-recovery), pinned at `95d68464afd049945e1dfd780c1c07621d565450`.

The downloaded repository trees index additional upstream material. These snapshots are reference material; library/source reuse requires checking the relevant license. Hardware PDFs were text-extracted, and Rev. B CPU and connector sheets were also rendered and visually inspected.

## Board and processors

| Component | Hardware facts | OS implication |
| --- | --- | --- |
| Main CPU | Zilog eZ80F92AZ020EG, LQFP100; 8-bit external data bus; 24-bit address space in ADL mode | Use ADL for the first port; this is not a conventional 32-bit target |
| Main clock | Rev. B schematic specifies ASE-18.432MHZ-L-C-T; MOS constants use 18,432,000 Hz | Calculate UART/timer divisors from 18.432 MHz, despite rounded 20 MHz marketing |
| CPU flash | 128 KiB on-chip | Normally contains MOS; preserve during initial RAM bring-up |
| CPU SRAM | 8 KiB on-chip | Relocatable within the 24-bit address space; MOS uses it |
| External SRAM | AS7C34096A-10TCTR, 512 KiB, connected through CPU CS0 | Main kernel/program memory; map is configured by software |
| Coprocessor | ESP32-PICO-D4, dual-core Xtensa, integrated 4 MB SPI flash and 520 KiB SRAM | Runs a separate firmware image; normally handles graphics, audio and keyboard |
| Coprocessor PSRAM | APS6404L-3SQR-SN, 64 Mbit / 8 MiB | Belongs to the ESP32, not directly addressable eZ80 RAM |
| Host USB | USB-C through CH340T to ESP32 UART0 | USB serial is not wired straight to an eZ80 UART |
| Storage | microSD connected to eZ80 SPI | A native OS eventually needs its own SD/block driver and filesystem |
| Human I/O | VGA, audio jack/buzzer, USB-A-shaped PS/2 keyboard socket | No eZ80 framebuffer and no general USB host controller |
| Expansion | 34-pin GPIO, 10-pin UEXT, 4-pin ACCESS.bus, 6-pin ZDI | Board-specific pin maps belong in the board layer |
| Power | USB-C 5 V; LiPo charger/backup; DC/DC conversion; 3.3 V logic | Reset/unplug behavior can differ with a battery connected |

Olimex's revision history identifies **Rev. B as the initial public release**; Rev. A was preliminary/unreleased. Rev. B changes include a corrected ZDI pinout and keyboard circuit. Use the board's printed revision before wiring debug connections. Dimensions are approximately 106 x 65 mm with six mounting holes.

The ESP32's wireless capabilities are properties of that chip, not a ready-made eZ80 network interface. Networking would need an explicit supported protocol and driver.

## CPU architecture and memory

The eZ80 supports Z80-compatible execution and 24-bit ADL execution, with mixed-mode instruction suffixes and distinct short/long stack behavior. Mode transitions, interrupt entry/exit, startup and context switching need architecture-specific assembly. The address space is 16 MiB; this board does not populate 16 MiB of CPU RAM.

There is no conventional MMU, paged virtual memory or user/kernel privilege separation. A practical OS design uses a shared address space and software-enforced boundaries. Unix/Plan 9 concepts can guide devices, files, processes and namespaces without promising a full Plan 9 port or POSIX compatibility.

### MOS-configured map (not immutable hardware decoding)

| Address range | Purpose under documented MOS configuration |
| --- | --- |
| `0x000000..0x01FFFF` | Internal flash / MOS |
| `0x040000..0x0AFFFF` | Conventional program RAM, 448 KiB |
| `0x0B0000..0x0B7FFF` | Moslet/module area, 32 KiB |
| `0x0B8000..0x0BBFFF` | MOS-reserved area, 16 KiB |
| `0x0BC000..0x0BFFFF` | MOS global heap/stack, 16 KiB |
| `0xB7E000..0xB7FFFF` | MOS mapping of 8 KiB internal SRAM |

All external SRAM is `0x040000..0x0BFFFF`. The upstream MOS prose contains a stray extra hex digit in its heap/stack endpoint; the 512 KiB SRAM range and linker configuration establish `0x0BFFFF` as the endpoint.

The internal SRAM resets enabled with upper address register `0xFF`, placing it at `0xFFE000..0xFFFFFF`; MOS relocates it. Do not confuse the reset map with the MOS map. Unpopulated address ranges are not usable RAM.

For the first RAM payload, constrain code, data, BSS and its own stack to `0x040000..0x0AFFFF`, with link-time overflow checks. Do not overwrite the loader or MOS areas. A later independent bootloader/kernel can establish a different map and reclaim external SRAM.

### Peripheral I/O space

These are **I/O port addresses**, not memory-mapped C pointers. Use small `IN0`/`OUT0` assembly wrappers or compiler-specific adapters confined to the architecture layer.

| Ports | Function |
| --- | --- |
| `0x80..0x91` | Six 16-bit programmable reload timers, three registers each |
| `0x92` | Timer input-source selection |
| `0x93..0x94` | Watchdog control/reset |
| `0x9A..0x9D` | Port B data, direction, ALT1, ALT2 |
| `0x9E..0xA1` | Port C data, direction, ALT1, ALT2 |
| `0xA2..0xA5` | Port D data, direction, ALT1, ALT2 |
| `0xA8..0xB3` | CS0..CS3 lower/upper bounds and control |
| `0xB4..0xB5` | Internal SRAM enable and mapping |
| `0xB8..0xBC` | SPI divisor, control, status, data |
| `0xC0..0xC7` | UART0; data/divisor aliases controlled by DLAB |
| `0xC8..0xCD` | I2C |
| `0xD0..0xD7` | UART1 |
| `0xDB..0xDC` | Peripheral clock power-down control |
| `0xE0..0xED` | RTC registers |
| `0xF0..0xF3` | Chip-select bus-mode controls |
| `0xF5..0xFF` | Flash configuration/protection/timing/programming registers |

UART line status bits include data-ready `0x01`, overrun `0x02`, parity `0x04`, framing `0x08`, transmit-holding-empty `0x20` and transmitter-empty `0x40`. UART0/1 status ports are `0xC5`/`0xD5`. The divisor is `clock / (16 * baud)`: 115200 uses 10, 384000 uses 3, and 1152000 uses 1 at the nominal board clock.

Important interrupt vector indices: timer0 `0x0A`, timer1 `0x0C`, UART0 `0x18`, UART1 `0x1A`, I2C `0x1C`, SPI `0x1E`, PB0 `0x30`, PB1 `0x32`. These are vector indices, not direct ISR addresses. The F92 controller uses two-byte vector entries; RAM-resident ADL handlers may need low-address trampolines. Read the F92 interrupt-controller section and MOS vector implementation before enabling interrupts. Initial bring-up should poll with maskable interrupts disabled.

## Serial routing and VDP protocol

The normal onboard path is:

`Mac -> USB-C -> CH340T -> ESP32 UART0 -> VDP firmware -> ESP32 high-speed UART -> eZ80 UART0`

On the eZ80, **PD0 is TX0 and PD1 is RX0**; PD2 is RTS0 and PD3 is CTS0. A comment in MOS's `uart.c` reverses RX/TX numbering; the schematic and CPU datasheet establish the pin functions.

ESP32 interprocessor pins in current VDP source: TX GPIO2, RX GPIO34, RTS GPIO13, CTS GPIO14. eZ80 RTS connects to ESP32 CTS; eZ80 CTS connects to ESP32 RTS. UART0 peripheral hardware and software flow control must be understood together, including the active-low pin convention and status-register interpretation.

The **host link is normally 115200 8N1**. The **internal CPU/VDP link may be 384000 or 1152000**, depending on firmware/negotiation. Do not configure both as 115200 or blindly change the internal divisor after loading a payload. Preserve/read the established configuration until the firmware is identified.

VDP console mode is normally off. MOS `SET CONSOLE 1` enables it; the underlying VDU sequence is bytes `23,0,254,1`. A prepared boot script can enable it without a keyboard. This assumes functioning MOS and VDP; it cannot repair an ESP32 running unrelated firmware.

The USB-side input is converted into VDP keyboard events. It is **not raw UART ASCII at the eZ80**. Current VDP packets are `[type | 0x80][payload length][payload]`; key packet `0x81` normally carries keycode, modifiers, virtual-key and key-down bytes. The bare-metal driver must parse lengths, ignore unrelated events and accept key-down text only. Ordinary output is sent as VDU text bytes; console mode reflects output to USB. A bounded ASCII monitor avoids terminal control-code ambiguity.

VDP terminal mode (`23,0,255`) changes the protocol substantially; it is a later alternative, not an interchangeable console setting.

The other eZ80 serial port is UART1, **PC0 TX1 / PC1 RX1**, exposed at GPIO pins **17 / 18**, and UEXT pins **3 / 4**. An external adapter must use 3.3 V TTL levels and a common ground, with TX/RX crossed. This route avoids the ESP32 at runtime but still needs a way to load code.

## Storage and shared buses

The SD card belongs to the eZ80: PB3 SCK, PB6 MISO, PB7 MOSI, and **PB4 as the SD chip-select GPIO**. PB2 is a separate expansion chip select. SPI is shared with expansion devices; drive unused selects inactive and serialize bus access.

The normal firmware uses FAT/FAT32 and boots scripts/programs from SD. The conservative compatibility target is a FAT32 card of at most 32 GB, matching Olimex guidance; larger cards are reported to work but are unnecessary for bring-up.

For a native OS, separate SD SPI transactions, sector/block access and filesystem policy. Start with RAM-only operation, then read-only SD access, then writes after recovery is reliable. Preserve existing card contents when preparing a loader; copy/backup boot scripts before changing startup behavior.

The eZ80 has six 16-bit timers, UART0/1, SPI, I2C, GPIO and watchdog. Its RTC pins are not populated as a functioning dedicated RTC supply/crystal circuit on this schematic; normal Agon time services are provided through VDP software. Do not assume a battery-backed wall clock from the LiPo alone. PB0 and PB1 carry ESP32 interrupt/video-sync signals, respectively; their actual behavior depends on VDP firmware.

## Expansion pinout: Rev. B

| Odd pin | Signal | Even pin | Signal |
| --- | --- | --- | --- |
| 1 | VBAT | 2 | USB 5 V input rail |
| 3 | GND | 4 | Backed-up +5 V rail |
| 5 | GND | 6 | ESP GPIO39 |
| 7 | ESP GPIO35 | 8 | ESP GPIO27 |
| 9 | ESP GPIO26 | 10 | ESP GPIO36 |
| 11 | ESP GPIO37 | 12 | ESP GPIO38 |
| 13 | eZ80 PD4 | 14 | eZ80 PD5 |
| 15 | eZ80 PD6 | 16 | eZ80 PD7 |
| 17 | eZ80 PC0 / TX1 | 18 | eZ80 PC1 / RX1 |
| 19 | eZ80 PC2 | 20 | eZ80 PC3 |
| 21 | eZ80 PC4 | 22 | eZ80 PC5 |
| 23 | eZ80 PC6 | 24 | eZ80 PC7 |
| 25 | Expansion CS / PB2 | 26 | PB5 |
| 27 | SPI MISO / PB6 | 28 | CPU clock output |
| 29 | I2C SDA | 30 | I2C SCL |
| 31 | SPI SCK / PB3 | 32 | SPI MOSI / PB7 |
| 33 | GND | 34 | +3.3 V |

UEXT: 1=3.3 V, 2=GND, 3=TX1, 4=RX1, 5=SCL, 6=SDA, 7=MISO, 8=MOSI, 9=SCK, 10=PC2/expansion CS. ACCESS.bus: 1=SDA, 2=SCL, 3=5 V, 4=GND. Note its 5 V supply despite 3.3 V logic signals.

Rev. B ZDI header: 1=3.3 V, 2=reset, 3=GND, 4=ZCL/TCK, 5=GND, 6=ZDA/TDI. Confirm orientation against the keyed header/schematic before cabling. Published ESP32-assisted recovery needs two GPIO-to-ZDI connections on an Agon Light 2. There is no default onboard ESP32-to-ZDI connection; USB alone cannot be assumed to provide arbitrary eZ80 debug access.

The USB-A keyboard socket uses PS/2 clock/data signaling and requires a PS/2-capable keyboard. The ESP32 owns VGA resistor-DAC output, synchronization and audio. Framebuffer, sprites, fonts and graphics modes are VDP services, not CPU memory-mapped video hardware.

All expansion logic is 3.3 V. Power guidance in the Olimex manual distinguishes USB input, backed-up 5 V and battery pins; do not treat them as interchangeable power inputs. Rev. B power circuitry includes BL4054 charger, MT3608 boost and SY8089 regulator.

Jumpers: `UART_DIS1`/`UART_D` open enables eZ80/ESP32 communication; closed disconnects it. `ESP_PROG1` controls ESP32 bootloader behavior. `BUZ_EN1` enables the buzzer when closed. Follow the relevant flashing procedure for programming jumpers rather than applying a generic jumper state.

## Build, loading and recovery

Agondev's latest stable release observed through GitHub on this date is **v0.22**, with an official `agondev-darwin_arm64.tar.gz` asset. It includes eZ80-targeting Clang and GNU binutils. Install a pinned release locally and inspect generated objects, ABI and dependencies before relying on it for freestanding code.

Agondev's ordinary CRT and libraries target MOS programs. The OS payload needs its own startup/linker settings; linking the normal CRT/libc by default can accidentally introduce MOS dependencies. Required compiler-generated arithmetic/memory helper routines must be audited separately from MOS-backed I/O.

The host currently has SDCC 4.6.0, whose target list includes eZ80 and MOS6502, but no Agondev executable was found on PATH or in the common directories checked. Agondev is the leading ADL candidate; the installed SDCC's ADL suitability has not been established. No toolchain was installed as part of research.

MOS binaries traditionally load at `0x040000`. A MOS-compatible wrapper contains `MOS` at file offset `0x40`, header version at `0x43`, and ADL flag `1` at `0x44`. The header is a loader entry convention, not permission to call MOS at runtime.

Hexload needs `hexload.bin` and `hexload.dll` in `/mos/` and a running receiver. `hexload vdp` receives over the onboard USB path; `hexload uart1` uses the external UART. Its documented prerequisites are MOS >=1.02, and MOS/VDP >=1.04 for the onboard route. Verify actual versions and utility presence. Intel HEX addresses must be checked by the host: the loader does not enforce safe RAM ranges. Newer paths include acknowledgment/CRC extensions, so use a pinned sender/receiver combination and verify success, not merely successful host writes.

Serial open/close and DTR/RTS can reset the board. Maintain one connection across command, upload and run, with explicit line states, readiness detection, bounded timeouts and raw transcripts. Re-enumerate the device after reset; the macOS device suffix can change.

If normal MOS/VDP is unavailable, recovery can temporarily run on the ESP32 and program the eZ80 through ZDI, with the two jumper wires mentioned above. This replaces ESP32 flash, and must follow a verified backup/recovery plan. A dedicated compatible Zilog debug cable is another route. Neither is required merely because a boot log is unusual; identify and back up the installed application first.

## Portability rules for the proposed OS

- Use a portable **freestanding ISO C99** subset in the core. Keep board registers, instructions, compiler attributes and firmware headers outside it.
- Separate CPU architecture (`arch/ez80/`), physical board (`boards/agon-light-2/`), reusable drivers (`drivers/`) and portable kernel/monitor (`kernel/`, `lib/`).
- Use `size_t` for object sizes, `uintptr_t` for address conversions and explicit-width integers for protocols. eZ80 ADL C commonly has 24-bit int/pointers; confirm the selected compiler's ABI instead of assuming host sizes.
- Serialize multibyte fields explicitly; do not transmit native C structs or assume pointer width, alignment, endianness or signed char behavior.
- Avoid required recursion, large automatic arrays, 64-bit hot paths and unbounded allocation. Keep stack and buffer budgets explicit.
- Hardware I/O and interrupt code cannot be expressed entirely in strictly portable C; isolate the small assembly/compiler adapter boundary.
- A MOS Technology 6502 port needs a specific board and memory/banking model. CPU startup, interrupt/context code, zero-page usage and its 256-byte hardware stack differ fundamentally. Standard C helps source portability but does not guarantee the same memory model or process implementation.
- LLVM-MOS is a future candidate with broad freestanding C99 support ([official SDK](https://github.com/llvm-mos/llvm-mos-sdk)); no 6502 implementation is included in the first milestone.

## Remaining facts to establish on hardware

Installed ESP32 firmware identity and full backup; installed eZ80/MOS image and version; current card contents and loader availability; physical board revision/jumper state; working headless console; internal UART baud/configuration; and repeatable recovery. These are explicit prerequisites or measurements, not assumed completed tests.

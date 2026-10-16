# VIOS Agon Light 2 elapsed timer and startup

The first VIOS milestone owns eZ80F92 timer 0 and uses polling with CPU
interrupts disabled. At the board's 18.432 MHz system clock, the timer's
1:64 prescaler produces **288000 ticks/second**, or **3.472222 microseconds
per tick**. A complete 16-bit period is **65536 ticks = 227.555556 ms**.
These are configured nominal values; no hardware frequency measurement is
claimed by the software checks below.

## Primary evidence

The locally archived [eZ80F92/eZ80F93 product specification,
PS015317-0120](sources/AgonLight2/DOCUMENTATION/EZ80F92.txt) documents:

- Page 38, Table 5: `CLK_PPD2` at `0xDC`, bit 0 gates the timer 0 clock.
- Page 77: reload `0x0000` selects a 65536-count period; a zero reload
  does not disable the timer.
- Pages 78–79: continuous mode reloads automatically and continues
  counting. Reading low latches the current high byte.
- Page 80 and pages 86–87, Table 37: `TMR_ISS` at `0x92`, bits 1:0 = `00`
  selects the system clock for timer 0. The other six source bits control
  timers 1–3.
- Page 82, Table 32: `TMR0_CTL` at `0x80`; prescaler bits 3:2 = `10`
  divide by 64. `0x1B` enables continuous mode, reload/restart and timer
  operation, while leaving timer interrupts disabled.
- Pages 83–85, Tables 33–36: `0x81` and `0x82` are respectively low and
  high count registers on reads and reload registers on writes. The
  low-then-high read order returns one coherent hardware snapshot.

No timer cascade input is documented: timers 0–3 select system clock,
RTC or external GPIO inputs. The first implementation therefore uses one
16-bit hardware timer and an explicit bounded measurement interval.

## API and measurement contract

`include/platform/timer.h` declares:

```c
#define PLATFORM_TIMER_HZ 288000UL
#define PLATFORM_TIMER_MODULUS 65536UL
void platform_timer_init(void);
unsigned short platform_timer_read(void);
```

`arch/ez80/timer.s` implements both functions directly with eZ80 `IN0` and
`OUT0` instructions; a board C shim is unnecessary. Initialization powers
up timer 0, disables it, selects its system-clock source, writes reload
`0x0000`, and starts continuous counting at divide-by-64. Read/modify/write
preserves the power and source choices for other peripherals. Timer 0
belongs exclusively to this API after initialization.

The read returns the raw **downcounter**, with the eZ80 C ABI's 16-bit
`unsigned short` result in HL. It zeroes the otherwise unused upper byte
of the 24-bit HL register. Low is read first, latching high; high is then
read without stopping, restarting or otherwise disturbing counting. With
interrupts disabled and no concurrent reader, the two reads cannot
interfere with another timer snapshot.

```c
unsigned short start = platform_timer_read();
/* Execute a bounded workload here; save its result for later validation. */
unsigned short finish = platform_timer_read();
unsigned short ticks = (unsigned short)(start - finish);
/* Validate the workload and print results after the second read. */
```

The subtraction handles a counter wrap provided **the entire measured
interval is strictly shorter than one 65536-tick period**. A timer-only
endpoint pair cannot distinguish one wrap from multiple wraps or zero
elapsed time from exactly one whole period. Choose conservative batch
sizes, confirm approximate scaling with smaller/larger batches, and shorten
workloads well before 227 ms. The same bound applies to compiler timing
and the C reference interpreter, which can take longer than native engines.
Time short programs as repeated batches to reduce quantization effects.

Keep serial input/output outside both timestamp boundaries. Validate the
computed program result, repeat samples, and record raw ticks plus the
batch size. Measure an empty timestamp pair separately to characterize
call/read overhead; report any subtraction explicitly. Conversion is
`ticks * 1000000 / PLATFORM_TIMER_HZ` microseconds, using arithmetic wide
enough to hold the product (24-bit target `unsigned int` is insufficient
for large samples).

Changing the fixed divider requires changing both the assembly control
byte and the header's frequency, then checking hardware timing again.
Divide-by-16 would improve resolution to 0.868056 us with only 56.888889 ms
per wrap. Divide-by-256 would allow 910.222222 ms per wrap at 13.888889 us
resolution. Neither alternative is enabled in this milestone. Longer
measurements need another explicitly designed timing API or frequent
software accumulation with a guaranteed polling deadline.

## BSS larger than 64 KiB

`arch/ez80/start.s` now computes `__bss_size - 1` using an ADL-mode
24-bit `SBC HL,DE`. A carry means the original size was zero, so startup
writes nothing. Otherwise it transfers that count to BC, seeds the first
BSS byte with zero, and uses the preserved zero flag to skip copying when
the original size was one. For larger sizes, overlapping `LDIR` propagates
the zero through the remaining bytes, using its full 24-bit ADL count.
This avoids checking only B and C, which previously skipped a 65536-byte
BSS entirely and truncated other large sizes.

The [eZ80 CPU user manual, UM007715-0415](sources/Zilog/eZ80-CPU-UM0077.txt),
page 241 documents the 24-bit ADL `LDIR` count; page 335 documents `SBC`
zero/carry flags. An unguarded `LDIR` with BC zero would copy 16 MiB, so
both zero and one-byte input cases are explicitly guarded. Temporary
PUSH/POP operations balance the already initialized stack.

The MOS header, ADL stack setup, interrupts-disabled entry, and
`RAM_RESTART` UART transmitter-drain loop remain intact. `ram.ld` retains
all BSS symbols and reserves `0xAF000`–`0xAFFFF` for the 4 KiB downward
growing stack, with initial SP `0xB0000`. The obsolete 65535-byte BSS
assertion is replaced by a bound against RAM below that reservation;
`__bss_end <= __stack_bottom` still rejects payload overlap.

## Local verification and limitations

Temporary probes live under `tmp/forth-platform/` and use the installed
AgonDev assembler/linker/Clang. They do not open a serial device.

- `verify.py` assembles and links actual startup bytes, then executes the
  relevant instructions in a small instruction harness. It first
  reproduced the old failure at 65536 bytes (zero bytes cleared). The
  updated startup clears exactly the requested bytes for sizes 0, 1,
  65535, 65536, 65537, 81920, 131072 and 454400, preserves neighboring
  bytes, reaches `_kernel_main`, and restores SP. The production linker
  accepts an 80 KiB BSS fixture and rejects a stack-overlap fixture.
- `verify-timer.py` executes the assembled timer entrypoints against the
  documented register/latch semantics. It checks source/power masks,
  reload/control writes, no IRQ enable, low-before-high reads across
  `0x1200`→`0x11FF` and `0x0000`→`0xFFFF`, and zero-extended results.
  This instruction harness cannot establish actual peripheral behavior
  or elapsed frequency on hardware.
- The assembler accepts both normal and `RAM_RESTART=1` startup variants.
  Disassembly confirms unsuffixed ADL `SBC`/`LDIR`, the three-byte count
  and addresses, and the retained raw restart drain loop.
- A Clang ABI probe confirms that a 16-bit return is read from HL and that
  widening to the target's 24-bit `unsigned int` preserves both bytes.

Hardware acceptance still requires repeated bounded workload samples,
known result validation, an empty-sample overhead measurement, and a
frequency sanity check. A separate agent owns deployment and that evidence.

# VIOS boot speed: why 75 s, and how to get under 10 s

Status: analysis and proposal, 2026-10-19. Read-only: nothing here was run on
the board; all numbers are from the code, the embedded-image metadata and the
timestamped serial logs in `docs/bring-up/`. Target hardware: Agon Light 2,
eZ80F92 at 18.432 MHz, ESP32-PICO-D4 VDP, image `build/vios-resident.bin`
152854 bytes (CRC32 `52c063ce`, internal UART 1152000, USB 115200).

## 1. Measured timeline (channel-3 events, seconds after ESP reset)

| t (s) | Event | Owner |
| --- | --- | --- |
| 0.00 | ESP reset | |
| 2.25 | `BOOT VIOS VDP 1.04 mux-v1` ... `NATIVE RAM ... DONE A5` | ESP: vdp-gl/audio/PS2 init, then `vios_bridge_boot()` identity, halt, discovery, stage0 (all within ~70 ms of `BOOT`) |
| 62.47 | `VIOREADY RAM image verified` | ESP: ZDI upload of 152854 bytes, **60.2 s** |
| 62.88 | `HEALTH flag 0` | CPU: `kernel_main` → console init, `forth_init`, prelude, services, handles, workbench init, `platform_boot_trial()` (**0.41 s**) |
| 66.15 | `HEALTH flag 1` | CPU: `vios_boot_startup()`: SD init, boot records, `fresh_runtime()`, `load_slot()` (packages read + compiled), `health()` (**3.27 s**) |
| ~75 | console usable (user observation) | CPU: `vios_console_open()` (mode 1, VDP fence, redraw), `/VIOS/USER.FTH`, `AUTORUN`, `forth_repl`; harness attach (**~9 s, not instrumented**) |

Cross-check from earlier logs (same ZDI code, smaller image): the Oct 16
fast-display boots measured `BOOT` → `VIOREADY` at 43.94–44.02 s; the image
committed shortly after those logs (`439c5e0`) is 111301 bytes (435 blocks),
for which the model below predicts 44.4 s. The ZDI phase scales linearly with
image size at ~102 ms per 256-byte block.

## 2. Time budget

### 2.1 ZDI upload (60 s of the 75)

Transport: `firmware/esp32-vios/include/vios_zdi.h`. Every bit costs two
`delayMicroseconds(10)` (`vz_write_bit`, `vz_read_bit`) plus ~1 µs of GPIO
register writes and call overhead, so **~21 µs per bit, 50 kHz ZCL**.

Per byte inside a burst: separator bit + 8 data bits + `delayMicroseconds(3)`
= 9 × 21 + 3 ≈ **192 µs**. Per transaction: start sequence (4 µs + 8 bits)
≈ 172 µs, done bit + 3 µs ≈ 24 µs.

Per 256-byte block in `vios_bridge_boot()` (`vios_bridge.h` lines 475–482):

| Step | Cost |
| --- | --- |
| `vios_zdi_write_memory`: `cpu_write(REG_PC)` (3-byte burst + 1-byte control) | ~1.2 ms |
| 256-byte write burst | ~49.4 ms |
| `vios_zdi_read_memory`: `cpu_write(REG_PC)` | ~1.2 ms |
| 256-byte read burst (full readback) | ~49.4 ms |
| `delay(1)` | 1.0 ms |
| **Total** | **~102 ms** |

598 blocks × 102 ms = **61 s** (measured 60.2 s). Breakdown: write 49 %,
readback 49 %, `delay(1)` 1 %, PC reloads 2 %. The whole 60 s is spent
busy-waiting inside `portENTER_CRITICAL` sections of ~50 ms each on the
Arduino core (core 1, where `setup()` runs; both core watchdogs are disabled
in `setup()`). The USB control task is pinned to core 0 and is not blocked,
but the audio driver task shares core 1 and only runs in the `delay(1)` gaps.

What the hardware allows: the eZ80F92 product specification (ZDI section,
Table 111) states memory transfers may run at up to one-half the system clock
and recommends a **4 MHz** ZDI clock for a 12–24 MHz system clock. Upstream
agon-vdp 1.04 `zdi.h` runs this same ESP32→eZ80 link with
`ZDI_WAIT_MICRO 1` (1 µs edges). The repo's own probe
(`tools/esp32-ram/zdi_probe.c`, `bit_delay` 1/10/50 µs) read the CPU identity
at all three speeds on 2026-10-16 (`docs/bring-up/progress.md`). The 10 µs
value in `vios_zdi.h` is an 80× safety margin inherited from the first probe,
never re-tuned. The link is breadboard jumpers (`docs/bring-up/progress.md`),
so 1–2 µs edges (250–500 kHz), not 0.25 µs, is the sensible target.

Model of the alternatives (same formula, 598 blocks):

| Edge delay | Per-block readback | `delay(1)` | ms/block | ZDI phase |
| --- | --- | --- | --- | --- |
| 10 µs (today) | yes | yes | 102 | 61 s |
| 10 µs | no | no | 50 | 30 s |
| 1 µs | yes | yes | 17 | 10 s |
| 1 µs | no | no | 8 | 4.7 s |
| 0.5 µs | no | no | 5.5 | 3.3 s |

### 2.2 ESP before `BOOT` (2.25 s)

Upstream VDP setup: PSRAM, vdp-gl VGA, audio, PS/2 keyboard, then
`vios_bridge_probe/boot`. Not VIOS code; leave it.

### 2.3 CPU side, `VIOREADY` → `HEALTH 0` (0.41 s)

`kernel/resident_main.c`: `platform_console_init`, `platform_timer_init`,
`forth_init` (primitive dictionary + the small built-in eval), `vios_runtime_init`
(`vios_prelude_init` compiles the ~900-byte prelude in `kernel/vios_prelude.c`,
`vios_services_init` registers 20 natives and evaluates the DEFER line, handles,
workbench), then `vios_boot_startup` sends `VDU 23,0,E2,0`. **All of this is
≤ 0.41 s measured**, so caching the compiled prelude cannot buy more than a
few hundred milliseconds. (Cleanup, not a speedup: `vios_boot_startup` calls
`fresh_runtime()` which repeats `forth_init` + `vios_runtime_init`, so the
prelude is compiled twice per boot.)

### 2.4 CPU side, `HEALTH 0` → `HEALTH 1` (3.27 s)

`kernel/vios_boot.c` `vios_boot_startup()`:

1. `storage_init()` → `sd_init()` (`drivers/sd-spi/sd.c`): 80 clocks at 288 kHz,
   CMD0 loop, CMD8, ACMD41 loop (card dependent, typically 100–1000 ms), CMD58,
   CSD, then SPI at 3.07 MHz (`ez80_spi_init(1)`, divisor 3); `f_mount` reads
   MBR/boot sector/FAT.
2. `read_state()`: two 32-byte records.
3. For the confirmed slot: `fresh_runtime()` (re-init, prelude again),
   `load_slot()`: bundle read, then per module `vios_module_load()` = package
   read (≤ 4.4 KB), CRC check, `forth_module_stage` + `forth_module_activate`
   (**compiles the module source on the eZ80**).
4. `health()`: `forth_eval("1 2 + 3 =")`, a VDP `23,0,86` round trip, `storage_sync()`.

The same segment took 1.85 s on Oct 16 before WBM1P was persisted into the
slot (`eecba9f`), so roughly 1.5–2 s is SD init + records and ~1.5 s is module
compile. SD byte transfers are cheap (polled SPI at 3 MHz, ~2.5 ms per 512-byte
sector; a few KB is tens of ms). The only large CPU-side win here would be a
dictionary snapshot instead of recompiling sources, which cuts against the
source-identity/CRC design of boot slots: **measure first, decide later**.

### 2.5 `HEALTH 1` → usable console (~9 s, unmeasured)

After `vios_boot_startup` returns, `kernel_main` runs
`vios_console_open()` (`kernel/workbench_runtime.c`: `display_reset()` sends
mode 1, waits for the `23,0,86` reply, palette/CLS; `con_redraw_now()` later
needs another VDP fence at idle), `vios_user_startup()` (reads and compiles
`/VIOS/USER.FTH`, contents unknown), `vios_autorun()` (spawns `AUTORUN` if a
module defines it) and finally `forth_repl()`, which is the first point that
consumes host console input. On the Mac side `tools/vios_resident_board.py`
polls `B` every 50 ms until `BOOT READY` (immediately after `HEALTH 1`), then
sends a nonce envelope and waits for the REPL to echo it. In the Oct 16 logs
the first `vios>` prompt followed `HEALTH 1` by ~10 ms, before the console
view, USER.FTH and AUTORUN existed, so the ~9 s is new and belongs to one of
those four steps or the harness attach. It needs instrumentation (section 5)
rather than a guess.

## 3. Options, ranked by speedup per unit of risk

| # | Change | Saves | Risk | Notes |
| --- | --- | --- | --- | --- |
| 1 | **ZDI edge delay 10 → 1 µs** (`vz_write_bit`, `vz_read_bit` in `vios_zdi.h`) | ~51 s (61 → 10 s) | low | Two constants. Within datasheet (4 MHz recommended, 9.2 MHz max), upstream VDP uses 1 µs, Oct 16 probe read identity at 1 µs. Also used by recovery `V`/`W`/`H`/`S`, so recovery gets faster too. Critical sections shrink from ~50 ms to ~5 ms. |
| 2 | **Replace per-block readback with one native CRC32 pass** (new `agon_crc.s` stage run like stage0; compare with `vios_image_crc`, already generated by `embed_vios.py`) | ~5 s more (10 → ~5.5 s incl. CRC run) | low–medium | Integrity check gets *stronger* (verified through the CPU's own bus, whole image, same reflected poly `0xEDB88320` as `vios_crc32`). New 60–100 byte eZ80 routine + ESP wait logic. Fallback that needs no CPU code: one whole-image ZDI readback pass after the write (keeps 10 s). |
| 3 | Drop `delay(1)` per block | 0.6 s | none | Was a pacing aid for 100 ms bursts; after (1) the burst is 5 ms. |
| 4 | **UART stage1 streaming**: ZDI loads a ~200-byte receiver into on-chip SRAM, ESP streams the image on `VDPSerial` at 1152000, stage1 CRCs it | ZDI phase → ~2 s | medium | 152854 × 10 bits / 1152000 = 1.33 s. Needs eZ80 receive loop (16-byte FIFO, 160 cycles per byte budget; RTS already asserted manually in `uart0_raw.s`, ESP side honours CTS in hardware via `HW_FLOWCTRL_CTS_RTS`), stage1 baud paired with the `.baud` sidecar, VDPSerial TX during `setup()`, and a ZDI fallback. Only ~3.5 s better than (1)+(2); do after the CPU side is measured. |
| 5 | Compress the image (zlib 152 → 73 KB, lzma 66 KB) with an eZ80 decompressor | halves ZDI phase | medium | Only useful if ZDI stays the transport; irrelevant after (4). LZ4/LZSA-class decoder ~300 bytes of asm, decode <1 s. |
| 6 | Persist the image in eZ80 flash | ZDI phase → 0 | high / blocked | eZ80F92 flash is 128 KB; the image is 152 KB and growing. Needs compression, a never-written ZDI flash programmer, and owner approval. Not viable now. |
| 7 | Boot from SD via MOS (`autoexec.txt` `load`/`run` at `0x40000`) | ZDI phase → SD read time | high / blocked | Changes the whole boot contract (MOS/VDP handshake "remains unresolved" per `docs/bring-up/progress.md`), SD boot files need owner approval, and the ZDI recovery path would have to coexist. Keep as a future option. |
| 8 | Dictionary snapshot instead of compiling slot modules | ≤ ~1.5 s | medium | Conflicts with source-identity boot slots; needs snapshot format and CRC binding. Measure first. |
| 9 | Compile the prelude once (remove the double `forth_init`/`vios_runtime_init`) | ≤ 0.2 s | low | Cleanup. |
| 10 | Instrument and fix the `HEALTH 1` → prompt tail | up to ~9 s | unknown until measured | Likely candidates: VDP mode switch + fence, USER.FTH, AUTORUN, harness attach. |

Not changed by any option: ESP VDP init (2.25 s), SD card init (~1–2 s).

## 4. Recommended plan

### Phase 1a: fast ZDI, readback kept (one afternoon, first A/B trial)

- `firmware/esp32-vios/include/vios_zdi.h`: introduce `VIOS_ZDI_EDGE_US`
  (default 1) and use it only in `vz_write_bit` / `vz_read_bit`. Keep the
  2 µs start-sequence and 3 µs inter-byte pauses (per transaction/byte, cheap),
  and do not touch the CPU *execution* settle times: `delayMicroseconds(10)`
  in `vios_zdi_boot_in0` (IN0 result latency, see the DISCOVER priming note
  in the README) and `delay(2)` after the stage0 release. If DISCOVER samples
  start disagreeing at 1 µs edges, raise the IN0 settle delay, not the edge.
- Record the edge constant: add it to `build-manifest.json`
  (`scripts/build_manifest.py`) and append it to the `BOOT VIOS VDP 1.04
  mux-v1` event (e.g. `zdi-edge 1us`) so before/after timing logs name the
  firmware build they came from.
- `firmware/esp32-vios/include/vios_bridge.h` `vios_bridge_boot()`: replace
  the per-block `delay(1)` with a `delay(1)` every 16 blocks so the audio
  driver task (same core) and the idle task still get a slice; the USB
  control task runs on core 0 and is unaffected either way.
- `firmware/esp32-vios/README.md`: the "10 us edges" sentence; control `2`
  reply text stays (frozen protocol marker).
- Expected: `VIOREADY` at ~12 s, console at ~25 s (with the current 13 s CPU
  tail). This phase keeps byte-for-byte readback, so it isolates "does ZDI work
  at 1 µs" from everything else.

### Phase 1b: CRC stage replaces per-block readback

- New `arch/ez80/agon_crc.s` (position independent, ADL, no stack), entry with
  `HL` = metadata block (base+256) as stage0: reads start address, length
  and a 1 KB table pointer written by the ESP into the metadata, computes
  CRC32 (reflected, init `0xFFFFFFFF`, final xor), stores the 4 bytes plus
  completion `A5`, then executes `halt` (interrupts are disabled by
  stage0's `di`). Table-driven: ~40 cycles per byte → ~0.35 s for 152854
  bytes; bitwise (no table) would be ~2 s.
- `Makefile`: build `build/agon-crc.bin` next to `agon-stage0.bin` (same
  rule shape, ≤ 256 bytes); `firmware/esp32-vios/scripts/embed_vios.py`:
  embed it as `vios_crc_stage` in `vios_stage0.h` with its hash in the
  manifest; `firmware/esp32-vios/scripts/build_manifest.py`: record it.
- `vios_bridge_boot()`: after the write loop, write the CRC table (ESP
  computes it at boot, 1 KB, 4 blocks ≈ 35 ms) and the stage into the
  discovered on-chip SRAM window (which stage0 already proved writable and
  verified by readback; keep that readback, it is 300 bytes), set `HL`/`PC`,
  release, then poll `vios_zdi_status()` bit 5 (`HALT_SLP`, datasheet Table
  125) with a 3 s bound, `cpu_halt()`, read the metadata and compare against
  `vios_image_crc`. Mismatch → `ERR VIOS RAM CRC; CPU left halted` (same
  outcome as today's readback failure). Keep the 8 KiB window overlap checks.
  Breaking into a CPU that is in `halt` via ZDI has not been exercised on
  this board: verify it first with the host fake and one board trial. The
  fallback needs nothing new: end the stage with a spin loop exactly like
  stage0, and on the ESP release, wait ~100 ms, `cpu_halt()`, read the
  metadata; if `A5` is not there yet, release and repeat (bounded, ~3 s).
  Stage0 already proves the release/halt/read/compare half of that loop.
- `firmware/esp32-vios/tests/bridge.cpp` / `tests/runtime.h`: the fake
  `vios_zdi_*` already models stage0 execution via `native_stage0()`; add a
  `native_crc()` path triggered when `PC` is set to the stage address, a
  `bad_crc` fault, and relax the `memory_writes`/`bad_readback` assertions
  that currently count one readback per block.
- Expected: `VIOREADY` at ~7–8 s (2.25 + 0.1 stage0 + 4.7 upload + 0.4 table/stage + 0.4 CRC).

### Phase 2: measure and trim the CPU side

- Add `TIMER@`-stamped boot lines (or simply more `vios_out` lines) around
  `storage_init`, `load_slot`, `health`, `vios_console_open`,
  `vios_user_startup`, `vios_autorun`; capture one boot with
  `tools/serial_monitor.py` (per-frame host timestamps) and attribute the 3.3 s
  and the ~9 s.
- Remove the double runtime init (option 9).
- Then decide on option 8 and on the console-open fence.

### Phase 3 (optional): UART stage1 streaming (option 4)

Worth ~3.5 s more; only after phases 1–2, and only with the ZDI path kept as
the fallback when the stage1 handshake fails.

### Expected boot after phases 1a+1b

| Segment | Today | After |
| --- | --- | --- |
| ESP init | 2.25 s | 2.25 s |
| ZDI upload + verify | 60.2 s | ~5.5 s |
| CPU init to `HEALTH 0` | 0.4 s | 0.4 s |
| SD + slot load to `HEALTH 1` | 3.3 s | 3.3 s (phase 2 target ≤ 2 s) |
| Console/USER.FTH/AUTORUN tail | ~9 s | ~9 s (phase 2 target ≤ 1 s) |
| **Total** | **~75 s** | **~20 s**, ~12 s after phase 2, ~9 s with phase 3 |

## 5. Verification on the board

All firmware changes go through the ESP A/B path landed in `71786fd`
(`tools/vios_firmware_ab.py install` into the inactive slot, trial marker,
automatic rollback if no `HEALTH 1` within 180 s). No eZ80 flash, SD boot
slot or partition write is involved.

1. Host tests first: `firmware/esp32-vios/tests/bridge.cpp` (boot loop, CRC
   stage, fault cases) and the existing resident host tests.
2. Phase 1a trial boot: expect `BOOT` → `NATIVE RAM ... DONE A5` unchanged
   (identity, discovery samples and stage0 readback all at 1 µs edges), then
   `VIOREADY` ~10 s after `BOOT`, `HEALTH 1`, `FIRMWARE confirmed`. Repeat
   five cold boots (`Z`) and compare channel-3 event deltas with today's
   2.25/62.47/66.15 baseline using `tools/serial_monitor.py`.
3. Recovery path at the new speed: `H` (halt + `REGS`), `S`, `D`,
   `V0400000100`-style reads (harness `memory_read`) on ten random blocks
   compared with `build/vios-resident.bin`, one `W` 256-byte write with CRC,
   `Y` resume, `X` fresh start; then the harness `connect`, `reboot`, `deploy`
   round trips. These share the same `vz_*` helpers, so a timing problem
   would show here first.
4. Phase 1b trial: same as (2), plus a deliberate negative test on the host
   fake (`bad_crc`) and one on the board by booting a firmware whose embedded
   `vios_image_crc` is deliberately wrong (expect `ERR VIOS RAM CRC`, CPU
   halted, `Z` still answers). Then install the correct image.
5. Soak: one of the existing 45-minute soaks (`tools/vios_soak.py`) on the new
   firmware, since the shorter critical sections change ESP scheduling during
   boot only but the ZDI speed also applies to recovery during the soak.

## 6. Constraints respected

- USB/ZDI recovery stays identical in protocol; only edge timing changes and
  it gets faster. `Z`/`B` remain answerable during bootstrap.
- CPU and ESP baud stay paired through the `.baud` sidecar; phase 1 does not
  touch UART. Phase 3 would make stage1 carry the same `VIOS_INTERNAL_BAUD`
  symbol via the embed script.
- Integrity: per-block readback is only dropped once the whole-image CRC32
  computed by the CPU is in place; stage0/stage1 code in on-chip SRAM keeps
  its byte-for-byte readback.
- No eZ80 flash, SD boot-slot, bootloader or partition writes. Options 6 and 7
  are listed as needing owner approval and are not recommended now.

## Result on the board (2026-10-19)

| Build | VIOREADY | HEALTH 1 | Evidence |
| --- | --- | --- | --- |
| before (10 us edges, per-block readback) | 62.5 s | 66.2 s | install-1 events |
| Phase 1a (1 us edges, readback kept) | 12.1 s | 15.7 s | `docs/bring-up/vios-boot-timing-zdi1us-2026-10-19.json` (5 boots) |
| Phase 1b (CRC stage, no per-block readback) | 7.6 s | 11.3 s | `docs/bring-up/vios-boot-timing-crc-2026-10-19.json` (5 boots) |

Times are from the ESP reset (serial-port open). Both builds were installed through the
A/B trial and confirmed. The first 1b build hit `ERR CRC stage did not finish`
and rolled itself back to app0 automatically: `vios_zdi_read_memory` loads PC,
so the poll resumed the CPU inside the metadata. The fix saves and restores PC.
The bridge fake now needs three 100 ms slices and asserts the resume PC, which
catches that bug (checked by reverting the fix). The CRC stage's real bytes run
in `tests/test_agon_crc.py` against `zlib.crc32`. Debug pause, registers, 10 RAM
block reads matching `build/vios-resident.bin`, and resume all work at 1 us
edges. Board: app1 = `firmware-bootcrc2.bin` (confirmed), app0 = Phase 1a build.
Phase 2 (CPU tail after HEALTH) is not done.

## Phase 2 result (2026-10-20, untethered boot)

`BOOT-TIMES` (new word) prints each startup step from ESP-clock stamps in
`vios_boot_startup`, with module loads split into read / check / compile.
Measured `HEALTH 0 → 1` on the board, before → after:

| Step | Before | After | Change |
|---|---|---|---|
| storage init | 81 ms | 42 ms | |
| boot state | 302 | 142 | |
| modules (5 loads) | 3,806 | 2,168 | read 1,103 → 526, check 811 → 180, compile 1,345 → 1,089 |
| health | 6 | 6 | |
| finish | 292 | 136 | |
| **total** | **4,454 ms** | **2,497 ms** | |

Changes: CRC-32 by a 256-entry table shared by `forth_module_crc32` and
`vios_crc32` (was eight 32-bit shifts per byte; +1 KB), SD sectors read by one
`ez80_spi_read` call per sector instead of a C call per byte, and the SD
CRC-16 computed with the byte-wise XMODEM formula instead of a bit loop (same
results; the card-model tests compute it bitwise). Next: compiling module
source (1.1 s) is now the largest part; caching compiled modules would be the
next step.

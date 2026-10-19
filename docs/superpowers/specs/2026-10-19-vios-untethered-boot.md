# VIOS untethered boot: booting without the ZDI wire

Status: **design proposal, 2026-10-19; needs owner decisions (§8). Nothing
here is implemented.**

## 1. Problem

Today, at every power-on, the ESP loads VIOS into the eZ80 over ZDI.

- **The wire:** two jumper links carry the load, GPIO26 to ZCL and GPIO27 to
  ZDA (`docs/bring-up/ram-console.md:20-25`). Steps:
  1. The ESP halts the CPU.
  2. It writes a 46-byte stage0 into on-chip SRAM.
  3. It uploads the 158 KB embedded image to `0x40000` in 256-byte blocks and
     checks it with a CRC32 that the CPU computes itself.
  4. It jumps to the image (`vios_bridge.h:620-681`).
- **Without the wire:** the ID check fails, so nothing loads. The eZ80 stays in
  the stock Agon Quark MOS in its flash, which VIOS never writes.
- **What the wire costs:**
  - It is a development fixture, not a product.
  - It holds GPIO26/27, the only output-capable ESP pins on the header, which
    the PS/2 mouse needs (`2026-10-19-vios-gui-windows.md:471-478`, R4).
  - A boot takes about 7.6 s to `VIOREADY`. About 2.1 s of that is MOS running
    before the ESP halts it (`vios-boot-timing-crc-2026-10-19.json`).

The goal is a boot that is reliable (it always reaches a usable system or a
clear recovery screen), flexible (images can change without reflashing the
ESP), and works without the wire. The wire stays useful for development, but
becomes optional.

## 2. Facts that shape the design

| Fact | Consequence |
|---|---|
| eZ80F92 flash is 128 KB, and the VIOS image is 158 KB and growing (`2026-10-19-vios-boot-speed.md:141`). | VIOS cannot live in eZ80 flash. Flash can hold at most a loader. |
| eZ80 flash holds MOS, which the board needs today for its memory map, CS0, the IM2 vector table and the UART set-up (`vios-tasks.md:~221`, `README.md:190`). | Replacing MOS means the loader takes over all of that. Keeping MOS gives it for free. |
| MOS boots from SD: it runs `autoexec.txt`, `load` puts a file at `0x40000`, and `run` jumps there. It loads moslets into `0x0B0000..0x0B7FFF` (32 KB) (`docs/research/hardware.md:75`). | A small loader can be a moslet that sits outside VIOS's range (`0x40000..0xAEFFF`, stack top `0xB0000`). |
| The ESP already embeds the exact resident it was built with, plus the baud and GUI pairing guard (`embed_vios.py`). | The ESP is the one place where the CPU image is guaranteed to match the firmware. An image loaded from SD can drift. |
| The CPU/VDP UART runs at 1,152,000 baud (`hardware.md:109,123`). | 158 KB is about 1.4 s over the UART, faster than the ~5.5 s ZDI upload. |
| Without ZDI, the ESP has no confirmed way to reset or halt the eZ80. To verify: the reset line on ZDI pin 2, and whether DTR/RTS reaches it (`hardware.md:187`). | Retry and rollback must run on the CPU side: the CPU jumps back to 0 (MOS) and loads again. The ESP's 3-attempt halt-and-reload loop does not apply. |
| The VIOS VDP firmware skips the MOS handshake (`video.ino:103`, `initialised = true`). | Under MOS, the ESP must answer MOS's VDP poll and mode requests again until VIOS takes over. |
| The SD card already holds VIOS boot bundles with trial, confirm and revert (`kernel/vios_boot.c:135-160`). | Everything above the resident is already reliable. Only the resident load is missing. |

## 3. Options

**A. MOS + SD, plain** (`autoexec.txt`: `load /vios/vios.bin`, then `run`)

- *For:* standard Agon practice, no flash writes, trivial to change.
- *Against:* the image can drift from the ESP firmware (baud, GUI, protocol).
  It has no CRC or fallback beyond what MOS gives, and MOS prints its own boot
  text over the screen.

**B. MOS + SD moslet stub, which loads the image from the ESP over the UART (recommended)**

`autoexec.txt` runs `/mos/viosboot.bin`, a stub of about 1–2 KB at
`0x0B0000`. Its steps:

1. Ask the ESP for a manifest, with a new `VDU 23,0,E8`. The manifest holds the
   size, the CRC32 and the build id of the embedded resident.
2. Stream the image in framed, CRC'd blocks to `0x40000`.
3. Check the CPU-computed CRC32 of the whole image, as the ZDI path already
   does.
4. Jump to `0x40000`.
5. If the ESP does not answer, or the CRC fails, fall back to an SD copy,
   `/vios/vios.bin`, checked against its own manifest file.
6. If that fails too, print a clear message and return to the MOS prompt.

- *For:*
  - Matching CPU and ESP builds stay guaranteed.
  - No flash writes.
  - About 1.4 s for the load.
  - The SD copy makes a broken ESP image survivable.
  - The ZDI path stays unchanged for the dev build.
- *Against:* it needs an SD card, plus a small MOS-handshake mode in the ESP
  firmware.

**C. eZ80 flash loader in place of MOS**

A VIOS loader of a few KB at flash address 0 sets up the clocks, the memory
map, CS0, the UART and the vectors, then loads the image from the ESP as in B
(or from SD with its own FAT reader).

- *For:* fastest (no 2.1 s MOS phase), no SD needed, no dependency on MOS
  state.
- *Against:*
  - The board loses MOS.
  - Writing the flash risks bricking it: a bad loader is recoverable only over
    ZDI, either the wire or a Zilog cable.
  - The loader must re-implement everything MOS sets up.
  - It needs owner approval (`vios-boot-speed.md:141,272`).

**D. MOS kept, with a VIOS hook in flash** (patched MOS or a second-stage
vector)

- Not viable: MOS is linked at 0 and fills the 128 KB image. It gives nothing
  over B except dropping the SD card.

## 4. Recommendation: B now, C later as an option

**B gives untethered, reliable and flexible boot with no flash risk.**

- Each layer has a fallback: the ESP image, then the SD image, then the MOS
  prompt, and above them the existing SD bundle trial and revert.
- **C is an upgrade path, not a prerequisite.** It is worth doing only if the
  2.1 s MOS phase or the SD dependency matters later. Its stub would reuse B's
  protocol unchanged, so B's work carries over.

**Boot modes, chosen automatically at ESP start:**

| Mode | When | Path |
|---|---|---|
| `dev` | ZDI ID check succeeds (the wire is present) | Today's ZDI load, unchanged; full recovery (`H S V W X Y`); no mouse |
| `user` | ZDI ID check fails (no wire) | GPIO26/27 released; the ESP answers MOS; B's stub loads the image; mouse allowed |

The ESP probes ZDI once at start. That costs milliseconds, and it already
fails cleanly with `ERR CPU identity/halt`. A build flag can force a mode.

## 5. Reliability design (B)

- **Integrity:**
  - Per-block CRC and the CPU-computed whole-image CRC32, both against the ESP
    manifest.
  - The stub refuses to jump if the image lacks the `MOS\0\1` header or has a
    build id other than the manifest's.
- **Retry:**
  - If a block fails, the stub asks again, up to 3 times. On the third failure
    it falls back to the SD copy.
  - If VIOS does not report healthy within the boot deadline (CPU health
    `E2`), the ESP records it. The next stub run reads that and goes straight to
    the SD copy.
- **Restart without ZDI:** VIOS's `platform_restart` jumps to `0`, which runs
  MOS, then `autoexec`, then the stub again. A failure counter in a reserved
  RAM word that survives a jump to `0` lets the stub stop after 3 tries and
  stay at the MOS prompt. Before this ships, verify on hardware that MOS
  leaves that word alone.
- **An ESP restart while the CPU runs** (A/B install, crash): the CPU sees the
  VDP vanish and come back. VIOS must re-handshake (the E6 mode and E4 GUI
  state) or restart through `0`. That behaviour gets its own test.
- **ESP A/B:**
  - In `user` mode the ESP cannot halt the CPU, so its trial verdict must come
    from the CPU's `E2` health over the UART (already the signal used).
  - A trial image whose start-up crashes must still fall back. The keyboard-LED
    boot loop of 2026-10-19 showed that a crash in `setup()` loops forever with
    the VALID-plus-marker scheme. The fix is to count boots in RTC_NOINIT
    early in `setup()`, before any GUI or VDP work, and to fall back after N
    starts that never confirmed.
- **No SD card or no autoexec:** MOS reaches its prompt. The ESP shows "VIOS:
  insert the boot SD" in the console window, so the screen is never blank.

## 6. Flexibility

- **Resident updates:**
  - Reflash the ESP (A/B, as now): the matched pair.
  - Or copy `/vios/vios.bin` plus its manifest to SD: the fallback image, and
    the way to try a CPU-only change without an ESP flash. A stub option
    (`viosboot sd`) prefers the SD copy.
- **Modules above the resident:** unchanged. These are the SD boot bundles
  with trial and confirm, and `BOOT-MANIFEST`.
- **MOS stays usable:** quitting VIOS or a failed boot leaves a working MOS
  with the SD card. That is the standard Agon environment, with its own tools,
  including `flash` for MOS updates.

## 7. Work plan

| Step | What | Where | Est. |
|---|---|---|---|
| U0 | Hardware facts: ZDI pin 2 reset, a RAM word surviving a jump to 0 under MOS, the MOS VDP handshake bytes at 1,152,000 baud, the moslet load and run at `0x0B0000` | board, wire attached, then removed | 0.5 d |
| U1 | ESP early boot counter (fixes the setup-crash loop, needed regardless) | `vios_bridge.h` `firmware_init` | 0.5 d |
| U2 | ESP `user` mode: ZDI probe, release GPIO26/27, MOS handshake, `E8` manifest and block server | `video.ino`, `vios_bridge.h`, `vdu_sys.h` | 1–2 d |
| U3 | Stub `viosboot.bin` (eZ80 asm/C, ≤ 2 KB): manifest, blocks, CRC, SD fallback, retry counter | `arch/ez80/`, `boards/agon-light-2/` | 1–2 d |
| U4 | SD layout and installer: `autoexec.txt`, `/mos/viosboot.bin`, `/vios/vios.bin`, manifest; the `vios_sd_install` tool | `tools/` | 0.5 d |
| U5 | VIOS restart through `0`, and re-handshake after an ESP restart | `platform_resident.c` | 1 d |
| U6 | Board acceptance without the wire: 20 cold boots, ESP A/B under `user`, a bad ESP image falling back to SD, no SD card, and boot timing | harness, user removes the wire | 1 d |

U1 is worth doing first whatever is decided: it closes the boot-loop hole that
the GUI image hit today.

## 8. Owner decisions

1. **Path:** B (MOS + SD stub, recommended), A (plain MOS `load`/`run`), or C
   (replace MOS in eZ80 flash).
2. **Mode selection:** automatic from the ZDI probe (recommended), or
   build-time only.
3. **Image source preference in `user` mode:** the ESP-embedded image first,
   then SD (recommended, keeps the matched pair), or SD first.
4. **MOS boot text:** hide it behind the GUI and log it to the console window
   (recommended), or show it.

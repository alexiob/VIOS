# Proposals: Stage 6 (networking) and Stage 7 (distribution, second architecture)

Status: owner decisions 2026-10-19: Stage 6 option A **implemented** (see below); app1 firmware A/B **approved**; second architecture **on hold**. Each
option lists cost on the measured Oct 19 system (CPU image ~147 KB, ~37 KB
RAM headroom; ESP app0 705 KB of 1.25 MB; USB 115200 to the Mac).

## Stage 6: networking

| Option | What | Cost / risk | Recommendation |
| --- | --- | --- | --- |
| A. Host services over the existing USB link | Mac-side services (already: MCP/TCP owner, SD file service, events) extended with e.g. a host-file namespace `/host/...` served by the harness through a new framed channel | ESP mux change (one channel), harness code; no Wi-Fi, no exposure | **Do first**: small, testable, keeps "no unauthenticated remote mutation" |
| B. ESP32 Wi-Fi broker | ESP joins Wi-Fi and offers a framed socket the CPU sees as handles (`/net/tcp/...`) | Wi-Fi stack ~200-300 KB on ESP, quotas, auth, recovery independence must hold while VGA/keyboard/ZDI keep working | Later, only with an explicit security design (auth token, local-network-only default, read-only by default) |
| C. Nothing on board, LAN via the Mac | The Mac proxies TCP to the harness (already possible) | none | Current state |

Acceptance for A: `OPEN /host/notes.txt` streams a Mac file through the same
handle contract; measured UART cost; no change to the console/control
latency (35.9 ms median today).

## Stage 7: robust distribution

1. **Power-loss safety of source saves.** Packages already use temp+rename
   and are content-addressed; the boot record has A/B slots. Missing:
   repeated real power cuts during `SAVE` / `BOOT-TRIAL`. Needs the owner to
   unplug the board on cue (scripted: the host starts a save and prints
   "unplug now"). ~1 hour with the owner.
2. **ESP firmware A/B.** The partition table already has app1. Option: write
   new firmware to app1, mark it pending with an ESP-side boot counter,
   confirm after the CPU reports healthy, otherwise fall back to app0. Needs
   a partition/OTA-data change (not done so far by policy); recommend only
   after (1) and with a verified USB recovery path, which exists (esptool +
   backups in `backups/app0-2026-10-18/`).
3. **Second architecture (6502).** Choose a concrete board first (NMOS 6502
   vs 65C02 matter). The portable core (`forth/system.c`, `vm_reference.c`,
   handles, prelude) is already free of eZ80 assumptions except the 24-bit
   cell; a 16-bit cell profile needs `fcell`/mask parameterization and a
   native engine. Estimate: host-side 16-bit reference build and tests first
   (days), then board bring-up.

## Questions for the owner

- Stage 6: proceed with option A (host namespace over USB)?
- Stage 7: schedule the power-cut session? Allow app1/OTA partition use?
- Which 6502 board (if any) should be the second target?

## Stage 6 option A, implemented 2026-10-19

USB channel 4 carries host-service messages: the CPU sends VDU 23,0,E3,len,
payload; the ESP forwards it on channel 4; the harness answers from
`HostService` (`tools/vios_host_service.py`, sandbox `~/VIOS-host`, no
absolute paths, `..` or symlink escapes) and the ESP delivers the reply as
CPU packet FC. `OPEN /host/...` is a handle kind (read/write, 200-byte
chunks, 1 s timeout when no harness serves); `LOAD`/`INCLUDE /host/...`
read whole files. An idle pump in the harness answers requests between
commands. Agon: read 37 bytes from a Mac file, wrote a file back, LOAD of a
Mac-edited source in 106 ms, forbidden path refused, no leaks.

## Stage 7 item 2 (ESP firmware A/B), implemented 2026-10-19

`tools/vios_firmware_ab.py status|install|rollback` (esptool at 115200,
chunked digest-checked reads; writes are hash-verified, never retried).
`install` backs up otadata and the target slot to `backups/ab-2026-10-19/`,
writes the image to the slot that is not running, then adds an otadata entry
in the other sector with state **VALID** and a `VIOS-TRIAL` marker in the
unused label field. The firmware (`vios_bridge.h`, `firmware_*`) treats the
marker as a trial: CPU health 1 clears it (`FIRMWARE confirmed`); a CPU boot
deadline failure, no health within 180 s, or the control command `FR` marks
the entry INVALID and reboots into the other slot. `F` reports
`FIRMWARE <slot> <state>[ trial]`.

Why not the standard NEW/PENDING_VERIFY flow: this board's bootloader (not
the Arduino or Olimex build) has rollback enabled but its own otadata writes
do not land: the erase succeeds, the program does not. A NEW entry written
by esptool was found erased after one boot, and the pre-install backup had
the same signature (seq 0, no CRC). Writes made by the app do land (seq 2
marked INVALID by `FR`, read back). Opening the USB serial port resets the
ESP. Replacing the bootloader would fix the standard flow but is outside
the approved scope.

On the board: app1 = `backups/app0-2026-10-18/firmware-ab2.bin` (sha256
6764528…71fb, includes the d95e070 host-protocol fix), seq 4 VALID and
confirmed; app0 = previous `firmware-host-load` image, seq 1, fallback.
Self-rollback app1 -> app0 verified with the first A/B build. `/host/`
re-verified: `docs/bring-up/vios-host-files-ab-2026-10-19.log`. Boot to
health takes ~66 s, dominated by the ZDI RAM upload (ESP ready at 2 s,
VIOREADY at 62 s).

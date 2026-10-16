# Prepared firmware and SD changes for review

2026-10-16. The user approved both changes below, and they have been applied. ESP32 app0 programming passed target hash verification (`esp32-app-update.log`). The card was backed up, reformatted, populated and safely ejected (`sd-bootstrap-manifest.json`). The original review details are retained below. Subsequent ZDI bring-up also preserved the complete eZ80 flash; its metadata is in `backups/ez80/original-2026-10-16.json` relative to the workspace root. eZ80 flash has not been programmed.

## Preserved ESP32 installation

- Chip: ESP32-PICO-D4 rev 1.0, 4 MB flash, 40 MHz crystal.
- Full backup: `backups/esp32/original-2026-10-16.bin`, 4,194,304 bytes.
- SHA-256: `91693875ae044e50eb10d02dd41c6efc528609bdd9b012aab4576b929c535e1c`.
- Full-image MD5 calculated on the target and matched locally: `171395d009f13c5eba3f921d5e7d745c`.
- Non-erased blocks were read with esptool's target MD5 check. Erased blocks were reconstructed as 0xFF only after their target MD5 matched an entirely erased block; the final assembled image was checked against a fresh target digest across all 4 MB. Individual file SHA-256 values are retained in `backups/esp32/chunks-manifest.json`.
- App0 at 0x10000 contains a valid ESP32 image with the observed Bulgarian GPIO diagnostics and an `Agon Quark VPD Version` string. App1 is erased. NVS and SPIFFS regions are erased. The OTA record selects app0.
- App0 reports a 2022-09-15 Arduino ESP-IDF v4.4.2 library build. This is not an exact application version. Its image checksum and appended SHA-256 validate. The diagnostic's control flow/test-fixture requirements are not established.
- No flash was erased or written during identification/backup. Esptool's flasher stub executed only from ESP32 RAM.

## Proposed ESP32 change

Replace **only the app0 application** at 0x10000 with Olimex's published VDP 1.04 image, from `OLIMEX/AgonLight2` commit `b7f2a21c282813efd8cdf26573eb5126876432a8`, path `SOFTWARE/2-AgonLight2-VDP-104/binary-flasher/firmware.bin`.

- Local candidate: `.tools/firmware/olimex-vdp-1.04/firmware.bin`.
- Size: 630,624 bytes, fits the existing 0x140000-byte app0 partition.
- SHA-256: `dbc1347aeee725b4646763d73e39bf05a65d70907b140bb8e51cc5fd0bf1dd23`.
- Esptool validates its ESP32 chip ID, image checksum and embedded hash. Image mode is DIO, 80 MHz flash, 4 MB, matching the existing boot settings.
- Preserve existing bootloader, partition table, OTA selection and other partitions. The vendor's newer table reserves a coredump partition at the end of SPIFFS, but the application slots and NVS/OTA locations match. No bootloader/table update is needed for the initial VDP test; do not claim full firmware compatibility before boot testing it.
- Keep the eZ80 flash untouched. At review time its firmware had not yet been identified or backed up; it was subsequently backed up twice through ZDI.
- After programming, esptool must verify the target application digest before reset. If the new app fails, restore the backed-up app0 slot or full image through the same ROM loader, which already connected without manual jumper changes.

Candidate source: https://github.com/OLIMEX/AgonLight2/blob/b7f2a21c282813efd8cdf26573eb5126876432a8/SOFTWARE/2-AgonLight2-VDP-104/binary-flasher/firmware.bin

Full restoration command, only if needed and after rechecking the device identity:

```
.venv/bin/esptool --chip esp32 --port /dev/cu.usbserial-22430 --baud 115200 write-flash 0 backups/esp32/original-2026-10-16.bin
```

## Proposed SD preparation

The user confirmed that `/Volumes/Untitled` is the Agon card. Read-only inventory identifies a 62.5 GB removable USB medium (`disk29`, partition `disk29s1`), ExFAT, volume UUID `16F9EDAE-4001-31C2-A737-FABB0ADF122C`. Its visible root has no application/user files, only macOS metadata folders. Legacy MOS needs FAT/FAT32; this card is not currently formatted suitably.

Before changing it, preserve a file inventory, a copy of its current files, and its partition metadata. Recheck removable status, capacity and UUID immediately before formatting; never reuse an obsolete disk number after reconnection.

Proposed layout: MBR, a 31 GB FAT32 partition named `AGON`, remaining space unallocated. This is deliberately below the manual's 32 GB compatibility recommendation. The command will repartition **this whole removable card**, replacing the ExFAT filesystem. User approval is required before executing it.

Populate only headless bring-up files: backed-up/adapted `autoexec.txt`, a MOS console initializer if needed, `mos/hexload.bin` and `mos/hexload.dll` from pinned hexload v1.51, and the RAM monitor binary. No eZ80 firmware update utility or automatic flash action in the boot script. First startup should expose firmware identification and the serial prompt; loading the monitor is a separate host action.

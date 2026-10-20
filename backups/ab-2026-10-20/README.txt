U6 acceptance (docs/superpowers/specs/2026-10-19-vios-untethered-boot.md §16.10),
written by tools/vios_firmware_ab.py install before each slot write:
- 073335: before mos3-2 -> app0 (app0 held firmware-ez80flash-2)
- 074541: before firmware-mos3-2-badcrc -> app1 (app1 held firmware-mos3-1)
- 075427: before mos3-1 -> app1 again (app1 held the bad image)
Final board state: app1 = firmware-mos3-1 (boots), app0 = firmware-mos3-2.
backups/app0-2026-10-18/firmware-mos3-2-badcrc.bin is DELIBERATELY BAD (manifest
CRC e872aae4 vs resident e872aae5, tools/vios_esp_fault_image.py): it only boots
VIOS through the SD fallback. Do not install it except to repeat that test.

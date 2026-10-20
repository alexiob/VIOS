SD card state before untethered-boot U6 (2026-10-19 15:40, read through the harness).
/autoexec.txt (23 B, sha256 c3971b547e88864d65ec52fb14a90c85d96e9ae31a769f2a5c89ea4eec43d781) = this folder's autoexec.txt
/: AUTOEXEC.TXT 23, CONSOLE.BIN 138, MONITOR.BIN 1390, MOS/, VIOS/ (+ macOS _ files)
/mos: HEXLOAD.BIN 315, HEXLOAD.DLL 6520 (no VIOSBOOT.BIN)
/vios: BOOT_A.VIB 840, BOOT_B.VIB 676, BOOT0.REC 32, BOOT1.REC 32, module dirs (no VIOS.BIN/VIOS.MAN/BOOT.CNT)
Undo U6: write autoexec.txt back; delete /mos/viosboot.bin, /vios/vios.bin, /vios/vios.man, /vios/boot.cnt.

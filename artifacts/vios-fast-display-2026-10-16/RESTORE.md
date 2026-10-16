# Fast-display release and restore

Default: matched 1,152,000-baud CPU/ESP pair; USB stays at 115200.
The 384,000-baud matched pair is retained under variants/384000/.
Never mix CPU and ESP images from different variants.

The original readback-verified app0 payload is under restore/. Its SHA-256 is recorded in manifest.json. Restore only this app0 image at offset 0x10000 using the established application-only restore procedure in evidence/docs/bring-up/firmware-and-sd-change.md (or the repository copy). Do not write eZ80 flash, the ESP bootloader/partition table, or SD boot selections. The original app0 embeds its matching resident.

Inspect source/ and evidence/ directly; matching compressed archives and per-original-path SHA-256 maps are also included. manifest.sha256 checks the manifest; manifest.json is written last. Verify with `python3 tools/freeze_fast_display.py --verify-only`.

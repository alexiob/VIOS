# FatFs source provenance

`ff.c`, `ff.h`, `ffconf.h`, `diskio.h` and `LICENSE` were retrieved from the
AgonPlatform/agon-mos repository at commit
`8336409351ee5314e02801a7b72a4f1bb5282519`, directory `src_fatfs`:

https://github.com/AgonPlatform/agon-mos/tree/8336409351ee5314e02801a7b72a4f1bb5282519/src_fatfs

The filesystem is ChaN FatFs **R0.14b**, revision 86631. Preserve its copyright,
redistribution condition and disclaimer, which remain in the source headers and
`LICENSE`. MOS's project-level MIT license is additionally preserved as
`MOS-LICENSE`; the FatFs files carry their own permissive license.

VIOS changes:

- `ff.h`: C99 `UINT` is explicitly `uint32_t`. AgonDev's `unsigned int` is
  24-bit; upstream documents a 16/32-bit integer model. API counts must have
  an explicit supported width. `BYTE`, `WORD`, `DWORD`, LBA and file offsets
  remain 8/16/32-bit types. Selected filesystem configurations were cross-built
  for eZ80 and exercised on a host image; actual target execution is separate.
- `ffconf.h`: one writable 512-byte-sector volume; tiny shared sector buffer;
  no long names, exFAT, format/partition APIs, RTC, heap, Unicode conversion,
  string I/O, trim or OS locking. Short-name directory and rename APIs remain.
  FSINFO free/last hints are ignored, so allocation does not trust stale hints.
- `diskio.c`: new VIOS adapter to `drivers/sd-spi/sd.h`, including physical
  bounds, status invalidation, and real CTRL_SYNC. It replaces MOS's adapter,
  whose RTC implementation depends on VDP.

`ff.c` is unmodified. No MOS syscall or MOS filesystem object ABI is used.

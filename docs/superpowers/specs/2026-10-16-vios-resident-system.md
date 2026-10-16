# VIOS persistent and malleable Agon system

User approved all four proposed next steps: concurrent USB control/VDP, SD-backed source, replaceable services, reproducible module activation. Execute in the existing non-Git workspace; prior firmware backups and tested artifacts remain immutable. This is architectural work, decomposed into transport, storage and module tasks. The user's explicit request to proceed supplies implementation authorization; do not add skill-induced approval loops.

## Hardware and transport

Keep all eZ80 flash untouched. A flash-backed ESP32 VDP derivative retains video, audio and keyboard and adds a boot supervisor over the existing GPIO26/27 ZDI links. Its generated embedded VIOS RAM payload boots without host intervention, while original MOS remains a recovery fallback. Preserve bootloader/partition table and bounds-check app0. Build/pin full upstream VDP sources/dependencies before any approved firmware write and preserve the exact rollback image. Verify every target transfer and final flash image.

USB packets use zero-delimited COBS with version1, channel u8, sequence u16LE, length u16LE, payload<=240 and CRC32IEEE of header+payload. Channels1 console,2 debug ASCII request/reply,3 status. Console is seq0. Control replies echo request sequence. CPU UART remains standard VDP protocol; reserved VDU23,0,E0,len,payload sends console output to USB, FE,len,payload carries console input back. Parse extensions only in VDP command context, never scan arbitrary graphic payload. No implicit VGA mirroring; explicit native VDU services keep serial bandwidth predictable. Retain RAM-only recovery helper independently.

## Native storage

Freestanding SPI SD driver: PB7 MOSI/PB6 MISO/PB3 SCK/PB4 active-low CS, PB2 SS high. Initialize below400kHz and accelerate only after card ready. Use bounded timeouts while preserving timer0 benchmarking. FAT32 partition and 32-bit sector/file arithmetic; no heap/LFN/RTC. Licensed FatFs configured and audited for eZ80 24-bit int. Whole-file read/write plus rename/remove/mkdir/sync interface, single owner. Preserve existing card contents; create VIOS files/directories only. Source persistence uses checked versioned files and a known-good boot selection, not raw dictionary snapshots.

## Runtime services and modules

Native service words use one explicit F_NATIVE trap with inline24bit ID; ordinary VM opcodes/calls remain unchanged. Console, clock, VDP and file services are exposed as words and replaceable DEFER bindings. Hardware details stay in board/arch drivers, not portable compiler code.

Single pending module stage: bounded names, version, source CRC32, dependencies, source and test word. Stage compiles and tests with C reference and a managed-data overlay, rejects irreversible services/native memory writes outside managed data, then rolls back all dictionary/data/VM state. Activate repeats validation against current dependencies and commits only on success. Retain bounded prior generations; strict LIFO rollback prevents reclaiming code still referenced by newer modules. Native direct engine remains normal execution default. Full module source/inventory/dependencies/test/activation data is inspectable over host harness.

## Acceptance and constraints

Headlessly create/test/activate/save a service, restart the board without host RAM upload, reload from SD, inspect source/hash, replace service and restore prior generation. Concurrent VDP mode/graphics/audio request responses remain available through the native services. No display/keyboard assumed attached; physical VGA/audio output needs later user observation if it cannot be established from hardware responses alone. Measure RAM/image cost, transport throughput and existing VM benchmark regression. Preserve raw hardware evidence and new artifact manifest without overwriting the prior milestone.

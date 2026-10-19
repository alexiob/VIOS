/* VIOS untethered boot, ESP side (docs/superpowers/specs/2026-10-19-vios-untethered-boot.md).
 * MIT.
 *
 * In user mode (no ZDI wire) MOS boots the eZ80 normally and its autoexec runs
 * the stub /mos/viosboot.bin, which loads the resident embedded in this
 * firmware over the VDP UART:
 *
 *   VDU 23,0,E8,0                       manifest request
 *   VDU 23,0,E8,1,index_lo,index_hi,n   send blocks index..index+n-1 (n 1..32)
 *
 * Every reply is one VDP packet with code VIOS_BOOT_PACKET (the CPU sees 0xFB);
 * its first byte says what it is:
 *
 *   'M' ver block size:u32 crc32:u32 gui baud:u32     manifest (16 bytes)
 *   'B' index:u16 data[<=block] crc32(data):u32       one block
 *   'E' index:u16                                    no such block (end/range)
 *
 * All integers are little endian. CRCs are the standard CRC-32 (zlib),
 * so the whole-image CRC equals vios_image_crc. Portable: no Arduino.
 * Also: the MOS-phase text filter (MOS boot text goes to the console window,
 * never to the framebuffer the GUI owns). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint8_t VIOS_BOOT_PACKET = 0x7B;
static const uint8_t VIOS_BOOT_VERSION = 1;
static const unsigned VIOS_BOOT_BLOCK = 192;      // 1 + 2 + 192 + 4 = 199 <= 255
static const unsigned VIOS_BOOT_BURST = 32;       // blocks per request: bounds parser time (~55 ms)

static inline uint32_t vios_boot_crc32(uint32_t crc, const uint8_t *data, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8; ++b) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0);
    }
    return ~crc;
}
static inline void vios_boot_put32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static inline unsigned vios_boot_blocks(unsigned size) { return (size + VIOS_BOOT_BLOCK - 1) / VIOS_BOOT_BLOCK; }

/* The manifest packet payload; returns its length (16). */
static inline unsigned vios_boot_manifest(uint8_t *out, unsigned size, uint32_t crc, uint8_t gui, uint32_t baud) {
    out[0] = 'M'; out[1] = VIOS_BOOT_VERSION; out[2] = (uint8_t)VIOS_BOOT_BLOCK;
    vios_boot_put32(out + 3, size); vios_boot_put32(out + 7, crc);
    out[11] = gui; vios_boot_put32(out + 12, baud);
    return 16;
}

/* Block `index` of the image as a packet payload in out (>= 199 bytes);
 * returns its length, or an 'E' payload (3) past the end. */
static inline unsigned vios_boot_block(uint8_t *out, const uint8_t *image, unsigned size, unsigned index) {
    out[1] = (uint8_t)index; out[2] = (uint8_t)(index >> 8);
    if (index >= vios_boot_blocks(size)) { out[0] = 'E'; return 3; }
    unsigned offset = index * VIOS_BOOT_BLOCK, n = size - offset;
    if (n > VIOS_BOOT_BLOCK) n = VIOS_BOOT_BLOCK;
    out[0] = 'B';
    memcpy(out + 3, image + offset, n);   // PROGMEM is directly addressable on the ESP32
    vios_boot_put32(out + 3 + n, vios_boot_crc32(0, out + 3, n));
    return 3 + n + 4;
}

/* MOS phase (before the resident's first VDU 23,0,E* command): how many
 * argument bytes follow VDU code c (BBC/Agon VDU table), so they can be
 * swallowed instead of drawn. VDU 23 is not listed: it goes to the parser. */
static inline unsigned vios_mos_vdu_args(uint8_t c) {
    switch (c) {
    case 1: case 17: case 22: return 1;
    case 18: case 31: return 2;
    case 28: case 29: return 4;
    case 19: case 25: return 5;
    case 24: return 8;
    default: return 0;
    }
}
/* MOS-phase bytes that are console text (printable, CR, LF, backspace). */
static inline bool vios_mos_text(uint8_t c) { return (c >= 32 && c < 127) || c == 13 || c == 10 || c == 8; }

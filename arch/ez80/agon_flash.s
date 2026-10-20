/* Agon Light 2 eZ80F92 flash programmer, run from external RAM over ZDI
 * (docs/superpowers/specs/2026-10-19-vios-untethered-boot.md §16).
 *
 * Same sequence as envenomator/agon-recovery src_flash/flash.s (MIT,
 * Copyright (c) Jeroen Venema): unlock, unprotect all pages, set the flash
 * clock divider for 18.432 MHz (5.1 us => 0x5F), mass erase, copy the image
 * with LDIR, protect all pages again, then set the done flag and spin.
 *
 * The ESP (vios_bridge.h, MF) places this at 0x040000, the image at
 * 0x050000 and its 24-bit length at 0x070000, clears the flag at 0x070003,
 * starts it with PC=0x040000 in ADL mode, polls the flag, and then reads the
 * whole flash back through ZDI to check its CRC. No stack is used besides SP.
 */
.assume ADL = 1
.section .text
.global _agon_flash
_agon_flash:
    ld sp, 0x0BFFFF
    xor a, a
    ld (0x070003), a          /* done = 0 */
    ld a, 0xB6                /* unlock */
    out0 (0xF5), a
    ld a, 0x49
    out0 (0xF5), a
    xor a, a                  /* unprotect all pages */
    out0 (0xFA), a
    ld a, 0xB6                /* unlock again */
    out0 (0xF5), a
    ld a, 0x49
    out0 (0xF5), a
    ld a, 0x5F                /* flash clock divider */
    out0 (0xF9), a
    ld a, 0x01                /* mass erase */
    out0 (0xFF), a
1:
    in0 a, (0xFF)
    and a, 0x01
    jr nz, 1b
    ld de, 0x000000
    ld hl, 0x050000
    ld bc, (0x070000)
    ldir
    ld a, 0xB6                /* unlock, then protect all pages */
    out0 (0xF5), a
    ld a, 0x49
    out0 (0xF5), a
    ld a, 0xFF
    out0 (0xFA), a
    ld a, 0x01
    ld (0x070003), a          /* done = 1 */
2:
    jr 2b

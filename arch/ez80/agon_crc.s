/* Agon Light 2 boot CRC stage. Position independent; no stack, flash or I/O.
 * Runs from on-chip SRAM after the ESP uploads the RAM image over ZDI without
 * per-block readback. ZDI supplies ADL=1, interrupts disabled (stage0's DI),
 * PC=stage start and:
 *   IX = image start, 256-byte aligned (0x040000)
 *   IY = metadata: +3,+4 block count (LE16, >0); +5..+8 CRC out; +9 done A5
 *   HL = table: four 256-byte planes, 256-aligned, plane k = byte k of the
 *        standard reflected CRC32 table entry (planes must not cross H=FF)
 * Computes zlib-compatible CRC32 over count*256 bytes, written last-to-first
 * with the A5 completion byte last, then spins like stage0.
 */
.assume ADL = 1
.section .text
.global _agon_crc
_agon_crc:
    ld bc, 0xFFFFFF          /* CRC register B:C:D:E = FFFFFFFF (B high) */
    ld de, 0xFFFFFF
.Lbyte:
    ld a, (ix+0)
    xor a, e
    ld l, a                  /* HL -> plane0[index] */
    ld a, (hl)
    xor a, d                 /* new byte0 = t0 ^ old byte1 */
    ld e, a
    inc h
    ld a, (hl)
    xor a, c                 /* new byte1 = t1 ^ old byte2 */
    ld d, a
    inc h
    ld a, (hl)
    xor a, b                 /* new byte2 = t2 ^ old byte3 */
    ld c, a
    inc h
    ld b, (hl)               /* new byte3 = t3 */
    dec h
    dec h
    dec h
    inc ix
    ld a, ixl
    or a, a
    jr nz, .Lbyte            /* 256 bytes per block (IX is block aligned) */
    ld a, (iy+3)
    sub a, 1
    ld (iy+3), a
    ld a, (iy+4)
    sbc a, 0
    ld (iy+4), a
    or a, (iy+3)
    jr nz, .Lbyte
    ld a, e
    cpl
    ld (iy+5), a
    ld a, d
    cpl
    ld (iy+6), a
    ld a, c
    cpl
    ld (iy+7), a
    ld a, b
    cpl
    ld (iy+8), a
    ld a, 0xA5
    ld (iy+9), a
.Ldone:
    jr .Ldone

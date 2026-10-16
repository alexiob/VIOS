/* Agon Light 2 SRAM bootstrap. Position independent; no stack or flash access.
 * ZDI supplies ADL=1, PC=on-chip SRAM start, HL=metadata address.
 * Configure external RAM with native CPU instructions, then report actual I/O
 * readbacks as LBR, UBR, CTL, BMC, completion A5 (written last).
 */
.assume ADL = 1
.section .text
.global _agon_stage0
_agon_stage0:
    di
    ld a, 0x0B
    out0 (0xA9), a
    ld a, 0x04
    out0 (0xA8), a
    ld a, 0x01
    out0 (0xF0), a
    ld a, 0x08
    out0 (0xAA), a
    in0 a, (0xA8)
    ld (hl), a
    inc hl
    in0 a, (0xA9)
    ld (hl), a
    inc hl
    in0 a, (0xAA)
    ld (hl), a
    inc hl
    in0 a, (0xF0)
    ld (hl), a
    inc hl
    ld a, 0xA5
    ld (hl), a
.Ldone:
    jr .Ldone

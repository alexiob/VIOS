/* eZ80F92, 18.432 MHz / (16 * 10) = 115200 baud, 8N1. */
.assume ADL = 1
.section .text
.global _ez80_uart0_raw_init
_ez80_uart0_raw_init:
    in0 a, (0xA3)
    or 3
    out0 (0xA3), a
    in0 a, (0xA4)
    and 0xFC
    out0 (0xA4), a
    in0 a, (0xA5)
    or 3
    out0 (0xA5), a
    ld a, 0x83
    out0 (0xC3), a
    ld a, 10
    out0 (0xC0), a
    xor a
    out0 (0xC1), a
    ld a, 3
    out0 (0xC3), a
    xor a
    out0 (0xC1), a
    ld a, 7
    out0 (0xC2), a
    ld a, 2
    out0 (0xC4), a
    ret

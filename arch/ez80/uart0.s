/* eZ80F92 I/O instructions. C ABI: byte result in A, stack slots 3 bytes. */
.assume ADL = 1
.section .text
.global _ez80_uart0_status, _ez80_uart0_modem, _ez80_uart0_read
.global _ez80_uart0_write, _ez80_uart0_polling
.global _ez80_uart0_tx_burst
.global _ez80_uart0_rx_pause, _ez80_uart0_rx_resume, _ez80_uart0_rx_drain
.global _ez80_uart0_rx_overruns
/* Preserve the status byte and FIFO destination while recording clear-on-read OE. */
.macro count_overrun
    bit 1, a
    jr z, 1f
    push hl
    ld hl, (_ez80_uart0_rx_overruns)
    inc hl
    ld (_ez80_uart0_rx_overruns), hl
    pop hl
1:
.endm
_ez80_uart0_status:
    in0 a, (0xC5)
    count_overrun
    ret
_ez80_uart0_modem:
    in0 a, (0xC6)
    ret
_ez80_uart0_read:
    in0 a, (0xC0)
    ret
_ez80_uart0_write:
    ld hl, 3
    add hl, sp
    ld a, (hl)
    out0 (0xC0), a
    ret
_ez80_uart0_tx_burst:
    push ix
    ld ix, 0
    add ix, sp
    ld bc, (ix+9)
    /* Clamp the complete 24-bit unsigned length before using its low byte. */
    ld hl, 32
    or a
    sbc hl, bc
    jr nc, 4f
    ld bc, 32
4:
    ld de, 0
    ld a, c
    or b
    jr z, 6f
    ld hl, (ix+6)
5:
    in0 a, (0xC6)
    bit 4, a
    jr z, 6f
    in0 a, (0xC5)
    count_overrun
    bit 5, a
    jr z, 6f
    ld a, (hl)
    out0 (0xC0), a
    inc hl
    inc de
    dec c
    jr nz, 5b
6:
    ex de, hl
    pop ix
    ret
_ez80_uart0_polling:
    xor a
    out0 (0xC1), a
    in0 a, (0xC4)
    or 2
    out0 (0xC4), a
    ret
_ez80_uart0_rx_pause:
    in0 a, (0xC4)
    and 0xFD
    out0 (0xC4), a
    ret
_ez80_uart0_rx_resume:
    in0 a, (0xC4)
    or 2
    out0 (0xC4), a
    ret
_ez80_uart0_rx_drain:
    push ix
    ld ix, 0
    add ix, sp
    in0 a, (0xC4)
    and 0xFD
    out0 (0xC4), a
    ld hl, (ix+6)
    ld b, 32
    ld c, 0
2:
    in0 a, (0xC5)
    count_overrun
    bit 0, a
    jr z, 3f
    in0 a, (0xC0)
    ld (hl), a
    inc hl
    inc c
    djnz 2b
3:
    ld hl, 0
    ld l, c
    pop ix
    ret
.section .bss
_ez80_uart0_rx_overruns:
    .space 3

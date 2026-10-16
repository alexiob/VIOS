/* eZ80F92 I/O instructions. C ABI: byte result in A, stack slots 3 bytes. */
.assume ADL = 1
.section .text
.global _ez80_uart0_status, _ez80_uart0_modem, _ez80_uart0_read
.global _ez80_uart0_write, _ez80_uart0_polling
_ez80_uart0_status:
    in0 a, (0xC5)
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
_ez80_uart0_polling:
    xor a
    out0 (0xC1), a
    in0 a, (0xC4)
    or 2
    out0 (0xC4), a
    ret

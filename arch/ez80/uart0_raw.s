/* eZ80F92, 18.432 MHz / (16 * divisor), 8N1.
 * Historical raw monitor builds retain 115200 unless explicitly selected. */
.ifndef VIOS_UART0_BAUD
.equ VIOS_UART0_BAUD, 115200
.endif
.if (VIOS_UART0_BAUD != 115200) && (VIOS_UART0_BAUD != 384000) && (VIOS_UART0_BAUD != 1152000)
.error "Unsupported internal UART baud"
.endif
.assume ADL = 1
.section .text
.global _ez80_uart0_raw_init
_ez80_uart0_raw_init:
    /* Enable UART0 and Port D clocks, including after a cold CPU reset. */
    in0 a, (0xDB)
    and 0x7E
    out0 (0xDB), a
    /* PD0..3: TX/RX/RTS/CTS alternate functions. Do not inherit MOS CTS setup. */
    in0 a, (0xA3)
    or 0x0F
    out0 (0xA3), a
    in0 a, (0xA4)
    and 0xF0
    out0 (0xA4), a
    in0 a, (0xA5)
    or 0x0F
    out0 (0xA5), a
    ld a, 0x83
    out0 (0xC3), a
    ld a, 1152000 / VIOS_UART0_BAUD
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

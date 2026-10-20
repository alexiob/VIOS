/* Interrupt-system helpers for the tasks design (docs/superpowers/specs/
 * 2026-10-19-vios-tasks.md, stage B0). MIT.
 * C ABI: byte results in A, arguments in 3-byte stack slots.
 */
.assume ADL = 1
.section .text
.global _platform_irq_i, _platform_io_read
/* unsigned char platform_irq_i(void): the I register (interrupt vector base). */
_platform_irq_i:
    ld a, i
    ret
/* unsigned char platform_io_read(unsigned port): IN from an internal I/O port
 * (00xxh). Some ports have read side effects (a timer's CTL acknowledges it). */
_platform_io_read:
    ld hl, 3
    add hl, sp
    ld bc, (hl)
    ld b, 0
    in a, (c)
    ret

/* ---- Stage B1: the 1 kHz PRT1 tick and the millisecond clock ---- */
.global _platform_io_write, _platform_irq_save, _platform_irq_restore
.global _vios_tick_isr, _vios_stray_isr, _vios_ms_clock, _vios_irq_stray
.global _vios_preempt_armed, _vios_preempt_count
.global _vios_uart0_isr, _vios_uart0_isr_enable, _vios_rx_ring, _vios_rx_head, _vios_rx_tail, _vios_rx_dropped
.extern _ez80_uart0_rx_overruns
.extern _fvm_budget, _forth_budget_stolen, _forth_tick_pending
/* void platform_io_write(unsigned port, unsigned char value) */
_platform_io_write:
    ld hl, 6
    add hl, sp
    ld a, (hl)
    ld hl, 3
    add hl, sp
    ld bc, (hl)
    ld b, 0
    out (c), a
    ret
/* unsigned char platform_irq_save(void): 1 if interrupts were enabled; disables them. */
_platform_irq_save:
    ld a, i                    /* P/V = IEF2 */
    ld a, 0
    jp po, 1f
    ld a, 1
1:
    di
    ret
/* void platform_irq_restore(unsigned char enabled) */
_platform_irq_restore:
    ld hl, 3
    add hl, sp
    ld a, (hl)
    or a, a
    ret z
    ei
    ret
/* PRT1 at 1 kHz: acknowledge (reading TMR1_CTL clears PRT_IRQ), count the
 * 32-bit millisecond clock, then the preemption countdown (stage B2): once it
 * is 0, steal a non-zero fvm_budget into forth_budget_stolen, zero it and set
 * forth_tick_pending; repeat on every later tick until C plans again (a steal
 * lost to the engine's own budget store is retried). 24-bit stores are single
 * instructions. Uses AF, HL, DE on the interrupted stack (9 bytes + return). */
_vios_tick_isr:
    push af
    push hl
    in0 a, (0x83)
    ld hl, _vios_ms_clock
    inc (hl)
    jr nz, 2f
    inc hl
    inc (hl)
    jr nz, 2f
    inc hl
    inc (hl)
    jr nz, 2f
    inc hl
    inc (hl)
2:
    ld a, (_vios_preempt_armed)
    or a, a
    jr z, .Ltick_done
    push de
    ld de, 0
    ld hl, (_vios_preempt_count)
    or a, a
    sbc hl, de
    jr z, .Ltick_steal
    dec hl
    ld (_vios_preempt_count), hl
    or a, a
    sbc hl, de
    jr nz, .Ltick_pop
.Ltick_steal:
    ld hl, (_fvm_budget)
    or a, a
    sbc hl, de
    jr z, .Ltick_pop           /* nothing to steal: already stolen, or between runs */
    ld (_forth_budget_stolen), hl
    ld (_fvm_budget), de
    ld a, 1
    ld (_forth_tick_pending), a
.Ltick_pop:
    pop de
.Ltick_done:
    pop hl
    pop af
    ei
    reti.l
/* Every other vector: count it and return. All sources are disarmed before
 * interrupts are enabled, so this should stay at 0 (IRQ-STATS). */
_vios_stray_isr:
    push af
    push hl
    ld hl, _vios_irq_stray
    inc (hl)
    jr nz, 3f
    inc hl
    inc (hl)
3:
    pop hl
    pop af
    ei
    reti.l
/* ---- Stage B3: UART0 RX by interrupt (IVECT 18h) ----
 * Every byte in the FIFO goes into a 256-byte ring (256-aligned, so the
 * index is the low address byte); RTS drops when 192 are queued and C
 * raises it again below 64 (boards/agon-light-2/platform_resident.c). The
 * ISR only moves bytes: the VDP transport parser runs in C, which is not
 * reentrant. Reading RBR clears the receive/timeout interrupt. Uses AF, BC,
 * HL (12 bytes of stack with the return address). */
_vios_uart0_isr:
    push af
    push bc
    push hl
.Lrx_next:
    in0 a, (0xC5)              /* LSR */
    bit 1, a
    jr z, 1f
    ld hl, (_ez80_uart0_rx_overruns)
    inc hl
    ld (_ez80_uart0_rx_overruns), hl
1:
    bit 0, a
    jr z, .Lrx_done
    in0 b, (0xC0)
    ld a, (_vios_rx_head)
    ld c, a
    inc a
    ld hl, _vios_rx_tail
    cp a, (hl)
    jr z, .Lrx_drop            /* full: should not happen, RTS dropped at 192 */
    ld hl, _vios_rx_ring
    ld l, c
    ld (hl), b
    ld (_vios_rx_head), a
    ld hl, _vios_rx_tail
    sub a, (hl)
    cp a, 192
    jr c, .Lrx_next
    in0 a, (0xC4)
    and a, 0xFD
    out0 (0xC4), a             /* RTS off: the peer pauses */
    jr .Lrx_next
.Lrx_drop:
    ld hl, (_vios_rx_dropped)
    inc hl
    ld (_vios_rx_dropped), hl
    jr .Lrx_next
.Lrx_done:
    pop hl
    pop bc
    pop af
    ei
    reti.l
/* FIFO on with an 8-byte trigger (the character timeout reports fewer),
 * receive interrupt on, RTS up. Called with interrupts disabled. */
_vios_uart0_isr_enable:
    ld a, 0x81
    out0 (0xC2), a
    ld a, 1
    out0 (0xC1), a
    in0 a, (0xC4)
    or a, 2
    out0 (0xC4), a
    ret
.section .bss
.balign 256
_vios_rx_ring:
    .space 256
_vios_rx_head:
    .space 1
_vios_rx_tail:
    .space 1
_vios_rx_dropped:
    .space 3
_vios_ms_clock:
    .space 4
_vios_irq_stray:
    .space 2
_vios_preempt_armed:
    .space 1
_vios_preempt_count:
    .space 3

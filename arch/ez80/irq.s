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
 * 32-bit millisecond clock, return. Uses only AF and HL, on the interrupted
 * stack (6 bytes plus the 3-byte return address). */
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
.section .bss
_vios_ms_clock:
    .space 4
_vios_irq_stray:
    .space 2

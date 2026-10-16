/* Agon Light 2 / eZ80F92: TMR0 at 18.432 MHz / 64 = 288000 Hz.
 * See include/platform/timer.h and docs/research/vios-timer.md.
 */
.assume ADL = 1
.section .text
.global _platform_timer_init, _platform_timer_read
_platform_timer_init:
    /* A bootstrap may have gated TMR0's peripheral clock. */
    in0 a, (0xDC)
    and 0xFE
    out0 (0xDC), a
    xor a
    out0 (0x80), a
    /* Select system clock for timer 0; preserve sources for timers 1-3. */
    in0 a, (0x92)
    and 0xFC
    out0 (0x92), a
    /* Reload 0000 means a full 65536-count period, not a stopped timer. */
    xor a
    out0 (0x81), a
    out0 (0x82), a
    /* Continuous, divide by 64, reload/restart, enable; IRQ_EN remains 0. */
    ld a, 0x1B
    out0 (0x80), a
    ret
_platform_timer_read:
    /* Reading low latches high in hardware (PS015317 pp79,83-84).
     * C ABI: unsigned short in HL; clear HLU as well for safe widening.
     */
    ld hl, 0
    in0 l, (0x81)
    in0 h, (0x82)
    ret

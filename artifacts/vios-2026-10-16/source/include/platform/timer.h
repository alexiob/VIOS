#ifndef PLATFORM_TIMER_H
#define PLATFORM_TIMER_H

/* Agon Light 2: 18.432 MHz system clock / 64. */
#define PLATFORM_TIMER_HZ 288000UL
#define PLATFORM_TIMER_MODULUS 65536UL

/* Owns TMR0. Call once with interrupts disabled before taking samples. */
void platform_timer_init(void);

/* Free-running 16-bit DOWNCOUNTER, coherently latched by the low-byte read.
 * Elapsed ticks = (unsigned short)(start - finish), valid ONLY when the
 * entire interval is shorter than 65536 ticks (227.555556 ms).
 * No interrupt handler or concurrent reader may touch TMR0 between its two
 * register reads. Serial output belongs outside the measured interval.
 */
unsigned short platform_timer_read(void);

#endif

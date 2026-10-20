#ifndef PLATFORM_CONSOLE_H
#define PLATFORM_CONSOLE_H
void platform_console_init(void);
void platform_console_putc(unsigned char c);
int platform_console_getc(void); /* -1 when no complete character is available. */
void platform_console_line_mode(int awaiting_line); /* Resident VDP console only. */
int platform_console_quiet(void); /* Resident: admission gate for background tasks. */
int platform_console_interrupt(void); /* Resident: 1 once per Ctrl-C (keyboard or host) since the last call. */
void platform_console_tap(void (*tap)(unsigned char)); /* Resident: copy of console output. */
/* Resident: while sink is set, console output goes only to it (not USB, not
 * the VDP console); NULL restores normal output. Returns the previous sink. */
void (*platform_console_divert(void (*sink)(unsigned char)))(unsigned char);
#endif

#ifndef PLATFORM_CONSOLE_H
#define PLATFORM_CONSOLE_H
void platform_console_init(void);
void platform_console_putc(unsigned char c);
int platform_console_getc(void); /* -1 when no complete character is available. */
#endif

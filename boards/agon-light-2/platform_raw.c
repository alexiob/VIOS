/* Direct UART console for the temporary ESP32 USB/UART RAM bridge. */
#include "platform/console.h"
#include "platform/system.h"
#include "arch/ez80/uart0.h"

extern void ez80_uart0_raw_init(void);

void platform_console_init(void) { ez80_uart0_raw_init(); }
void platform_console_putc(unsigned char c) {
    while (!(ez80_uart0_status() & 0x20)) {}
    ez80_uart0_write(c);
}
int platform_console_getc(void) {
    if (!(ez80_uart0_status() & 1)) return -1;
    return ez80_uart0_read();
}
const char *platform_identity(void) {
    return "Agon Light 2 / eZ80 ADL / raw UART0 115200 / freestanding C";
}

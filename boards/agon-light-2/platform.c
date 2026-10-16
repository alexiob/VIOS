#include "platform/console.h"
#include "platform/system.h"
#include "drivers/vdp_keyboard.h"
#include "arch/ez80/uart0.h"

static struct vdp_keyboard keyboard;

void platform_console_putc(unsigned char c) {
    /* CTS register bit is the inverted active-low pin: set means ready. */
    while (!(ez80_uart0_modem() & 0x10)) {}
    while (!(ez80_uart0_status() & 0x20)) {}
    ez80_uart0_write(c);
}
void platform_console_init(void) {
    /* Preserve MOS's established UART baud and pin mux. Own reception now. */
    ez80_uart0_polling();
    platform_console_putc(23); platform_console_putc(0);
    platform_console_putc(254); platform_console_putc(1);
}
int platform_console_getc(void) {
    if (!(ez80_uart0_status() & 1)) return -1;
    return vdp_keyboard_feed(&keyboard, ez80_uart0_read());
}
const char *platform_identity(void) {
    return "Agon Light 2 / eZ80 ADL / polled UART0 / freestanding C";
}

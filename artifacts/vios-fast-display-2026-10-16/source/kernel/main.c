#include "kernel/monitor.h"
#include "platform/console.h"
void kernel_main(void) {
    platform_console_init();
    monitor_init();
    for (;;) {
        int c = platform_console_getc();
        if (c >= 0) monitor_feed((unsigned char)c);
    }
}

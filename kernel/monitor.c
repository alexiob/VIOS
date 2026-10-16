#include <stddef.h>
#include "kernel/monitor.h"
#include "platform/console.h"
#include "platform/system.h"

#ifndef MONITOR_BUILD
#define MONITOR_BUILD "development"
#endif

static char line[80];
static size_t used;
static unsigned char overflow, after_cr;

static void print(const char *s) {
    while (*s) platform_console_putc((unsigned char)*s++);
}
static int equal(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static void execute(void) {
    line[used] = '\0';
    if (!used) return;
    if (equal(line, "help"))
        print("help | ping | info | echo TEXT | reboot\r\n");
    else if (equal(line, "ping")) print("pong\r\n");
    else if (equal(line, "info")) {
        print(platform_identity()); print("\r\nbuild: " MONITOR_BUILD "\r\n");
    } else if (equal(line, "echo")) print("\r\n");
    else if (used >= 5 && line[0] == 'e' && line[1] == 'c' &&
             line[2] == 'h' && line[3] == 'o' && line[4] == ' ') {
        print(line + 5); print("\r\n");
    } else if (equal(line, "reboot")) {
        print("rebooting\r\n"); platform_restart();
    } else print("unknown command; try help\r\n");
}
void monitor_init(void) {
    used = 0; overflow = 0; after_cr = 0;
    print("\r\nagon-monitor " MONITOR_BUILD "\r\nagon> ");
}
void monitor_feed(unsigned char c) {
    if (c == '\n' && after_cr) { after_cr = 0; return; }
    after_cr = 0;
    if (c == '\r' || c == '\n') {
        after_cr = (c == '\r');
        print("\r\n");
        if (overflow) print("line too long; discarded\r\n");
        else execute();
        used = 0; overflow = 0;
        print("agon> ");
    } else if (c == 8 || c == 127) {
        if (!overflow && used) { --used; print("\b \b"); }
    } else if (c >= 32 && c < 127 && !overflow) {
        if (used + 1 < sizeof line) {
            line[used++] = (char)c; platform_console_putc(c);
        } else overflow = 1;
    }
}

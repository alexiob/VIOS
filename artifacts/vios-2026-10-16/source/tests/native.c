#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel/monitor.h"
#include "drivers/vdp_keyboard.h"
#include "platform/console.h"
#include "platform/system.h"

static char output[16384];
static size_t output_len;
static unsigned restarts;
void platform_console_putc(unsigned char c) {
    assert(output_len + 1 < sizeof output);
    output[output_len++] = (char)c;
    output[output_len] = '\0';
}
const char *platform_identity(void) { return "test-board"; }
void platform_restart(void) { ++restarts; }
static void clear_output(void) { output_len = 0; output[0] = '\0'; }
static void input(const char *s) { while (*s) monitor_feed((unsigned char)*s++); }
static unsigned occurrences(const char *s) {
    unsigned n = 0; const char *p = output;
    while ((p = strstr(p, s)) != NULL) { ++n; ++p; }
    return n;
}
static void monitor_tests(void) {
    unsigned i;
    monitor_init();
    assert(strstr(output, "agon-monitor") != NULL);
    clear_output(); input("ping\r\n");
    assert(strstr(output, "pong\r\n") != NULL);
    assert(occurrences("agon> ") == 1); /* CRLF must execute once. */
    clear_output(); input("echo hello C\n");
    assert(strstr(output, "\r\nhello C\r\n") != NULL);
    clear_output(); input("pinx\bg\r");
    assert(strstr(output, "pong\r\n") != NULL);
    clear_output(); input("pinx\177g\r");
    assert(strstr(output, "pong\r\n") != NULL);
    clear_output(); input("info\r");
    assert(strstr(output, "test-board") != NULL);
    clear_output(); input("help\r");
    assert(strstr(output, "reboot") != NULL);
    clear_output(); input("pingx\r");
    assert(strstr(output, "unknown command") != NULL);
    assert(strstr(output, "pong") == NULL);
    clear_output(); input("\r");
    assert(strstr(output, "unknown") == NULL);
    clear_output();
    for (i = 0; i < 400; ++i) monitor_feed('x');
    input("\b\bping\r");
    assert(strstr(output, "line too long") != NULL);
    assert(strstr(output, "pong") == NULL); /* Discard the whole invalid line. */
    clear_output(); input("ping\r");
    assert(strstr(output, "pong") != NULL); /* Parser recovers next line. */
    input("reboot\r"); assert(restarts == 1);
}
static int frame(struct vdp_keyboard *p, const unsigned char *b, size_t n) {
    int c = -1;
    while (n--) { int x = vdp_keyboard_feed(p, *b++); if (x >= 0) c = x; }
    return c;
}
static void vdp_tests(void) {
    struct vdp_keyboard p = {0};
    const unsigned char key[] = {0x81,4,'p',0,0,1};
    const unsigned char up[] = {0x81,4,'p',0,0,0};
    const unsigned char unrelated[] = {0x82,4,'X',0,0,1};
    const unsigned char short_key[] = {0x81,2,'Z',1};
    const unsigned char extended[] = {0x81,6,'q',0,0,1,0x81,0x80};
    const unsigned char noise[] = {'x','y',0x80,0};
    assert(frame(&p, noise, sizeof noise) == -1);
    assert(frame(&p, key, sizeof key) == 'p');
    assert(frame(&p, up, sizeof up) == -1);
    assert(frame(&p, unrelated, sizeof unrelated) == -1);
    assert(frame(&p, short_key, sizeof short_key) == -1);
    assert(frame(&p, extended, sizeof extended) == 'q');
    assert(frame(&p, key, sizeof key) == 'p');
}
int main(void) {
    monitor_tests(); vdp_tests();
    puts("native behavior checks passed");
    return 0;
}

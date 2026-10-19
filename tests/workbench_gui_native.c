/* Differential test of the workbench text screen backends. The same scripted
 * workbench session runs against text_screen.c (VDU, CPU planes) or, built
 * with -DVIOS_GUI, against text_screen_vds.c driving the real ESP display
 * server in-process. Each checkpoint prints the 80x60 screen as read through
 * vios_text_cell; the Python test requires identical output from both.
 * Usage: workbench_gui [FONT.bin]   (the font is needed only with VIOS_GUI) */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vios/workbench.h"
#include "vios/text_screen.h"
#ifdef VIOS_GUI
#include "vios/ui.h"
#include "vds_proto.h"
#define W 640
#define H 480
static uint8_t fb_a[H][W / 2], fb_b[H][W / 2], *rows_a[H], *rows_b[H], font[256 * 8];
static vds_cell cells[VDS_MAX_SURFACES][80 * 60];
static vds v;
static vds_proto p;
static uint8_t reply[32], reply_n, reply_valid;
static vds_cell *cell_mem(void *ctx, uint8_t id, uint16_t n) { (void)ctx; return id < VDS_MAX_SURFACES && n <= 80 * 60 ? cells[id] : 0; }
static void esp_send(void *ctx, uint8_t code, const uint8_t *d, uint8_t n) { (void)ctx; (void)code; memcpy(reply, d, n); reply_n = n; reply_valid = 1; }
static uint8_t vdu[300];
static unsigned vdu_n, payload_bytes;
void platform_vdp_send(const void *data, size_t length) {
    const uint8_t *b = data;
    for (size_t i = 0; i < length; ++i) {
        vdu[vdu_n++] = b[i];
        assert(vdu[0] == 23 && (vdu_n < 2 || vdu[1] == 0) && (vdu_n < 3 || vdu[2] == 0xE4));   /* only E4 batches */
        if (vdu_n >= 4 && vdu_n == 4u + vdu[3]) {
            assert(vds_proto_batch(&p, vdu + 4, vdu[3]) && !p.errors);
            payload_bytes += vdu[3];
            vdu_n = 0;
        }
    }
}
int platform_vdp_request(const void *command, size_t length, unsigned char type, void *data, size_t capacity, unsigned *actual) {
    reply_valid = 0;
    platform_vdp_send(command, length);
    if (type != 0xF1 || !reply_valid || reply_n > capacity) { *actual = 0; return 0; }
    memcpy(data, reply, reply_n);
    *actual = reply_n;
    return 1;
}
#else
void platform_vdp_send(const void *data, size_t length) { (void)data; (void)length; }
#endif
void platform_console_putc(unsigned char c) { (void)c; }
int platform_console_getc(void) { return -1; }
void platform_console_init(void) {}
const char *platform_identity(void) { return "test"; }
void platform_restart(void) {}

static void drain(unsigned budget) {
    unsigned guard = 0;
#ifdef VIOS_GUI
    if (budget < VIOS_TEXT_BATCH_BYTES) budget = VIOS_TEXT_BATCH_BYTES;  /* ops are never split; the runtime uses 128 */
#endif
    while (vios_text_pending()) {
        unsigned n = vios_text_render(budget);
        assert(n <= budget && ++guard < 100000);
    }
}

static void checkpoint(const char *name) {
    printf("== %s\n", name);
    for (unsigned y = 0; y < 60; ++y) {
        struct vios_text_cell row[80];
        for (unsigned x = 0; x < 80; ++x) assert(vios_text_cell(x, y, &row[x]));
        for (unsigned x = 0; x < 80; ++x) putchar(row[x].ch);
        putchar('|');
        for (unsigned x = 0; x < 80; ++x) printf("%02x", row[x].attr);
        putchar('\n');
    }
#ifdef VIOS_GUI
    /* The server's composited framebuffer is exact for what it holds. */
    vds_window_open(&v, 26, VIOS_GUI_SCREEN_SURFACE, 0, 0, 80, 60, VDS_W_VISIBLE, "");
    for (int i = 0; i < 100 && !vds_frame(&v, UINT32_MAX); ++i) {}
    v.rows = rows_b; vds_damage_all(&v); for (int i = 0; i < 100 && !vds_frame(&v, UINT32_MAX); ++i) {} v.rows = rows_a;
    assert(!memcmp(fb_a, fb_b, sizeof fb_a));
    vds_window_close(&v, 26);
#endif
}

static struct vios_workbench_status status(void) { struct vios_workbench_status s; vios_workbench_status(&s); return s; }

int main(int argc, char **argv) {
    (void)argc; (void)argv;
#ifdef VIOS_GUI
    assert(argc >= 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f && fread(font, 1, sizeof font, f) == sizeof font);
    fclose(f);
    for (int y = 0; y < H; ++y) { rows_a[y] = fb_a[y]; rows_b[y] = fb_b[y]; }
    assert(vds_init(&v, rows_a, W, H, font));
    vds_proto_init(&p, &v, cell_mem, esp_send, 0);
#endif
    forth_init();
    vios_text_reset();
    vios_workbench_init();
    vios_workbench_open();
    assert(vios_workbench_new("integration", "1", 0));
    char source[3000];
    unsigned n = 0;
    for (unsigned i = 0; i < 65; ++i) n += (unsigned)snprintf(source + n, sizeof source - n, "row %02u\tcontent %u\r\n", i, i * 7);
    assert(vios_workbench_patch(status().revision, 0, 0, source, n));
    vios_workbench_compose(); drain(128); checkpoint("opened");
    vios_workbench_key(9, 0); vios_workbench_compose(); drain(8); checkpoint("editor focus");
    for (int i = 0; i < 5; ++i) { vios_workbench_key(VIOS_WB_UP, 0); vios_workbench_compose(); drain(16); }
    checkpoint("scrolled up");
    for (int i = 0; i < 70; ++i) vios_workbench_key(VIOS_WB_DOWN, 0);
    vios_workbench_compose(); drain(128); checkpoint("bottom");
    vios_workbench_key(VIOS_WB_HOME, 0); vios_workbench_key(13, 0); vios_workbench_key('x', 0);
    vios_workbench_compose(); drain(8); checkpoint("edited");
    assert(vios_text_render(5) <= 5);                          /* a partial render, then more change */
    vios_workbench_zoom(1); vios_workbench_compose(); drain(64); checkpoint("zoomed");
    for (int i = 0; i < 30; ++i) vios_workbench_key(VIOS_WB_UP, 0);
    vios_workbench_compose(); drain(128); checkpoint("zoomed scroll");
    vios_workbench_zoom(0); vios_workbench_key(9, 0); vios_workbench_key(VIOS_WB_DOWN, 0);
    vios_workbench_compose(); drain(128); checkpoint("browser");
    vios_text_begin_frame(); vios_workbench_compose(); drain(128); checkpoint("full frame");
    vios_text_reset(); vios_workbench_compose(); drain(128); checkpoint("after reset");
#ifdef VIOS_GUI
    fprintf(stderr, "payload bytes %u\n", payload_bytes);
#endif
    printf("workbench screen session done\n");
    return 0;
}

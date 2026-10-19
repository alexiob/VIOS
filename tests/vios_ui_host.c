/* End-to-end host test of the GUI stack without a board: Forth UI words
 * (kernel/vios_ui.c) on the real Forth system send VDU 23,0,E4 batches through
 * a stubbed VDP link into the real ESP display server (vds_proto, vds_ui,
 * vds); replies and polled events travel back the same way.
 * Usage: vios_ui_host FONT.bin */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "forth/system.h"
#include "vios/ui.h"
#include "vds_ui.h"

#define W 640
#define H 480
static uint8_t fb_a[H][W / 2], fb_b[H][W / 2], *rows_a[H], *rows_b[H];
static uint8_t font[256 * 8];
static vds_cell cells_a[VDS_MAX_SURFACES][80 * 60], cells_b[VDS_MAX_SURFACES][80 * 60];
static vds va, vb;
static vds_proto p;
static vds_ui u;

/* ESP side: surface memory and packets back to the CPU. */
static vds_cell *cell_mem(void *ctx, uint8_t id, uint16_t n) { (void)ctx; return id < VDS_MAX_SURFACES && n <= 80 * 60 ? cells_a[id] : 0; }
static uint8_t reply[32], reply_n, reply_valid;
static void esp_send(void *ctx, uint8_t code, const uint8_t *d, uint8_t n) {
    (void)ctx;
    assert(code == VDS_PACKET_REPLY && n <= 32);       /* events are pulled, never pushed */
    memcpy(reply, d, n); reply_n = n; reply_valid = 1;
}

/* CPU side platform stubs: a VDU byte stream parsed for 23,0,E4,len,payload. */
static uint8_t vdu[300];
static unsigned vdu_n, e4_batches, other_bytes;
void platform_vdp_send(const void *data, size_t length) {
    const uint8_t *b = data;
    for (size_t i = 0; i < length; ++i) {
        vdu[vdu_n++] = b[i];
        if (vdu_n >= 4 && vdu[0] == 23 && vdu[1] == 0 && vdu[2] == 0xE4 && vdu_n == 4u + vdu[3]) {
            assert(vds_proto_batch(&p, vdu + 4, vdu[3]));
            ++e4_batches;
            vdu_n = 0;
        } else if (vdu_n >= 3 && !(vdu[0] == 23 && vdu[1] == 0 && vdu[2] == 0xE4)) { ++other_bytes; vdu_n = 0; }
        assert(vdu_n < sizeof vdu);
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
void platform_console_putc(unsigned char c) { (void)c; }
int platform_console_getc(void) { return -1; }
void platform_console_flush(void) {}

static void eval(const char *s) {
    if (!forth_eval(s)) { fprintf(stderr, "eval failed: %s: %s\n", s, forth_last_error()); abort(); }
}
static void fails(const char *s) { assert(!forth_eval(s)); fvm_reset(); }
static void finish(vds *v) { for (int i = 0; i < 1000; ++i) if (vds_frame(v, UINT32_MAX)) return; assert(0); }
static void same_screen(void) {
    vds_proto_frame(&p, 40, UINT32_MAX);
    finish(&va); finish(&vb);
    for (int y = 0; y < H; ++y) if (memcmp(fb_a[y], fb_b[y], W / 2)) { fprintf(stderr, "Forth and API screens differ, row %d\n", y); abort(); }
}

int main(int argc, char **argv) {
    assert(argc >= 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f && fread(font, 1, sizeof font, f) == sizeof font);
    fclose(f);
    for (int y = 0; y < H; ++y) { rows_a[y] = fb_a[y]; rows_b[y] = fb_b[y]; }
    assert(vds_init(&va, rows_a, W, H, font) && vds_init(&vb, rows_b, W, H, font));
    vds_proto_init(&p, &va, cell_mem, esp_send, 0);
    vds_ui_init(&u, &p);
    forth_init();
    assert(vios_ui_init());

    /* A window with source text, built from Forth ... */
    eval("0 80 20 48 UI-SURFACE");
    eval("S\" : SQUARE ( n -- n*n ) DUP * ;\" 0 1 1 48 UI-TYPE");
    eval("S\" \\ a line longer than one 64-character op is sent as a PUT plus MORE runs!\" 0 1 3 48 UI-TYPE");
    eval("0 0 5 10 2 46 48 UI-FILL  0 2 1 1 1 UI-CARET");
    eval("1 0 61 40 60 20 UI-VISIBLE UI-DECOR OR UI-SHADOW OR UI-WINDOW  S\" Browser\" 1 UI-TITLE  1 UI-FOCUS");
    eval("15 UI-ARROW  15 100 70 UI-SPRITE-AT  15 TRUE UI-SPRITE-SHOW");
    assert(!fvm_depth && !p.errors && other_bytes == 0);
    /* ... and through the API. */
    const char *long_line = "\\ a line longer than one 64-character op is sent as a PUT plus MORE runs!";
    assert(vds_surface_init(&vb, 0, 80, 20, cells_b[0], 48));
    vds_put(&vb, 0, 1, 1, 48, ": SQUARE ( n -- n*n ) DUP * ;", 29);
    vds_put(&vb, 0, 1, 3, 48, long_line, strlen(long_line));
    vds_fill(&vb, 0, 0, 5, 10, 2, '.', 48);
    vds_cursor(&vb, 0, 2, 1, 1, 1);
    vds_window_open(&vb, 1, 0, 61, 40, 60, 20, VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW | VDS_W_ACTIVE, "Browser");
    vds_sprite_define(&vb, 15, VDS_ARROW_W, VDS_ARROW_H, 0, 0, VDS_ARROW_TRANSPARENT, vds_arrow_cursor);
    vds_sprite_move(&vb, 15, 100, 70);
    vds_sprite_show(&vb, 15, 1);
    same_screen();

    /* UI-BEGIN .. UI-COMMIT is one E4 payload, held until its COMMIT. */
    unsigned before = e4_batches;
    eval("UI-BEGIN  S\" one\" 0 1 7 48 UI-TYPE  S\" two\" 0 1 8 48 UI-TYPE  S\" three\" 0 1 9 48 UI-TYPE  UI-COMMIT");
    assert(e4_batches == before + 1 && !p.hold);
    vds_put(&vb, 0, 1, 7, 48, "one", 3); vds_put(&vb, 0, 1, 8, 48, "two", 3); vds_put(&vb, 0, 1, 9, 48, "three", 5);
    same_screen();

    /* UI-SYNC answers from the server. */
    eval("UI-SYNC");
    fcell errors = fvm_pop(), generation = fvm_pop(), epoch = fvm_pop();
    assert(errors == 0 && generation == p.generation - 1 && epoch == p.epoch);

    /* Commands and a menu from Forth; a key on the ESP becomes a Forth event. */
    eval("S\" &Save\" 3 UI-ENABLED UI-STATUS OR 0 UI-CTRL CHAR s UI-COMMAND");
    eval("S\" &Open...\" 2 UI-ENABLED 162 0 0 UI-COMMAND");
    eval("3 2 2 S\" &File\" 1 UI-MENU  1 UI-SUBMENU 1 S\" \" 0 UI-MENU");
    assert(!p.errors && u.menus[1].count == 2 && u.menus[0].items[0] == (VDS_ITEM_SUBMENU | 1));
    eval("UI-NEXT");                                                /* the earlier UI-FOCUS */
    assert(fvm_pop() == FVM_MASK && fvm_pop() == VDS_NONE && fvm_pop() == 1 && fvm_pop() == 6);
    eval("UI-NEXT");
    assert(fvm_pop() == 0);                                         /* nothing pending */
    assert(vds_ui_key(&u, 0x13, VDS_MOD_CTRL, 0, 1));              /* Ctrl+S on the board */
    eval("UI-NEXT");
    assert(fvm_pop() == FVM_MASK && fvm_pop() == 3 && fvm_pop() == 1 && fvm_pop() == 17);  /* COMMAND 3, window 1 */
    assert(vds_ui_key(&u, 0, 0, VDS_VK_F10, 1) && vds_ui_key(&u, 'f', 0, 0, 1) && vds_ui_key(&u, 'o', 0, 0, 1));  /* world > File > Open */
    eval("CREATE EV 32 ALLOT  EV UI-EVENT");
    fcell len = fvm_pop();
    assert(len == 6);
    eval("EV 1+ C@  EV 3 + C@");
    assert(fvm_pop() == 2 && fvm_pop() == 17);
    /* Many events at once: pulled in several replies, none lost. */
    for (int i = 0; i < 12; ++i) assert(vds_ui_key(&u, 0x13, VDS_MOD_CTRL, 0, 1));
    unsigned got = 0;
    for (int i = 0; i < 14; ++i) {
        eval("UI-NEXT");
        if (!fvm_pop()) break;
        fvm_pop(); fvm_pop(); fvm_pop();
        ++got;
    }
    assert(got == 12);

    /* Animation and the pointer from Forth. */
    eval("UI-TWEEN-WINDOW 1 UI-Y 40 120 100 UI-LINEAR UI-ANIMATE");
    vds_proto_frame(&p, 50, UINT32_MAX);
    assert(va.windows[1].y == 80);
    vds_proto_frame(&p, 50, UINT32_MAX);
    assert(va.windows[1].y == 120);
    eval("UI-NEXT");
    assert(fvm_pop() == FVM_MASK && fvm_pop() == (0 | 1 << 8) && fvm_pop() == 1 && fvm_pop() == 18);  /* ANIMDONE window 1, prop Y */
    eval("200 150 1 UI-POINTER 200 150 0 UI-POINTER");
    eval("UI-NEXT");
    assert(fvm_pop() == FVM_MASK && fvm_pop() == 200 && fvm_pop() == 1 && fvm_pop() == 7);            /* POINTER over window 1 */
    eval("UI-NEXT");
    assert(fvm_pop() == FVM_MASK);
    fvm_pop(); fvm_pop(); fvm_pop();
    fails("2 1 0 0 1 10 0 UI-ANIMATE");
    fails("0 1 0 0 1 10 4 UI-ANIMATE");

    /* Widgets from Forth: a column with an input and a button. */
    eval("0 1 UI-BOX UI-NONE 0 0 0 0 UI-WF-FILL UI-WIDGET  0 UI-COLUMN 1 UI-LAYOUT");
    eval("1 1 UI-INPUT 0 0 0 0 0 0 UI-WIDGET  1 1 0 UI-WSIZE");
    eval("2 1 UI-BUTTON 0 0 0 0 0 0 UI-WIDGET  2 1 0 UI-WSIZE  S\" &Go\" 2 UI-WTEXT");
    eval("3 1 UI-LIST 0 0 0 0 0 0 UI-WIDGET  3 2 UI-ITEMS  S\" alpha\" 3 0 UI-ITEM  S\" beta\" 3 1 UI-ITEM");
    assert(!p.errors && u.widgets[1].row == 0 && u.widgets[2].row == 2 && u.widgets[3].row == 4 && u.widgets[3].count == 2);
    assert(vds_ui_key(&u, 'h', 0, 0, 1) && vds_ui_key(&u, 'i', 0, 0, 1));           /* typing into the input */
    eval("CREATE TXT 32 ALLOT  1 TXT UI-WGET");
    assert(fvm_pop() == 2 && fvm_pop() == 2);                                      /* length 2, caret 2 */
    eval("TXT C@ TXT 1+ C@");
    assert(fvm_pop() == 'i' && fvm_pop() == 'h');
    for (int i = 0; i < 2; ++i) { eval("UI-NEXT"); assert(fvm_pop() == FVM_MASK && fvm_pop() == (fcell)(i + 1) && fvm_pop() == 1 && fvm_pop() == 4); }
    assert(vds_ui_key(&u, 9, 0, 142, 1) && vds_ui_key(&u, ' ', 0, 1, 1));          /* Tab to Go, press */
    eval("UI-NEXT");
    assert(fvm_pop() == FVM_MASK && fvm_pop() == 1 && fvm_pop() == 2 && fvm_pop() == 2);   /* ACTIVATE widget 2 in window 1 */
    eval("1 UI-WDELETE 0 UI-WDELETE");
    assert(!u.widgets[0].used && !u.widgets[2].used);
    fails("48 1 UI-LABEL UI-NONE 0 0 1 1 0 UI-WIDGET");
    fails("5 1 9 UI-NONE 0 0 1 1 0 UI-WIDGET");
    fails("5 TXT UI-WGET");

    /* Turbo Vision from Forth: the menu bar and the status line appear. */
    eval("1 UI-STYLE");
    assert(u.bar_shown && u.status_shown);
    eval("0 UI-STYLE 1 UI-SNAP 8 UI-SNAP");
    assert(!u.bar_shown && va.snap_x == 8);

    /* Argument checks fail in Forth, before anything reaches the wire. */
    unsigned sent = e4_batches, errors_before = p.errors;
    fails("11 10 10 0 UI-SURFACE");
    fails("27 0 0 0 10 10 1 UI-WINDOW");
    fails("0 81 10 0 UI-SURFACE");
    fails("2 UI-STYLE");
    fails("4 UI-SNAP");
    fails("16 UI-ARROW");
    fails("S\" a title that is far too long for a window\" 1 UI-TITLE");
    fails("0 0 0 200 1 UI-SCROLL");
    assert(e4_batches == sent && p.errors == errors_before);

    /* Reset clears the server and the CPU's event ring. */
    eval("UI-RESET");
    assert(!va.count && !u.menus[0].used);
    eval("UI-SYNC");
    assert(fvm_pop() == 0 && (fvm_pop(), fvm_pop() == p.epoch));
    printf("vios ui checks passed: %u E4 batches\n", e4_batches);
    return 0;
}

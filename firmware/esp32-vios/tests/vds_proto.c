/* Host test of the VDS wire protocol (lib/vios-vds/src/vds_proto.c): E4
 * batches build the same screen as direct API calls; malformed batches are
 * rejected without side effects; replies, events, hold/commit, raw mode,
 * reset and sprite upload; the inspection ops (94-98) /dev/win reads with; random
 * batches never crash. Usage: vds_proto FONT.bin */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vds_ui.h"

#define W 640
#define H 480
static uint8_t fb_a[H][W / 2], fb_b[H][W / 2], *rows_a[H], *rows_b[H];
static uint8_t font[256 * 8];
static vds_cell cells_a[VDS_MAX_SURFACES][80 * 60], cells_b[VDS_MAX_SURFACES][80 * 60];
static vds va, vb;
static vds_proto p;

static vds_cell *cells(void *ctx, uint8_t id, uint16_t count) {
    (void)ctx;
    return id < VDS_MAX_SURFACES && count <= 80 * 60 ? cells_a[id] : 0;
}

static uint8_t last_code, last[32], last_n;
static unsigned events, replies, error_events;
static void send(void *ctx, uint8_t code, const uint8_t *data, uint8_t n) {
    (void)ctx;
    assert(n <= 30);                      /* fits the CPU's 32-byte packet slots */
    last_code = code; last_n = n; memcpy(last, data, n);
    if (code == VDS_PACKET_EVENT) { ++events; if (data[1] == VDS_EVENT_ERROR) ++error_events; }
    else ++replies;
}

static void finish(vds *v) { for (int i = 0; i < 1000; ++i) if (vds_frame(v, UINT32_MAX)) return; assert(0); }
static void same_screen(void) {
    finish(&va); finish(&vb);
    for (int y = 0; y < H; ++y)
        if (memcmp(fb_a[y], fb_b[y], W / 2)) { fprintf(stderr, "protocol and API screens differ on row %d\n", y); abort(); }
}

/* Batch builder. */
static uint8_t batch[255];
static size_t blen;
static void begin(void) { blen = 0; }
static void bytes(const void *d, size_t n) { assert(blen + n <= sizeof batch); memcpy(batch + blen, d, n); blen += n; }
static void b8(unsigned x) { uint8_t c = (uint8_t)x; bytes(&c, 1); }
static void b16(int x) { b8((unsigned)x & 255); b8(((unsigned)x >> 8) & 255); }
static int send_batch(void) { return vds_proto_batch(&p, batch, blen); }

/* Yield hook: called between ops, always inside the batch (generation unchanged). */
static unsigned yields;static uint16_t yield_generation;
static void count_yield(void *ctx) { (void)ctx; assert(p.generation == yield_generation); ++yields; }

static unsigned seed = 99;
static unsigned rnd(unsigned n) { seed = seed * 1103515245u + 12345u; return (seed >> 16) % n; }

int main(int argc, char **argv) {
    assert(argc >= 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f && fread(font, 1, sizeof font, f) == sizeof font);
    fclose(f);
    for (int y = 0; y < H; ++y) { rows_a[y] = fb_a[y]; rows_b[y] = fb_b[y]; }
    assert(vds_init(&va, rows_a, W, H, font) && vds_init(&vb, rows_b, W, H, font));
    vds_proto_init(&p, &va, cells, send, 0);
    p.push_events = 1;                                  /* these checks watch events as they happen */

    /* One scene through E4 batches ... */
    begin();
    b8(0x01); b8(0); b8(40); b8(12); b8(0x30);                       /* surface 0: 40x12, ink 0 on 3 */
    b8(0x10); b8(0); b8(1); b8(1); b8(0x30); b8(5); bytes("HELLO", 5);
    b8(0x11); b8(0x03); b8(6); bytes(" WORLD", 6);                   /* MORE: inverted run continues */
    b8(0x12); b8(0); b8(1); b8(3); b8(2); b8('O'); b8(0x31); b8('K'); b8(0x13);
    b8(0x13); b8(0); b8(0); b8(5); b8(10); b8(2); b8('.'); b8(0x30);
    b8(0x15); b8(0); b8(2); b8(3); b8(VDS_CURSOR_BLOCK); b8(0);
    b8(0x50); b8(1); b8(0); b16(61); b16(50); b8(40); b8(12); b8(VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW | VDS_W_ACTIVE);
    b8(0x54); b8(1); b8(7); bytes("Browser", 7);
    b8(0x7A); b8(VDS_CURSOR_SPRITE); b8(0x78); b8(VDS_CURSOR_SPRITE); b16(100); b16(70); b8(0x79); b8(VDS_CURSOR_SPRITE); b8(1);
    b8(0x30);
    assert(send_batch() && !error_events);
    /* ... and through the API. */
    assert(vds_surface_init(&vb, 0, 40, 12, cells_b[0], 0x30));
    vds_put(&vb, 0, 1, 1, 0x30, "HELLO", 5);
    vds_put(&vb, 0, 6, 1, 0x03, " WORLD", 6);
    vds_put(&vb, 0, 1, 3, 0x31, "O", 1); vds_put(&vb, 0, 2, 3, 0x13, "K", 1);
    vds_fill(&vb, 0, 0, 5, 10, 2, '.', 0x30);
    vds_cursor(&vb, 0, 2, 3, VDS_CURSOR_BLOCK, 0);
    vds_window_open(&vb, 1, 0, 61, 50, 40, 12, VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW | VDS_W_ACTIVE, "Browser");
    vds_sprite_define(&vb, VDS_CURSOR_SPRITE, VDS_ARROW_W, VDS_ARROW_H, 0, 0, VDS_ARROW_TRANSPARENT, vds_arrow_cursor);
    vds_sprite_move(&vb, VDS_CURSOR_SPRITE, 100, 70);
    vds_sprite_show(&vb, VDS_CURSOR_SPRITE, 1);
    assert(vds_proto_frame(&p, 16, UINT32_MAX));
    same_screen();

    /* Replies: SYNC, GET, STATS. */
    begin(); b8(0x31); b8(7); assert(send_batch());
    assert(last_code == VDS_PACKET_REPLY && last[0] == 0x31 && last[1] == 7 && last[6] == 0 && last[7] == 0 && last[8] == 0);
    uint16_t gen = (uint16_t)(last[4] | last[5] << 8);
    begin(); b8(0x32); b8(0); b8(1); b8(1); b8(5); assert(send_batch());
    assert(last[0] == 0x32 && last[1] == 5 && last_n == 12 && !memcmp((char[]){'H', 0x30, 'E', 0x30, 'L', 0x30, 'L', 0x30, 'O', 0x30}, last + 2, 10));
    begin(); b8(0x33); b8(2); assert(send_batch());
    assert(last[0] == 0x33 && last_n == 10 && last[7] == 1 && last[8] == 1);   /* 1 window, 1 sprite */

    /* Malformed batches change nothing and report a desync. */
    uint8_t before[H][W / 2];
    memcpy(before, fb_a, sizeof before);
    unsigned errs = error_events;
    begin(); b8(0x13); b8(0); b8(0); b8(0); b8(5); b8(5); b8('#'); b8(0x30); b8(0x10); b8(0); b8(0); b8(0); b8(0x30); b8(9); bytes("SHORT", 5);
    assert(!send_batch() && error_events == errs + 1);
    begin(); b8(0x13); b8(0); b8(0); b8(0); b8(5); b8(5); b8('#'); b8(0x30); b8(0xEE);
    assert(!send_batch() && error_events == errs + 2);
    assert(vds_proto_frame(&p, 40, UINT32_MAX));
    assert(!memcmp(before, fb_a, sizeof before));
    begin(); b8(0x31); b8(8); assert(send_batch());
    assert(last[7] == 1 && (uint16_t)(last[4] | last[5] << 8) == gen + 3);    /* desync seen; GET/STATS batches counted */
    begin(); b8(0x31); b8(9); assert(send_batch()); assert(last[7] == 0);    /* cleared once reported */

    /* A well-formed op on something absent is skipped; the rest applies. */
    errs = error_events;
    begin(); b8(0x52); b8(9); b8(0x10); b8(0); b8(1); b8(7); b8(0x30); b8(2); bytes("OK", 2); b8(0x30);
    assert(send_batch() && error_events == errs + 1);
    vds_put(&vb, 0, 1, 7, 0x30, "OK", 2);
    assert(vds_proto_frame(&p, 16, UINT32_MAX));
    same_screen();

    /* Hold: nothing shows before COMMIT, unless the client stalls 33 ms. */
    memcpy(before, fb_a, sizeof before);
    begin(); b8(0x10); b8(0); b8(1); b8(9); b8(0x30); b8(4); bytes("HOLD", 4); assert(send_batch());
    vds_put(&vb, 0, 1, 9, 0x30, "HOLD", 4);
    assert(!vds_proto_frame(&p, 16, UINT32_MAX) && !memcmp(before, fb_a, sizeof before));
    assert(vds_proto_frame(&p, 20, UINT32_MAX));                              /* 36 ms: shown anyway */
    same_screen();
    begin(); b8(0x10); b8(0); b8(10); b8(9); b8(0x30); b8(1); b8('!'); b8(0x30); assert(send_batch());
    vds_put(&vb, 0, 10, 9, 0x30, "!", 1);
    assert(vds_proto_frame(&p, 1, UINT32_MAX));                               /* committed: shown at once */
    same_screen();

    /* Raw mode: the compositor leaves the framebuffer alone, then repaints. */
    begin(); b8(0x35); b8(1); b8(0x56); b8(1); b8(0); b8(1); b8(0x30); assert(send_batch());
    memcpy(before, fb_a, sizeof before);
    assert(vds_proto_frame(&p, 16, UINT32_MAX) && !memcmp(before, fb_a, sizeof before));
    memset(fb_a, 0x77, sizeof fb_a);                                          /* legacy VDU scribbles */
    begin(); b8(0x35); b8(0); b8(0x30); assert(send_batch());
    vds_window_scroll(&vb, 1, 0, 1);
    assert(vds_proto_frame(&p, 16, UINT32_MAX));
    same_screen();

    /* Sprite upload: define, pixels, move, show; config: style and snap. */
    begin(); b8(0x76); b8(2); b8(4); b8(2); b8(0); b8(0); b8(15);
    b8(0x77); b8(2); b16(0); b8(4); b8(0x12); b8(0x3F); b8(0xF4); b8(0x56);
    b8(0x78); b8(2); b16(300); b16(200); b8(0x79); b8(2); b8(1);
    b8(0x74); b8(2); b16(VDS_STYLE_TURBO); b8(0x74); b8(1); b16(1); b8(0x30);
    assert(send_batch() && error_events == errs + 1);
    static const uint8_t px2[4] = {0x12, 0x3F, 0xF4, 0x56};
    vds_sprite_define(&vb, 2, 4, 2, 0, 0, 15, px2);
    vds_sprite_move(&vb, 2, 300, 200); vds_sprite_show(&vb, 2, 1);
    vds_set_style(&vb, VDS_STYLE_TURBO); vds_set_snap(&vb, 1);
    assert(vds_proto_frame(&p, 16, UINT32_MAX));
    same_screen();
    errs = error_events;
    begin(); b8(0x77); b8(2); b16(3); b8(2); b8(0); b8(0); assert(send_batch() && error_events == errs + 1);  /* past the end */
    begin(); b8(0x74); b8(1); b16(4); assert(send_batch() && error_events == errs + 2);                    /* bad snap */

    /* Reset drops everything and bumps the epoch. */
    begin(); b8(0x31); b8(1); assert(send_batch());
    uint16_t epoch = (uint16_t)(last[2] | last[3] << 8);
    begin(); b8(0x34); b8(0x30); assert(send_batch());
    assert(!va.count && !va.sprites[2].used && !va.surfaces[0].cells && va.style.kind == VDS_STYLE_SMALLTALK);
    begin(); b8(0x31); b8(2); assert(send_batch());
    assert((uint16_t)(last[2] | last[3] << 8) == epoch + 1);

    /* Pull mode (the board default): events queue until the CPU polls. */
    p.push_events = 0;
    for (uint8_t i = 0; i < 5; ++i) vds_proto_event(&p, 0x11, i, (const uint8_t[]){i, 0, 2}, 3);
    unsigned before_events = events;
    begin(); b8(0x36); b8(255); assert(send_batch());
    assert(events == before_events && last_code == VDS_PACKET_REPLY && last[0] == 0x36);
    assert(last[2] == 3 && last[1] == 2 && last_n == 3 + 3 * 7);              /* 3 x (len + 6 bytes) fit in 30 */
    assert(last[3] == 6 && last[5] == 0x11 && last[6] == 0 && last[3 + 7 + 3] == 1);
    begin(); b8(0x36); b8(255); assert(send_batch());
    assert(last[2] == 2 && last[1] == 0 && last[6] == 3 && last[13] == 4);
    begin(); b8(0x36); b8(255); assert(send_batch());
    assert(last[2] == 0 && last_n == 3);
    vds_proto_event(&p, 0x11, 1, 0, 0); vds_proto_event(&p, 0x11, 2, 0, 0);
    begin(); b8(0x36); b8(1); assert(send_batch());
    assert(last[2] == 1 && last[1] == 1);                                      /* the CPU's limit is honoured */
    begin(); b8(0x36); b8(0); assert(send_batch());
    assert(last[2] == 0 && last[1] == 1);
    begin(); b8(0x36); b8(9); assert(send_batch());
    for (unsigned i = 0; i < VDS_EVENT_QUEUE + 3; ++i) vds_proto_event(&p, 0x11, (uint8_t)i, 0, 0);
    begin(); b8(0x36); b8(255); assert(send_batch());
    assert(last[4 + 1] == VDS_EVENT_OVERFLOW && last[4 + 3] == 3);           /* loss reported first */
    assert(last[4 + 4] == 3 && last[4 + 4 + 1 + 2] == 3);                      /* oldest kept: id 3 */
    while (last[1] || last[2]) { begin(); b8(0x36); b8(255); assert(send_batch()); }
    p.push_events = 1;

    /* Random batches: valid opcodes with random arguments, plus junk. */
    static const uint8_t ops[] = {0x01, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x30, 0x31, 0x32, 0x33, 0x35, 0x50, 0x51,
                                  0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x74, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x34};
    p.yield = count_yield;
    unsigned applied = 0;
    for (int i = 0; i < 20000; ++i) {
        begin();
        unsigned nops = 1 + rnd(6);
        for (unsigned k = 0; k < nops && blen < 200; ++k) {
            b8(rnd(50) ? ops[rnd(sizeof ops - (rnd(40) ? 1 : 0))] : rnd(256));
            unsigned args = rnd(12);
            for (unsigned a = 0; a < args && blen < 254; ++a) b8(rnd(4) ? rnd(64) : rnd(256));
        }
        unsigned before = yields;yield_generation = p.generation;
        int ok = vds_proto_batch(&p, batch, blen);
        assert(ok ? yields > before : yields == before);
        applied += (unsigned)ok;
        vds_proto_frame(&p, rnd(40), rnd(3) ? 1 + rnd(50000) : UINT32_MAX);
        assert(va.ndamage <= VDS_MAX_DAMAGE && va.count <= VDS_MAX_WINDOWS && p.pool_used <= VDS_PROTO_POOL);
    }
    assert(applied > 100);

    /* Inspection (ops 94-98): what exists, read back from the server's own
     * widget state (the CPU keeps no table), without starting a hold. */
    static vds_ui u;
    p.yield = 0; p.push_events = 0;
    vds_ui_init(&u, &p);
    begin(); b8(0x34); b8(0x01); b8(3); b8(30); b8(10); b8(48);
    b8(0x50); b8(4); b8(3); b16(16); b16(24); b8(30); b8(10); b8(VDS_W_VISIBLE | VDS_W_DECOR);
    b8(0x54); b8(4); b8(7); bytes("Greeter", 7);
    b8(0x80); b8(7); b8(4); b8(VDS_WK_BUTTON); b8(255); b8(2); b8(1); b8(12); b8(1); b8(0);
    b8(0x81); b8(7); b8(5); bytes("Hello", 5); b8(0x94); b8(7); b8(14); bytes("hello-button12", 14);
    b8(0x80); b8(9); b8(4); b8(VDS_WK_LIST); b8(255); b8(2); b8(3); b8(20); b8(4); b8(0);
    b8(0x92); b8(9); b8(2); b8(3); bytes("one", 3); b8(26); bytes("abcdefghijklmnopqrstuvwxyz", 26);
    b8(0x30); assert(send_batch() && !p.errors);
    begin(); b8(0x95); assert(send_batch() && !p.hold && last_code == VDS_PACKET_REPLY && last_n == 15);
    assert(last[0] == 0x95 && last[1] == 0x80 && last[2] == 0x02 && !last[3] && last[9] == 0x10 && last[13] == 0x08 && !last[14]);
    begin(); b8(0x96); b8(7); assert(send_batch() && !p.hold);
    assert(last[1] == 7 && last[2] == VDS_WK_BUTTON && last[3] == 4 && last[4] == 255 && last[5] == 2 && last[6] == 1 && last[7] == 12 && last[8] == 1);
    assert(last[18] == VDS_WIDGET_NAME && last_n == 19 + VDS_WIDGET_NAME && !memcmp(last + 19, "hello-butt", 10));   /* name cut to 10 */
    begin(); b8(0x96); b8(9); assert(send_batch() && last[2] == VDS_WK_LIST && last[12] == 2 && last[13] == 0 && last[18] == 0);
    begin(); b8(0x97); b8(4); assert(send_batch() && !p.hold && last_n == 14);
    assert(last[1] == 4 && (last[2] & 3) == 3 && last[3] == 16 && last[5] == 24 && last[7] == 30 && last[8] == 10);
    assert(last[9] == 3 && last[10] == 30 && last[11] == 10 && last[13] == 7);
    begin(); b8(0x98); b8(2); b8(4); b16(0); b8(0); assert(send_batch() && last[6] == 7 && !memcmp(last + 7, "Greeter", 7));
    begin(); b8(0x98); b8(0); b8(7); b16(0); b8(1); assert(send_batch() && last[5] == 1 && last[6] == 4 && !memcmp(last + 7, "ello", 4));
    begin(); b8(0x98); b8(1); b8(9); b16(1); b8(0); assert(send_batch() && !p.hold);
    assert(last[3] == 1 && last[6] == 23 && last_n == 30 && !memcmp(last + 7, "abcdefghijklmnopqrstuvw", 23));
    begin(); b8(0x98); b8(1); b8(9); b16(1); b8(23); assert(send_batch() && last[6] == 3 && !memcmp(last + 7, "xyz", 3));
    begin(); b8(0x98); b8(1); b8(9); b16(1); b8(26); assert(send_batch() && last[6] == 0 && last_n == 7);   /* past the end */
    unsigned before_errors = p.errors;
    begin(); b8(0x98); b8(1); b8(9); b16(2); b8(0); b8(0x96); b8(8); b8(0x97); b8(5); b8(0x98); b8(1); b8(7); b16(0); b8(0); b8(0x94); b8(8); b8(1); b8(65);
    assert(send_batch() && p.errors == before_errors + 5);   /* no row 2, widget 8, window 5; a button has no rows; no name for 8 */
    begin(); b8(0x94); b8(7); b8(9); bytes("abc", 3); assert(!send_batch());   /* truncated: the batch is refused */
    before_errors = p.errors;
    begin(); b8(0x80); b8(11); b8(4); b8(VDS_WK_BOX); b8(255); b8(0); b8(5); b8(30); b8(3); b8(0);
    b8(0x80); b8(3); b8(4); b8(VDS_WK_LABEL); b8(11); b8(0); b8(0); b8(5); b8(1); b8(0); b8(0x30); assert(send_batch() && p.errors == before_errors);
    begin(); b8(0x9B); b8(0); assert(send_batch() && !p.hold && last[2] == 4 && !memcmp(last + 3, "\x07\x09\x0b\x03", 4));
    begin(); b8(0x9B); b8(3); assert(send_batch() && last[1] == 3 && last[2] == 1 && last[3] == 3);   /* parents first */
    begin(); b8(0x99); b8(7); b8(4); b8(2); b8(9); b8(1); b8(0x9A); b8(7); b8(VDS_WF_HIDDEN); b8(0x96); b8(7); assert(send_batch());
    assert(last[5] == 4 && last[6] == 2 && last[7] == 9 && last[9] == VDS_WF_HIDDEN && !strcmp(u.widgets[7].text, "Hello"));
    begin(); b8(0x9A); b8(7); b8(0); b8(0x30); assert(send_batch() && p.errors == before_errors);
    begin(); b8(0x87); b8(11); b8(0x9B); b8(0); assert(send_batch() && last[2] == 2);   /* the box took its label along */
    begin(); b8(0x87); b8(7); b8(0x95); assert(send_batch() && last[1] == 0 && last[2] == 0x02);
    begin(); b8(0x34); b8(0x95); assert(send_batch() && !last[2] && !last[9] && !last[13]);

    /* Files (ops 9C-9E): /dev/win text rendered and parsed by the server. */
    static const char make_text[] = "window - Files\nbutton w0 ok";
    begin(); b8(0x9D); b8(VDS_FILE_NEW); b8(255); b8(0); b8(sizeof make_text - 1); bytes(make_text, sizeof make_text - 1); b8(0x30);
    assert(send_batch() && last[0] == 0x9D && last[1] == 1 && last[2] == 0 && va.windows[0].used && u.widgets[0].used);
    begin(); b8(0x9C); b8(VDS_FILE_LIST); b8(255); b16(0); b16(0); b8(0); assert(send_batch() && !p.hold);
    assert(last[1] == 0 && last[6] == 23 && !memcmp(last + 7, "w0 window - Files 64 64", 23));          /* more of this line */
    begin(); b8(0x9C); b8(VDS_FILE_LIST); b8(255); b16(0); b16(0); b8(23); assert(send_batch());
    assert(last[1] == 1 && last[6] == 7 && !memcmp(last + 7, " 32 12\n", 7) && last[2] == 0 && last[4] == 1);   /* next: (0, 1) */
    begin(); b8(0x9C); b8(VDS_FILE_LIST); b8(255); b16(0); b16(1); b8(0); assert(send_batch());
    assert(last[1] == 1 && last[6] == 23 && !memcmp(last + 7, "0 button w0 ok 0 0 8 1\n", 23) && last[2] == 27 && last[4] == 1);
    begin(); b8(0x9C); b8(VDS_FILE_LIST); b8(255); b16(27); b16(1); b8(0); assert(send_batch() && last[1] == 2 && last[6] == 0);
    begin(); b8(0x9C); b8(VDS_FILE_CTL); b8(5); b16(0); b16(0); b8(0); assert(send_batch());
    assert(last[1] == 3 && last[6] == 14 && !memcmp(last + 7, "no such widget", 14));                   /* an error says why */
    static const char bad[] = "rect 1 2\n";
    begin(); b8(0x9D); b8(VDS_FILE_CTL); b8(0); b8(0); b8(sizeof bad - 1); bytes(bad, sizeof bad - 1); assert(send_batch());
    assert(last[1] == 0 && last[3] > 0 && !memcmp(last + 4, "rect:", 5));
    begin(); b8(0x9D); b8(VDS_FILE_TEXT); b8(0); b8(0); b8(5); bytes("Hello", 5); b8(0x30); assert(send_batch() && last[1] == 1);
    begin(); b8(0x9D); b8(VDS_FILE_TEXT); b8(0); b8(1); b8(3); bytes(" yo", 3); b8(0x30); assert(send_batch() && !strcmp(u.widgets[0].text, "Hello yo"));
    uint8_t in_window = 0;
    begin(); b8(0x9E); b8(0); b8(1); assert(send_batch());
    vds_proto_event(&p, VDS_EVENT_ACTIVATE, 0, &in_window, 1);                                          /* routed: a STREAM line */
    const uint8_t *queued = p.events[(p.event_head + p.event_count - 1) % VDS_EVENT_QUEUE];
    assert(queued[1] == VDS_EVENT_STREAM && queued[2] == 0 && !memcmp(queued + 3, "activate\n", 9));
    begin(); b8(0x9E); b8(0); b8(0); assert(send_batch());
    vds_proto_event(&p, VDS_EVENT_ACTIVATE, 0, &in_window, 1);
    queued = p.events[(p.event_head + p.event_count - 1) % VDS_EVENT_QUEUE];
    assert(queued[1] == VDS_EVENT_ACTIVATE && queued[2] == 0);
    before_errors = p.errors;
    begin(); b8(0x9E); b8(VDS_NODE_WINDOW | 27); b8(1); assert(send_batch() && p.errors == before_errors + 1);   /* no such window id */
    printf("vds protocol checks passed: %u replies, %u events, %u random batches applied\n", replies, events, applied);
    return 0;
}

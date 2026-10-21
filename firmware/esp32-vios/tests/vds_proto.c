/* Host test of the VDS wire protocol (lib/vios-vds/src/vds_proto.c): E4
 * batches build the same screen as direct API calls; malformed batches are
 * rejected without side effects; replies, events, hold/commit, raw mode,
 * reset, sprite upload and G5 canvases; random batches never crash. Usage: vds_proto FONT.bin */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vds_proto.h"

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

static uint8_t canvas_a[VDS_MAX_CANVASES][VDS_CANVAS_ITEMS * sizeof(vds_item) + W / 2 * H], canvas_b[VDS_CANVAS_ITEMS * sizeof(vds_item) + 160 * 80];
static void *canvas_mem(void *ctx, uint8_t id, uint32_t bytes) {
    (void)ctx;
    return id < VDS_MAX_CANVASES && bytes <= sizeof canvas_a[0] ? canvas_a[id] : 0;
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

static void finish(vds *v) {
    for (int i = 0; i < 1000; ++i) { vds_canvas_prepare(v, UINT32_MAX); if (vds_frame(v, UINT32_MAX) && !vds_canvas_pending(v)) return; }
    assert(0);
}
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

    /* G5 canvases: the same picture through C0-C9 as through the API. */
    p.canvas_mem = canvas_mem;
    assert(vds_init(&vb, rows_b, W, H, font));          /* the batch below starts with a reset */
    vds_canvas_icons(&vb);
    unsigned errors_before = error_events;
    begin();
    b8(0x34);                                                          /* reset: a clean epoch */
    b8(0x01); b8(2); b8(40); b8(12); b8(0x30);
    b8(0x50); b8(1); b8(2); b16(80); b16(60); b8(40); b8(12); b8(VDS_W_VISIBLE | VDS_W_DECOR);
    b8(0xC0); b8(3); b8(2); b16(16); b16(8); b16(160); b16(80); b8(3); b8(VDS_CV_VISIBLE);
    b8(0xC4); b8(3); b8(0); b8(VDS_DRAW_LINE); b8(0); b16(0); b16(0); b16(159); b16(79);
    b8(0xC4); b8(3); b8(1); b8(VDS_DRAW_DISC); b8(1); b16(10); b16(10); b16(40); b16(30);
    b8(0xC4); b8(3); b8(2); b8(VDS_DRAW_TEXT); b8(0); b16(60); b16(4); b8(5); bytes("chart", 5);
    b8(0xC4); b8(3); b8(3); b8(VDS_DRAW_ICON); b8(15); b16(100); b16(40); b8(VDS_ICON_FOLDER);
    b8(0xC4); b8(3); b8(4); b8(VDS_DRAW_STIPPLE); b8(2); b16(120); b16(50); b16(30); b16(20); b8(0);
    b8(0xC4); b8(3); b8(5); b8(VDS_DRAW_DOT); b8(1); b16(-3); b16(2);
    b8(0xC6); b8(3); b8(1); b16(5); b16(-2);
    b8(0xC7); b8(3); b8(0); b8(1);
    b8(0xC8); b8(0); b8(3); b8(2); b8(15);
    b8(0xC9); b8(0); b16(0); b8(4); b8(0x12); b8(0x3F); b8(0x45); b8(0x6F);
    b8(0xC4); b8(3); b8(6); b8(VDS_DRAW_ICON); b8(0); b16(2); b16(70); b8(0);
    assert(send_batch());
    assert(error_events == errors_before);
    static uint8_t bitmap[4] = {0x12, 0x3F, 0x45, 0x6F};
    vds_surface_init(&vb, 2, 40, 12, cells_b[2], 0x30);
    vds_window_open(&vb, 1, 2, 80, 60, 40, 12, VDS_W_VISIBLE | VDS_W_DECOR, "");
    assert(vds_canvas_create(&vb, 3, 2, 16, 8, 160, 80, 3, VDS_CV_VISIBLE, canvas_b));
    vds_item it[7] = {{VDS_DRAW_LINE, 1, 0, 0, 0, 0, 159, 79, ""}, {VDS_DRAW_DISC, 1, 0, 0, 15, 8, 40, 30, ""},
                      {VDS_DRAW_TEXT, 0, 0, 5, 60, 4, 0, 0, "chart"}, {VDS_DRAW_ICON, 15, VDS_ICON_FOLDER, 0, 100, 40, 0, 0, ""},
                      {VDS_DRAW_STIPPLE, 2, 0, 0, 120, 50, 30, 20, ""}, {VDS_DRAW_DOT, 1, 0, 0, -3, 2, 0, 0, ""},
                      {VDS_DRAW_ICON, 0, 0, 0, 2, 70, 0, 0, ""}};
    for (uint8_t i = 0; i < 7; ++i) assert(vds_canvas_set(&vb, 3, i, &it[i]));
    assert(vds_bitmap_define(&vb, 0, 3, 2, 15, bitmap));
    same_screen();
    /* Replies: canvas info, an item read back as its C4 bytes, a hit test. */
    begin(); b8(0xCB); b8(3); assert(send_batch());
    assert(last_code == VDS_PACKET_REPLY && last_n == 18 && last[0] == 0xCB && last[2] == 1 && last[3] == VDS_CV_VISIBLE && last[4] == 2);
    assert(last[9] == 160 && last[11] == 80 && last[13] == 7);
    begin(); b8(0xCC); b8(3); b8(2); assert(send_batch());
    assert(last_n == 15 && last[3] == VDS_DRAW_TEXT && last[5] == 60 && last[9] == 5 && !memcmp(last + 10, "chart", 5));
    begin(); b8(0xCC); b8(3); b8(1); assert(send_batch());
    assert(last_n == 13 && last[3] == VDS_DRAW_DISC && last[5] == 15 && last[7] == 8 && last[9] == 40);
    begin(); b8(0xCC); b8(3); b8(9); assert(send_batch());
    assert(last_n == 4 && last[3] == 0);
    begin(); b8(0xCA); b8(3); b16(20); b16(20); assert(send_batch());
    assert(last_n == 7 && last[2] == 1);
    begin(); b8(0xCA); b8(3); b16(159); b16(0); assert(send_batch());
    assert(last[2] == 0);                                              /* boxes: the line's covers it all */
    begin(); b8(0xCA); b8(3); b16(200); b16(0); assert(send_batch());
    assert(last[2] == 255);
    begin(); b8(0xCB); b8(5); assert(send_batch());
    assert(last_n == 18 && last[2] == 0);
    /* An unknown item kind is malformed: the whole batch is dropped. */
    uint16_t generation = p.generation;
    begin(); b8(0xC5); b8(3); b8(0); b8(0xC4); b8(3); b8(9); b8(VDS_DRAW_KINDS); b8(0); b16(0); b16(0);
    assert(!send_batch() && p.generation == generation && vds_canvas_item(&va, 3, 0));
    /* Bad arguments are skipped with an error: x not a multiple of 8, long text, CPU-reserved surface. */
    unsigned errors = error_events;
    begin(); b8(0xC1); b8(3); b16(17); b16(0); assert(send_batch());
    begin(); b8(0xC4); b8(3); b8(9); b8(VDS_DRAW_TEXT); b8(0); b16(0); b16(0); b8(21); for (int i = 0; i < 21; ++i) b8('x'); assert(send_batch());
    begin(); b8(0xC0); b8(4); b8(VDS_PROTO_SURFACES); b16(0); b16(0); b16(8); b16(8); b8(0); b8(1); assert(send_batch());
    begin(); b8(0xC8); b8(VDS_ICON_FIRST); b8(4); b8(4); b8(15); assert(send_batch());          /* built-ins are read-only */
    assert(error_events == errors + 4);
    /* Reset drops canvases and uploaded bitmaps, keeps the built-in icons. */
    begin(); b8(0x34); assert(send_batch());
    assert(!va.canvases[3].used && !va.bitmaps[0].used && va.bitmaps[VDS_ICON_DISK].used && p.bitmap_used == 0);
    vds_canvas_delete(&vb, 3);
    vds_bitmap_undefine(&vb, 0);
    vds_window_close(&vb, 1);
    same_screen();

    /* Random batches: valid opcodes with random arguments, plus junk. */
    static const uint8_t ops[] = {0x01, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x30, 0x31, 0x32, 0x33, 0x35, 0x50, 0x51,
                                  0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x74, 0x76, 0x77, 0x78, 0x79, 0x7A,
                                  0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC4, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0x34};
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
        assert(va.ndamage <= VDS_MAX_DAMAGE && va.count <= VDS_MAX_WINDOWS && p.pool_used <= VDS_PROTO_POOL && p.bitmap_used <= VDS_PROTO_BITMAP_POOL);
    }
    assert(applied > 100);
    printf("vds protocol checks passed: %u replies, %u events, %u random batches applied\n", replies, events, applied);
    return 0;
}

/* Host test of the G5 canvases (lib/vios-vds/src/vds_canvas.c) on a fake
 * 640x480 4 bpp framebuffer with the real Agon font.
 *
 * - Rasteriser: every item kind except lines against an independent
 *   per-pixel definition, at random positions including off-canvas ones;
 *   lines by their properties (endpoints, one pixel per major step, distance
 *   to the ideal line).
 * - Compositor: canvases over cells, transparency, readiness, scrolling.
 * - Oracle: after any random sequence of canvas, item, bitmap and window
 *   operations with budget-limited frames, every raster equals a fresh replay
 *   of its list and the framebuffer equals a fresh full render.
 * Usage: vds_canvas FONT.bin */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vds.h"
#include "vds_canvas.h"

#define W 640
#define H 480
#define CW 320
#define CH 200
static uint8_t fb_a[H][W / 2], fb_b[H][W / 2], *rows_a[H], *rows_b[H];
static uint8_t font[256 * 8];
static vds_cell cells[4][60 * 80];
static uint8_t mem[VDS_MAX_CANVASES][VDS_CANVAS_ITEMS * sizeof(vds_item) + W / 2 * H];
static uint8_t ref[CH][CW / 2], replayed[H][W / 2];
static uint8_t bitmap_pixels[4][VDS_BITMAP_MAX / 2 * VDS_BITMAP_MAX];
static vds v;

static unsigned seed = 4242;
static unsigned rnd(unsigned n) { seed = seed * 1103515245u + 12345u; return (seed >> 16) % n; }
static int srnd(int lo, int hi) { return lo + (int)rnd((unsigned)(hi - lo + 1)); }

static uint8_t get4(const uint8_t *row, int x) { return (x & 1) ? row[x >> 1] & 15 : row[x >> 1] >> 4; }
static void put4(uint8_t *row, int x, uint8_t c) { row[x >> 1] = (x & 1) ? (uint8_t)((row[x >> 1] & 0xF0) | c) : (uint8_t)((row[x >> 1] & 0x0F) | c << 4); }
static uint8_t px(int x, int y) { return get4(fb_a[y], x); }

static void finish(void) {
    for (int i = 0; i < 5000; ++i) {
        vds_canvas_prepare(&v, 20000);
        if (vds_frame(&v, 100000) && !vds_canvas_pending(&v)) return;
    }
    assert(!"frames never finished");
}

/* Every raster equals a fresh replay, and the screen a fresh full render. */
#define consistent() consistent_at(__LINE__)
static void consistent_at(int line) {
    finish();
    for (uint8_t k = 0; k < VDS_MAX_CANVASES; ++k) {
        const vds_canvas *c = &v.canvases[k];
        if (!c->used) continue;
        assert(c->flags & VDS_CV_READY);
        size_t stride = (size_t)(c->w + 1) / 2;
        memset(replayed, 0x5A, sizeof replayed);
        vds_rect all = {0, 0, c->w, c->h};
        vds_canvas_replay(&v, k, all, &replayed[0][0]);   /* rows of the canvas stride, flat */
        for (int y = 0; y < c->h; ++y)
            for (int x = 0; x < c->w; ++x)
                if (get4(&replayed[0][0] + y * stride, x) != get4(c->pixels + y * stride, x)) {
                    fprintf(stderr, "check at line %d: canvas %u raster differs from a replay at %d,%d\n", line, k, x, y);
                    abort();
                }
    }
    memset(fb_b, 0xEE, sizeof fb_b);
    v.rows = rows_b;
    vds_damage_all(&v);
    finish();
    v.rows = rows_a;
    for (int y = 0; y < H; ++y)
        if (memcmp(fb_a[y], fb_b[y], W / 2)) {
            int x = 0;
            while (fb_a[y][x] == fb_b[y][x]) ++x;
            fprintf(stderr, "check at line %d: stale pixels at x %d row %d (%02X vs %02X)\n", line, 2 * x, y, fb_a[y][x], fb_b[y][x]);
            abort();
        }
}

/* ------------------------------------------------------------ reference */

/* Does item it cover canvas pixel (x, y)? Lines are tested separately. */
static int covers(const vds_item *it, int x, int y, uint8_t *colour) {
    long a = it->a, b = it->b, w = it->c, h = it->d;
    *colour = it->colour;
    switch (it->kind) {
    case VDS_DRAW_DOT: return x == a && y == b;
    case VDS_DRAW_BAR: return x >= a && x < a + w && y >= b && y < b + h;
    case VDS_DRAW_STIPPLE:
        if ((x + y) & 1) *colour = it->extra;
        return x >= a && x < a + w && y >= b && y < b + h;
    case VDS_DRAW_RECT:
        return x >= a && x < a + w && y >= b && y < b + h && (x == a || x == a + w - 1 || y == b || y == b + h - 1);
    case VDS_DRAW_DISC: case VDS_DRAW_OVAL: {
        if (w <= 0 || h <= 0) return 0;
#define INSIDE(i, j) ((i) >= 0 && (i) < w && (j) >= 0 && (j) < h && \
        (long long)(2 * (i) + 1 - w) * (2 * (i) + 1 - w) * h * h + (long long)(2 * (j) + 1 - h) * (2 * (j) + 1 - h) * w * w <= (long long)w * w * h * h)
        long i = x - a, j = y - b;
        if (!INSIDE(i, j)) return 0;
        return it->kind == VDS_DRAW_DISC || !INSIDE(i - 1, j) || !INSIDE(i + 1, j) || !INSIDE(i, j - 1) || !INSIDE(i, j + 1);
    }
    case VDS_DRAW_TEXT: {
        long k = (x - a) >> 3;
        if (x < a || k >= it->n || y < b || y >= b + 8) return 0;
        return (font[(uint8_t)it->text[k] * 8 + (y - b)] & (0x80 >> ((x - a) & 7))) != 0;
    }
    case VDS_DRAW_ICON: {
        const vds_bitmap *bm = &v.bitmaps[it->extra];
        if (!bm->used || x < a || x >= a + bm->w || y < b || y >= b + bm->h) return 0;
        *colour = get4(bm->pixels + (size_t)(y - b) * ((bm->w + 1u) / 2), (int)(x - a));
        return *colour != bm->transparent;
    }
    }
    return 0;
}

static vds_item random_item(int kind, int cw, int ch) {
    vds_item it;
    memset(&it, 0, sizeof it);
    it.kind = (uint8_t)kind;
    it.colour = (uint8_t)rnd(16);
    it.a = (int16_t)srnd(-40, cw + 10);
    it.b = (int16_t)srnd(-40, ch + 10);
    it.c = (int16_t)srnd(-2, 90);
    it.d = (int16_t)srnd(-2, 70);
    if (kind == VDS_DRAW_LINE) { it.c = (int16_t)srnd(-40, cw + 40); it.d = (int16_t)srnd(-40, ch + 40); }
    if (kind == VDS_DRAW_STIPPLE) it.extra = (uint8_t)rnd(16);
    if (kind == VDS_DRAW_ICON) it.extra = (uint8_t)(rnd(2) ? VDS_ICON_FIRST + rnd(8) : rnd(4));
    if (kind == VDS_DRAW_TEXT) {
        it.n = (uint8_t)rnd(VDS_CANVAS_TEXT_MAX + 1);
        for (unsigned i = 0; i < it.n; ++i) it.text[i] = (char)(32 + rnd(224));
    }
    return it;
}

static void check_kinds(void) {
    for (int round = 0; round < 400; ++round) {
        uint8_t bg = (uint8_t)rnd(16);
        assert(vds_canvas_create(&v, 0, 0, 0, 0, CW, CH, bg, 0, mem[0]));
        vds_item list[12];
        int n = 1 + (int)rnd(12);
        for (int i = 0; i < n; ++i) {
            int kind;
            do kind = 1 + (int)rnd(VDS_DRAW_KINDS - 1); while (kind == VDS_DRAW_LINE);
            list[i] = random_item(kind, CW, CH);
            assert(vds_canvas_set(&v, 0, (uint8_t)(i * 3), &list[i]));
        }
        for (int y = 0; y < CH; ++y)
            for (int x = 0; x < CW; ++x) {
                uint8_t c = bg, k;
                for (int i = 0; i < n; ++i) if (covers(&list[i], x, y, &k)) c = k;
                put4(ref[y], x, c);
            }
        vds_rect all = {0, 0, CW, CH};
        vds_canvas_replay(&v, 0, all, &replayed[0][0]);
        for (int y = 0; y < CH; ++y)
            for (int x = 0; x < CW; ++x)
                if (get4(&replayed[0][0] + (size_t)y * (CW / 2), x) != get4(ref[y], x)) {
                    fprintf(stderr, "round %d: pixel %d,%d is %u, expected %u\n", round, x, y,
                            get4(&replayed[0][0] + (size_t)y * (CW / 2), x), get4(ref[y], x));
                    for (int i = 0; i < n; ++i) fprintf(stderr, "  item kind %u %d %d %d %d\n", list[i].kind, list[i].a, list[i].b, list[i].c, list[i].d);
                    abort();
                }
    }
    /* A 5x5 disc is the square without its corners. */
    assert(vds_canvas_create(&v, 0, 0, 0, 0, 16, 16, 0, 0, mem[0]));
    vds_item d = {VDS_DRAW_DISC, 3, 0, 0, 1, 1, 5, 5, ""};
    assert(vds_canvas_set(&v, 0, 0, &d));
    vds_rect all = {0, 0, 16, 16};
    vds_canvas_replay(&v, 0, all, &replayed[0][0]);
    const uint8_t *r0 = &replayed[0][0];
    assert(get4(r0 + 8, 1) == 0 && get4(r0 + 8, 2) == 3 && get4(r0 + 8, 5) == 0 && get4(r0 + 16, 1) == 3 && get4(r0 + 40, 5) == 0 && get4(r0 + 40, 4) == 3);
}

static void check_lines(void) {
    for (int round = 0; round < 2000; ++round) {
        assert(vds_canvas_create(&v, 0, 0, 0, 0, CW, CH, 0, 0, mem[0]));
        vds_item it = random_item(VDS_DRAW_LINE, CW, CH);
        it.colour = 3;
        if (round < 1000) { it.a = (int16_t)rnd(CW); it.b = (int16_t)rnd(CH); it.c = (int16_t)rnd(CW); it.d = (int16_t)rnd(CH); }
        else if (round % 4 == 0) { it.a = (int16_t)srnd(-32768, 32767); it.c = (int16_t)srnd(-32768, 32767); it.b = (int16_t)srnd(-32768, 32767); it.d = (int16_t)srnd(-32768, 32767); }
        assert(vds_canvas_set(&v, 0, 7, &it));
        vds_rect all = {0, 0, CW, CH};
        vds_canvas_replay(&v, 0, all, &replayed[0][0]);
        long dx = labs((long)it.c - it.a), dy = labs((long)it.d - it.b);
        int steep = dy > dx;
        long count = 0;
        for (int y = 0; y < CH; ++y)
            for (int x = 0; x < CW; ++x) {
                if (get4(&replayed[0][0] + (size_t)y * (CW / 2), x) != 3) continue;
                ++count;
                /* within half a pixel of the ideal line along the minor axis */
                double t, ideal;
                if (steep) { t = (double)(y - it.b) / (double)(it.d - it.b); ideal = it.a + t * (it.c - it.a); assert(t >= -1e-9 && t <= 1 + 1e-9 && ideal - x <= 0.5 + 1e-9 && x - ideal <= 0.5 + 1e-9); }
                else if (dx) { t = (double)(x - it.a) / (double)(it.c - it.a); ideal = it.b + t * (it.d - it.b); assert(t >= -1e-9 && t <= 1 + 1e-9 && ideal - y <= 0.5 + 1e-9 && y - ideal <= 0.5 + 1e-9); }
                else assert(x == it.a && y == it.b);
            }
        /* one pixel per major-axis step, endpoints included, clipped to the canvas */
        long expect = 0;
        for (long s = 0; s <= (steep ? dy : dx); ++s) {
            long major = steep ? it.b + (it.d > it.b ? s : -s) : it.a + (it.c > it.a ? s : -s);
            if (major >= 0 && major < (steep ? CH : CW)) ++expect;
        }
        if (it.a >= 0 && it.a < CW && it.b >= 0 && it.b < CH && it.c >= 0 && it.c < CW && it.d >= 0 && it.d < CH) {
            assert(count == (steep ? dy : dx) + 1);
            assert(get4(&replayed[0][0] + (size_t)it.b * (CW / 2), it.a) == 3);
            assert(get4(&replayed[0][0] + (size_t)it.d * (CW / 2), it.c) == 3);
        } else
            assert(count <= expect);
    }
}

/* ------------------------------------------------------------ compositor */

static void check_compositor(void) {
    uint8_t ink = (uint8_t)(0 | 3 << 4);
    for (uint8_t s = 0; s < 4; ++s) assert(vds_surface_init(&v, s, 80, 60, cells[s], ink));
    vds_put(&v, 0, 0, 0, ink, "HELLO WORLD", 11);
    assert(vds_window_open(&v, 1, 0, 64, 48, 40, 20, VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW | VDS_W_ACTIVE, "Canvas"));
    consistent();

    /* An opaque canvas at surface pixel (16, 8) shows over the cells once ready. */
    assert(!vds_canvas_create(&v, 1, 0, 12, 8, 64, 32, 2, VDS_CV_VISIBLE, mem[1]));   /* x not a multiple of 8 */
    assert(vds_canvas_create(&v, 1, 0, 16, 8, 64, 32, 2, VDS_CV_VISIBLE, mem[1]));
    assert(!(v.canvases[1].flags & VDS_CV_READY));
    vds_frame(&v, UINT32_MAX);
    assert(px(64 + 16, 48 + 8) == 3);                           /* not ready: still the cells */
    vds_item bar = {VDS_DRAW_BAR, 1, 0, 0, 4, 4, 8, 8, ""};
    assert(vds_canvas_set(&v, 1, 10, &bar));
    consistent();
    assert(px(64 + 16, 48 + 8) == 2 && px(64 + 16 + 4, 48 + 8 + 4) == 1 && px(64 + 16 + 63, 48 + 8 + 31) == 2);
    assert(px(64 + 16 + 64, 48 + 8) == 3);

    /* Transparent: the background colour shows the cells ('H' at 0,0 is ink). */
    assert(vds_canvas_create(&v, 2, 0, 0, 0, 8, 8, 3, VDS_CV_VISIBLE | VDS_CV_CLEAR, mem[2]));
    vds_item dot = {VDS_DRAW_DOT, 1, 0, 0, 7, 7, 0, 0, ""};
    assert(vds_canvas_set(&v, 2, 0, &dot));
    consistent();
    assert(px(64 + 1, 48 + 1) == (font['H' * 8 + 1] & 0x40 ? 0 : 3) && px(64 + 7, 48 + 7) == 1);

    /* The window's scroll offset moves canvases with the text. */
    assert(vds_window_scroll(&v, 1, 0, 1));
    consistent();
    assert(px(64 + 16 + 4, 48 + 4) == 1);
    assert(vds_window_scroll(&v, 1, 0, 0));

    /* A cell scroll under a canvas recomposes (no blit) and the canvas stays. */
    uint32_t blits = v.stats.blits;
    assert(vds_scroll_rect(&v, 0, 0, 0, 40, 20, 1, ink));
    assert(v.stats.blits == blits);
    consistent();
    assert(px(64 + 16 + 4, 48 + 8 + 4) == 1);
    assert(vds_canvas_flags(&v, 1, 0) && vds_canvas_flags(&v, 2, 0));
    blits = v.stats.blits;
    assert(vds_scroll_rect(&v, 0, 0, 0, 40, 20, 1, ink));
    assert(v.stats.blits == blits + 1);                          /* hidden canvases: blit again */
    consistent();
    assert(px(64 + 16 + 4, 48 + 8 + 4) == 3);
    assert(vds_canvas_flags(&v, 1, VDS_CV_VISIBLE));
    consistent();

    /* Two windows on one surface both show it; moving the canvas damages both. */
    assert(vds_window_open(&v, 2, 0, 400, 200, 20, 10, VDS_W_VISIBLE | VDS_W_DECOR, "Twin"));
    consistent();
    assert(px(400 + 20, 200 + 12) == 1);
    assert(vds_canvas_move(&v, 1, 24, 16) && !vds_canvas_move(&v, 1, 25, 16));
    consistent();
    assert(px(400 + 28, 200 + 20) == 1);

    /* Hit test: topmost item whose box holds the point. */
    vds_item cover = {VDS_DRAW_RECT, 5, 0, 0, 0, 0, 10, 10, ""};
    assert(vds_canvas_set(&v, 1, 20, &cover));
    assert(vds_canvas_hit(&v, 1, 5, 5) == 20 && vds_canvas_hit(&v, 1, 20, 20) == 255);
    assert(vds_canvas_erase(&v, 1, 20) && vds_canvas_hit(&v, 1, 5, 5) == 10);
    assert(!vds_canvas_erase(&v, 1, 20));

    /* Item edits: shift, ink, read back. */
    assert(vds_canvas_shift(&v, 1, 10, 10, -2) && vds_canvas_item(&v, 1, 10)->a == 14 && vds_canvas_item(&v, 1, 10)->b == 2);
    assert(vds_canvas_ink(&v, 1, 10, 6) && vds_canvas_item(&v, 1, 10)->colour == 6);
    consistent();
    assert(!vds_canvas_item(&v, 1, 11) && !vds_canvas_ink(&v, 1, 11, 1) && !vds_canvas_ink(&v, 1, 10, 16));

    /* Validation and the raster cap. */
    vds_item bad = {VDS_DRAW_KINDS, 1, 0, 0, 0, 0, 1, 1, ""};
    assert(!vds_canvas_set(&v, 1, 0, &bad) && !vds_canvas_set(&v, 1, 255, &bar) && !vds_canvas_set(&v, 7, 0, &bar));
    bad.kind = VDS_DRAW_TEXT; bad.n = VDS_CANVAS_TEXT_MAX + 1;
    assert(!vds_canvas_set(&v, 1, 0, &bad));
    assert(!vds_canvas_create(&v, 3, 0, 0, 0, 641, 10, 0, 0, mem[3]) && !vds_canvas_create(&v, 3, 0, 0, 0, 10, 481, 0, 0, mem[3]));
    assert(!vds_canvas_create(&v, 3, VDS_MAX_SURFACES, 0, 0, 8, 8, 0, 0, mem[3]) && !vds_canvas_create(&v, 3, 0, 0, 0, 8, 8, 16, 0, mem[3]));
    assert(vds_canvas_create(&v, 3, 3, 0, 0, 640, 480, 0, 0, mem[3]));
    assert(vds_canvas_create(&v, 4, 3, 0, 0, 640, 300, 0, 0, mem[4]));
    assert(!vds_canvas_create(&v, 5, 3, 0, 0, 640, 300, 0, 0, mem[5]));    /* over 256 KB in all */
    assert(vds_canvas_delete(&v, 3) && vds_canvas_delete(&v, 4) && !vds_canvas_delete(&v, 4));
    consistent();

    /* Bitmaps: redefining one redraws the canvases that use it. */
    memset(bitmap_pixels[0], 0x11, sizeof bitmap_pixels[0]);
    assert(vds_bitmap_define(&v, 0, 4, 4, 15, bitmap_pixels[0]));
    vds_item icon = {VDS_DRAW_ICON, 0, 0, 0, 40, 4, 0, 0, ""};
    assert(vds_canvas_set(&v, 1, 30, &icon));
    consistent();
    assert(px(64 + 24 + 40, 48 + 16 + 4) == 1);
    memset(bitmap_pixels[0], 0x44, sizeof bitmap_pixels[0]);
    vds_bitmap_changed(&v, 0);
    consistent();
    assert(px(64 + 24 + 40, 48 + 16 + 4) == 4);
    assert(vds_bitmap_define(&v, 0, 8, 8, 15, bitmap_pixels[0]));
    consistent();
    assert(px(64 + 24 + 47, 48 + 16 + 11) == 4);
    vds_bitmap_undefine(&v, 0);
    consistent();
    icon.extra = VDS_ICON_ERROR;
    assert(vds_canvas_set(&v, 1, 30, &icon));
    consistent();
    assert(px(64 + 24 + 40 + 8, 48 + 16 + 4 + 8) == 3 || px(64 + 24 + 40 + 8, 48 + 16 + 4 + 8) == 1);
    assert(!vds_bitmap_define(&v, 32, 4, 4, 15, bitmap_pixels[0]) && !vds_bitmap_define(&v, 1, 65, 4, 15, bitmap_pixels[0]));

    /* Deleting a canvas restores the cells; erase-all empties the list. */
    assert(vds_canvas_erase(&v, 1, 255) && !vds_canvas_item(&v, 1, 10));
    consistent();
    assert(px(64 + 24 + 4, 48 + 16 + 4) == 2);
    assert(vds_canvas_delete(&v, 1) && vds_canvas_delete(&v, 2));
    consistent();
    assert(vds_window_close(&v, 1) && vds_window_close(&v, 2));
}

/* ------------------------------------------------------------ budget */

static void check_budget(void) {
    uint8_t ink = (uint8_t)(0 | 3 << 4);
    assert(vds_surface_init(&v, 0, 80, 60, cells[0], ink));
    assert(vds_window_open(&v, 3, 0, 0, 0, 80, 60, VDS_W_VISIBLE, ""));
    assert(vds_canvas_create(&v, 0, 0, 0, 0, 640, 480, 4, VDS_CV_VISIBLE, mem[0]));
    finish();
    vds_item bar = {VDS_DRAW_BAR, 1, 0, 0, 0, 0, 640, 480, ""};
    assert(vds_canvas_set(&v, 0, 0, &bar));
    /* A full-screen change rasterises in bands over several frames, damaging
     * only the rows done; each call makes progress. */
    unsigned calls = 0;
    while (vds_canvas_pending(&v)) {
        uint32_t done = vds_canvas_prepare(&v, 640 * 48);
        assert(done >= 640 * 8 && done <= 640 * 48);
        vds_frame(&v, UINT32_MAX);
        ++calls;
    }
    assert(calls == 10);
    consistent();
    assert(px(0, 0) == 1 && px(639, 479) == 1);
    /* Cost follows what a band draws, not the item's extent: 255 lines and
     * rectangles spanning the s16 range, replayed in the smallest bands. */
    for (unsigned i = 0; i < VDS_CANVAS_ITEMS; ++i) {
        vds_item far = {(uint8_t)(i & 1 ? VDS_DRAW_LINE : VDS_DRAW_RECT), (uint8_t)(i & 15), 0, 0, -32768, (int16_t)(-32768 + (int)i), 32767, 32767, ""};
        assert(vds_canvas_set(&v, 0, (uint8_t)i, &far));
    }
    while (vds_canvas_pending(&v)) vds_canvas_prepare(&v, 1);
    consistent();
    assert(vds_canvas_delete(&v, 0) && vds_window_close(&v, 3));
}

/* ------------------------------------------------------------ oracle */

static void check_random(void) {
    uint8_t ink = (uint8_t)(0 | 3 << 4);
    for (uint8_t s = 0; s < 4; ++s) assert(vds_surface_init(&v, s, 80, 60, cells[s], ink));
    for (uint8_t s = 0; s < 4; ++s)
        for (int r = 0; r < 60; r += 3) vds_put(&v, s, (uint8_t)(r % 7), (uint8_t)r, ink, "canvas oracle text", 18);
    for (int i = 0; i < 4; ++i) memset(bitmap_pixels[i], (int)(0x12 * (i + 1)), sizeof bitmap_pixels[i]);
    for (int step = 0; step < 6000; ++step) {
        uint8_t id = (uint8_t)rnd(VDS_MAX_CANVASES), win = (uint8_t)(1 + rnd(5));
        switch (rnd(20)) {
        case 0:
            vds_canvas_create(&v, id, (uint8_t)rnd(4), (int16_t)(8 * srnd(-4, 70)), (int16_t)srnd(-30, 460),
                              (int16_t)srnd(1, 200), (int16_t)srnd(1, 150), (uint8_t)rnd(16), (uint8_t)rnd(4), mem[id]);
            break;
        case 1: vds_canvas_move(&v, id, (int16_t)(8 * srnd(-4, 70)), (int16_t)srnd(-30, 460)); break;
        case 2: vds_canvas_flags(&v, id, (uint8_t)rnd(4)); break;
        case 3: if (!rnd(4)) vds_canvas_delete(&v, id); break;
        case 4: case 5: case 6: case 7: {
            vds_item it = random_item(1 + (int)rnd(VDS_DRAW_KINDS - 1), 200, 150);
            vds_canvas_set(&v, id, (uint8_t)rnd(40), &it);
            break;
        }
        case 8: vds_canvas_erase(&v, id, rnd(10) ? (uint8_t)rnd(40) : 255); break;
        case 9: vds_canvas_shift(&v, id, (uint8_t)rnd(40), (int16_t)srnd(-20, 20), (int16_t)srnd(-20, 20)); break;
        case 10: vds_canvas_ink(&v, id, (uint8_t)rnd(40), (uint8_t)rnd(16)); break;
        case 11: {
            uint8_t b = (uint8_t)rnd(4);
            if (rnd(3)) vds_bitmap_define(&v, b, (uint8_t)srnd(1, 64), (uint8_t)srnd(1, 64), (uint8_t)rnd(16), bitmap_pixels[b]);
            else vds_bitmap_undefine(&v, b);
            break;
        }
        case 12:
            vds_window_open(&v, win, (uint8_t)rnd(4), (int16_t)srnd(-40, 600), (int16_t)srnd(-40, 460), (uint8_t)srnd(1, 50),
                            (uint8_t)srnd(1, 30), (uint8_t)(VDS_W_VISIBLE | (rnd(2) ? VDS_W_DECOR : 0) | (rnd(2) ? VDS_W_SHADOW : 0) | (rnd(4) ? 0 : VDS_W_FREE_X)), "w");
            break;
        case 13: vds_window_move(&v, win, (int16_t)srnd(-40, 600), (int16_t)srnd(-40, 460)); break;
        case 14: vds_window_scroll(&v, win, (uint8_t)rnd(5), (uint8_t)rnd(8)); break;
        case 15: vds_scroll_rect(&v, (uint8_t)rnd(4), (uint8_t)rnd(20), (uint8_t)rnd(20), (uint8_t)srnd(1, 40), (uint8_t)srnd(1, 30), srnd(-3, 3), ink); break;
        case 16: vds_window_raise(&v, win); break;
        case 17: if (!rnd(4)) vds_window_close(&v, win); break;
        case 18: vds_put(&v, (uint8_t)rnd(4), (uint8_t)rnd(70), (uint8_t)rnd(60), (uint8_t)rnd(256), "xyz", 3); break;
        case 19: {                                         /* a budget-limited frame */
            vds_canvas_prepare(&v, rnd(30000));
            vds_frame(&v, rnd(40000));
            break;
        }
        }
        if (step % 20 == 19) consistent();
    }
    consistent();
}

int main(int argc, char **argv) {
    assert(argc >= 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f && fread(font, 1, sizeof font, f) == sizeof font);
    fclose(f);
    for (int y = 0; y < H; ++y) { rows_a[y] = fb_a[y]; rows_b[y] = fb_b[y]; }
    assert(vds_init(&v, rows_a, W, H, font));
    vds_canvas_icons(&v);
    for (int i = 0; i < 8; ++i) assert(v.bitmaps[VDS_ICON_FIRST + i].used && v.bitmaps[VDS_ICON_FIRST + i].w == 16);
    assert(vds_canvas_bytes(640, 480) <= sizeof mem[0] && vds_canvas_bytes(0, 4) == 0);
    finish();

    check_kinds();
    check_lines();
    assert(vds_canvas_delete(&v, 0));
    check_compositor();
    check_budget();
    check_random();
    puts("vds canvas checks passed");
    return 0;
}

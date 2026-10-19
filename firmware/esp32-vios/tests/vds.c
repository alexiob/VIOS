/* Host test of the VIOS Display Server core (lib/vios-vds) on a fake 640x480
 * 4 bpp framebuffer with the real Agon font. The main oracle: after any
 * sequence of operations and (budget-limited) frames, the incrementally
 * updated framebuffer equals a fresh full render of the same scene. Usage:
 * vds FONT.bin [DUMP_DIR] (DUMP_DIR receives PPM screenshots of both styles). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vds.h"

#define W 640
#define H 480
static uint8_t fb_a[H][W / 2], fb_b[H][W / 2], *rows_a[H], *rows_b[H];
static uint8_t font[256 * 8];
static vds_cell cells[4][60 * 80];
static vds v;
static int last_row;

static void row_written(void *ctx, int16_t y) { (void)ctx; assert(y > last_row); last_row = y; }  /* beam order */
static int frame(uint32_t budget) { last_row = -1; return vds_frame(&v, budget); }
static void finish(void) { for (int i = 0; i < 1000; ++i) if (frame(UINT32_MAX)) return; assert(!"frame never finished"); }
static uint8_t px(int x, int y) { return (x & 1) ? fb_a[y][x >> 1] & 15 : fb_a[y][x >> 1] >> 4; }

/* The retained framebuffer must equal a full render of the current scene. */
#define matches_full_render() matches_full_render_at(__LINE__)
static void matches_full_render_at(int line) {
    finish();
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

static uint8_t glyph_px(char ch, int row, int col, uint8_t attr) {
    return (font[(uint8_t)ch * 8 + row] & (0x80 >> col)) ? (attr & 15) : (attr >> 4);
}

static unsigned seed = 12345;
static unsigned rnd(unsigned n) { seed = seed * 1103515245u + 12345u; return (seed >> 16) % n; }

static void dump(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    assert(f);
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t c = v.style.palette[px(x, y)], rgb[3] = {(uint8_t)((c >> 4 & 3) * 85), (uint8_t)((c >> 2 & 3) * 85), (uint8_t)((c & 3) * 85)};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static void scene(void) {
    static const char *source[] = {": ROW    ( n -- )", "   DUP 4 .R  DUP SQUARE 6 .R", "   CUBE 8 .R  CR ;", "",
                                   ": TABLE  ( -- )", "   CR .\"     n    n^2     n^3\" CR", "   11 1 DO  I ROW  LOOP ;"};
    uint8_t ink = v.style.kind == VDS_STYLE_TURBO ? (uint8_t)(2 | 4 << 4) : (uint8_t)(0 | 3 << 4);
    vds_fill(&v, 0, 0, 0, 80, 60, ' ', ink);
    for (unsigned i = 0; i < sizeof source / sizeof *source; ++i) vds_put(&v, 0, 1, (uint8_t)(i + 1), ink, source[i], strlen(source[i]));
    vds_put(&v, 0, 1, 9, (uint8_t)(3 | 0 << 4), "TABLE", 5);
    vds_fill(&v, 1, 0, 0, 80, 60, ' ', ink);
    vds_put(&v, 1, 1, 1, ink, "boot confirmed B", 16);
    vds_put(&v, 1, 1, 2, ink, "VIOS Forth 24 / Agon Light 2", 28);
    vds_put(&v, 1, 1, 3, ink, "vios> DEMO", 10);
    vds_fill(&v, 2, 0, 0, 80, 60, ' ', ink);
    vds_put(&v, 2, 1, 1, ink, "3 4 + .", 7);
    vds_put(&v, 2, 1, 2, ink, "' SQUARE INSPECT", 16);
    finish();
}

int main(int argc, char **argv) {
    assert(argc >= 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f && fread(font, 1, sizeof font, f) == sizeof font);
    fclose(f);
    for (int y = 0; y < H; ++y) { rows_a[y] = fb_a[y]; rows_b[y] = fb_b[y]; }

    assert(!vds_init(&v, rows_a, 641, H, font) && !vds_init(&v, rows_a, 636, H, font));
    assert(vds_init(&v, rows_a, W, H, font));
    v.row_written = row_written;
    finish();
    /* Smalltalk desktop: 50 % dither of colours 4 and 5. */
    assert(px(0, 0) == 5 && px(1, 0) == 4 && px(0, 1) == 4 && px(1, 1) == 5);

    for (uint8_t s = 0; s < 4; ++s) assert(vds_surface_init(&v, s, 80, 60, cells[s], (uint8_t)(0 | 3 << 4)));
    uint8_t ink = (uint8_t)(0 | 3 << 4);
    assert(vds_put(&v, 0, 0, 0, ink, "HELLO", 5));
    /* x = 13 snaps to 8: aligned word stores for every glyph row. */
    assert(vds_window_open(&v, 1, 0, 13, 40, 40, 20, VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW | VDS_W_ACTIVE, "Browser"));
    assert(v.windows[1].x == 8);
    finish();
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c) assert(px(8 + c, 40 + r) == glyph_px('H', r, c, ink));
    assert(px(6, 45) == 0 && px(7, 45) == 0);                       /* 2 px active border */
    vds_rect outer = vds_window_outer(&v, 1);
    assert(outer.x == 6 && outer.y == 28 && outer.w == 328 && outer.h == 178);
    assert(px(331, 100) == v.style.shade[4]);                       /* shadow over desktop colour 4 */
    assert(px(7, 33) == v.style.tab);                               /* title tab background */
    matches_full_render();

    /* Occlusion and raise. */
    assert(vds_surface_init(&v, 1, 80, 60, cells[1], (uint8_t)(0 | 7 << 4)));
    assert(vds_window_open(&v, 2, 1, 100, 60, 20, 10, VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW, "Inspector"));
    finish();
    assert(px(104, 64) == 7);
    matches_full_render();
    assert(vds_window_raise(&v, 1));
    finish();
    assert(px(104, 64) == 3);
    matches_full_render();
    assert(vds_window_move(&v, 2, 300, 300) && vds_window_raise(&v, 2));
    matches_full_render();

    /* Free x: the generic path places the same glyph at an odd pixel. */
    assert(vds_set_snap(&v, 1) && !vds_set_snap(&v, 4));
    assert(vds_window_open(&v, 3, 0, 417, 250, 10, 4, VDS_W_VISIBLE, ""));
    assert(v.windows[3].x == 417);
    finish();
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c) assert(px(417 + c, 250 + r) == glyph_px('H', r, c, ink));
    matches_full_render();
    assert(vds_set_snap(&v, 8));

    /* A small budget carries damage over at scanline boundaries. */
    uint32_t carried = v.stats.carried;
    vds_damage_all(&v);
    unsigned frames = 1;
    while (!frame(W * 10)) ++frames;
    assert(frames > 40 && v.stats.carried > carried);
    matches_full_render();

    /* Damage stays bounded, and merged damage is still complete. */
    for (int i = 0; i < 200; ++i) {
        vds_rect r = {(int16_t)rnd(W), (int16_t)rnd(H), (int16_t)(1 + rnd(9)), (int16_t)(1 + rnd(9))};
        vds_damage(&v, r);
        assert(v.ndamage <= VDS_MAX_DAMAGE);
    }
    matches_full_render();

    /* Rectangle scroll: only the rectangle moves; vacated rows are cleared. */
    {
        static vds_cell sc[10 * 6];
        assert(vds_surface_init(&v, 5, 10, 6, sc, 0x30));
        for (uint8_t r = 0; r < 6; ++r) { char line[10]; memset(line, 'A' + r, 10); vds_put(&v, 5, 0, r, 0x30, line, 10); }
        assert(vds_scroll_rect(&v, 5, 2, 1, 4, 4, 1, 0x31));               /* rows 1..4, cols 2..5 up by one */
        assert(sc[1 * 10 + 2].ch == 'C' && sc[3 * 10 + 5].ch == 'E');           /* moved up inside */
        assert(sc[1 * 10 + 1].ch == 'B' && sc[1 * 10 + 6].ch == 'B' && sc[5 * 10 + 2].ch == 'F');   /* outside untouched */
        assert(sc[4 * 10 + 2].ch == ' ' && sc[4 * 10 + 2].attr == 0x31 && sc[0 * 10 + 2].ch == 'A');
        assert(vds_scroll_rect(&v, 5, 2, 1, 4, 4, -2, 0x30));                   /* down by two */
        assert(sc[1 * 10 + 2].ch == ' ' && sc[2 * 10 + 2].ch == ' ' && sc[3 * 10 + 2].ch == 'C' && sc[4 * 10 + 2].ch == 'D');
        assert(vds_scroll_rect(&v, 5, 0, 0, 10, 6, 6, 0x30) && sc[0].ch == ' ' && !vds_scroll_rect(&v, 5, 10, 0, 1, 1, 1, 0));
    }

    /* Sprites: the arrow cursor over a window, transparency and hotspot. */
    assert(vds_window_close(&v, 3));
    assert(!vds_sprite_define(&v, 16, 4, 4, 0, 0, 15, vds_arrow_cursor) && !vds_sprite_define(&v, 0, 4, 4, 4, 0, 15, vds_arrow_cursor));
    assert(vds_sprite_define(&v, VDS_CURSOR_SPRITE, VDS_ARROW_W, VDS_ARROW_H, 0, 0, VDS_ARROW_TRANSPARENT, vds_arrow_cursor));
    assert(vds_sprite_move(&v, VDS_CURSOR_SPRITE, 64, 120));
    finish();
    assert(px(64, 120) == 3 || px(64, 120) == v.style.body);            /* hidden after define: window shows */
    assert(vds_sprite_show(&v, VDS_CURSOR_SPRITE, 1));
    finish();
    assert(px(64, 120) == 0 && px(65, 122) == 3 && px(66, 122) == 0);    /* outline, fill, outline */
    matches_full_render();
    for (int step = 0; step < 50; ++step) {                               /* drag it across overlapping windows */
        vds_sprite_move(&v, VDS_CURSOR_SPRITE, (int16_t)(rnd(660) - 10), (int16_t)(rnd(500) - 10));
        frame(1 + rnd(30000));
    }
    matches_full_render();
    /* A second sprite below the cursor: higher ids draw on top. */
    static uint8_t block[8 * 8 / 2];
    memset(block, 0x99, sizeof block);
    assert(vds_sprite_define(&v, 3, 8, 8, 0, 0, VDS_NONE, block) && vds_sprite_move(&v, 3, 200, 200) && vds_sprite_show(&v, 3, 1));
    assert(vds_sprite_move(&v, VDS_CURSOR_SPRITE, 200, 200));
    finish();
    assert(px(200, 200) == 0 && px(203, 200) == 9 && px(205, 207) == 3);   /* arrow over block; block through transparency */
    vds_rect sr = vds_sprite_rect(&v, VDS_CURSOR_SPRITE);
    assert(sr.x == 200 && sr.y == 200 && sr.w == VDS_ARROW_W && sr.h == VDS_ARROW_H);
    matches_full_render();
    assert(vds_sprite_undefine(&v, 3));
    matches_full_render();

    /* Text caret: block inverts the cell, blinking is local time only. */
    vds_sprite_show(&v, VDS_CURSOR_SPRITE, 0);
    assert(!vds_cursor(&v, 0, 80, 0, 1, 0) && !vds_cursor(&v, 0, 0, 0, 4, 0));
    assert(vds_window_open(&v, 3, 0, 400, 300, 10, 4, VDS_W_VISIBLE, ""));
    assert(vds_cursor(&v, 0, 0, 0, VDS_CURSOR_BLOCK, 1));
    finish();
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c) assert(px(400 + c, 300 + r) == glyph_px('H', r, c, (uint8_t)(ink >> 4 | ink << 4)));
    vds_tick(&v, VDS_BLINK_MS);                                             /* off phase: plain glyph */
    finish();
    assert(px(400, 300) == glyph_px('H', 0, 0, ink));
    matches_full_render();
    vds_tick(&v, VDS_BLINK_MS);
    assert(vds_cursor(&v, 0, 2, 0, VDS_CURSOR_UNDERLINE, 0));
    finish();
    assert(px(416, 306) == (ink & 15) && px(416, 307) == (ink & 15));
    matches_full_render();
    assert(vds_window_close(&v, 3) && vds_cursor(&v, 0, 0, 0, VDS_CURSOR_OFF, 0));

    /* Random operations, partial frames and style switches. */
    static const char *words[] = {"DUP", "SWAP", ": SQUARE DUP * ;", "LOOP", "VIOS", "ok"};
    static const unsigned seeds[] = {12345, 7, 2024};
    for (unsigned run = 0; run < 3; ++run) {
    seed = seeds[run];
    uint32_t carried_before = v.stats.carried;
    for (int step = 0; step < 3000; ++step) {
        uint8_t id = (uint8_t)(1 + rnd(5)), sid = (uint8_t)rnd(4);
        switch (rnd(16)) {
        case 0: case 1: {
            const char *w = words[rnd(6)];
            vds_put(&v, sid, (uint8_t)rnd(80), (uint8_t)rnd(60), (uint8_t)(rnd(16) | rnd(16) << 4), w, strlen(w));
            break;
        }
        case 2: vds_fill(&v, sid, (uint8_t)rnd(80), (uint8_t)rnd(60), (uint8_t)rnd(20), (uint8_t)rnd(8), (uint8_t)('A' + rnd(26)), (uint8_t)rnd(256)); break;
        case 3:
            if (rnd(2)) vds_scroll(&v, sid, (int)rnd(7) - 3, (uint8_t)rnd(256));
            else vds_scroll_rect(&v, sid, (uint8_t)rnd(80), (uint8_t)rnd(60), (uint8_t)rnd(40), (uint8_t)rnd(30), (int)rnd(9) - 4, (uint8_t)rnd(256));
            break;
        case 4:
            if (!v.windows[id].used)
                vds_window_open(&v, id, rnd(5) ? sid : VDS_NONE, (int16_t)(rnd(700) - 40), (int16_t)(rnd(520) - 30),
                                (uint8_t)(1 + rnd(50)), (uint8_t)(1 + rnd(30)), (uint8_t)rnd(32), words[rnd(6)]);
            break;
        case 5: case 6: vds_window_move(&v, id, (int16_t)(rnd(700) - 40), (int16_t)(rnd(520) - 30)); break;
        case 7: vds_window_resize(&v, id, (uint8_t)(1 + rnd(60)), (uint8_t)(1 + rnd(40))); break;
        case 8: vds_window_raise(&v, id); break;
        case 9: vds_window_flags(&v, id, (uint8_t)rnd(rnd(4) ? 32 : 64)); break;   /* sometimes modal */
        case 10: vds_window_scroll(&v, id, (uint8_t)rnd(20), (uint8_t)rnd(20)); break;
        case 11: if (!rnd(4)) vds_window_close(&v, id); else vds_window_title(&v, id, words[rnd(6)]); break;
        case 14: if (rnd(2)) vds_cursor(&v, sid, (uint8_t)rnd(80), (uint8_t)rnd(60), (uint8_t)rnd(4), (uint8_t)rnd(2)); else vds_tick(&v, rnd(700)); break;
        case 13: if (rnd(2)) vds_sprite_move(&v, VDS_CURSOR_SPRITE, (int16_t)(rnd(660) - 10), (int16_t)(rnd(500) - 10)); else vds_sprite_show(&v, VDS_CURSOR_SPRITE, (int)rnd(2)); break;
        case 12: if (!rnd(20)) vds_set_style(&v, (uint8_t)rnd(VDS_STYLE_COUNT)); else vds_set_snap(&v, rnd(2) ? 8 : 1); break;
        default: frame(rnd(3) ? (uint32_t)(1 + rnd(40000)) : UINT32_MAX); break;
        }
        assert(v.ndamage <= VDS_MAX_DAMAGE && v.count <= VDS_MAX_WINDOWS);
        if (step % 100 == 99) matches_full_render();
    }
    matches_full_render();
    assert(v.stats.carried > carried_before + 50);    /* partial frames really carried damage */
    }

    /* Turbo Vision: blue dither, double frame at insets 2 and 5. */
    for (uint8_t id = 1; id <= 5; ++id) vds_window_close(&v, id);
    vds_set_snap(&v, 8);
    assert(vds_set_style(&v, VDS_STYLE_TURBO) && !vds_set_style(&v, 2));
    assert(vds_window_open(&v, 1, 0, 160, 80, 40, 20, VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW | VDS_W_ACTIVE, "WBM1P.FTH"));
    finish();
    assert(px(0, 0) == 12 && px(1, 0) == 5);
    assert(px(152 + 2, 100) == 3 && px(152 + 5, 100) == 3 && px(152 + 3, 100) == 4);
    matches_full_render();

    if (argc > 2) {   /* screenshots of the real compositor for visual review */
        for (uint8_t id = 1; id <= 5; ++id) vds_window_close(&v, id);
        for (uint8_t style = 0; style < VDS_STYLE_COUNT; ++style) {
            vds_set_style(&v, style);
            scene();
            uint8_t deco = VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW;
            vds_window_open(&v, 3, 2, 24, 360, 36, 10, deco, "Workspace");
            vds_window_open(&v, 2, 1, 368, 340, 32, 12, deco, "Transcript");
            vds_window_open(&v, 1, 0, 64, 60, 56, 30, deco | VDS_W_ACTIVE, style ? "WBM1P.FTH" : "Module Browser");
            finish();
            dump(argv[2], style ? "vds-turbo" : "vds-smalltalk");
            for (uint8_t id = 1; id <= 3; ++id) vds_window_close(&v, id);
            finish();
        }
    }
    printf("vds checks passed: %u frames, %u rows, %u spans, %u carried, %u LUT misses\n",
           (unsigned)v.stats.frames, (unsigned)v.stats.rows, (unsigned)v.stats.spans,
           (unsigned)v.stats.carried, (unsigned)v.stats.lut_misses);
    return 0;
}

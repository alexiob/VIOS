/* Host test of the VDS intrinsics (lib/vios-vds/src/vds_ui.c): focus cycling,
 * keyboard move/resize, zoom, close requests, modal windows and CPU focus
 * requests, with the retained screen checked against full renders.
 * Usage: vds_ui FONT.bin */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vds_ui.h"

#define W 640
#define H 480
static uint8_t fb_a[H][W / 2], fb_b[H][W / 2], *rows_a[H], *rows_b[H];
static uint8_t font[256 * 8];
static vds_cell cells[VDS_MAX_SURFACES][80 * 60];
static vds v;
static vds_proto p;
static vds_ui u;

static vds_cell *cell_mem(void *ctx, uint8_t id, uint16_t n) { (void)ctx; return id < VDS_MAX_SURFACES && n <= 80 * 60 ? cells[id] : 0; }
static uint8_t ev[64][32], ev_n;
static void send(void *ctx, uint8_t code, const uint8_t *d, uint8_t n) {
    (void)ctx;
    if (code == VDS_PACKET_EVENT && ev_n < 64) { memset(ev[ev_n], 0, 32); memcpy(ev[ev_n++], d, n); }
}
static void finish(void) { for (int i = 0; i < 1000; ++i) if (vds_frame(&v, UINT32_MAX)) return; assert(0); }
static void matches_full_render(void) {
    finish();
    v.rows = rows_b; vds_damage_all(&v); finish(); v.rows = rows_a;
    for (int y = 0; y < H; ++y) if (memcmp(fb_a[y], fb_b[y], W / 2)) { fprintf(stderr, "stale row %d\n", y); abort(); }
}
static int key(uint8_t ascii, uint8_t mods, uint8_t vk) {
    int a = vds_ui_key(&u, ascii, mods, vk, 1), b = vds_ui_key(&u, ascii, mods, vk, 0);
    assert(a == b);                                   /* a release follows its press */
    return a;
}
static uint8_t top(void) { return v.order[v.count - 1]; }
static int active(uint8_t id) { return (v.windows[id].flags & VDS_W_ACTIVE) != 0; }
static const uint8_t *last_event(uint8_t kind) { for (int i = ev_n - 1; i >= 0; --i) if (ev[i][1] == kind) return ev[i]; return 0; }
static int16_t s16(const uint8_t *b) { return (int16_t)(uint16_t)(b[0] | b[1] << 8); }
static uint8_t px(int x, int y) { return (x & 1) ? fb_a[y][x >> 1] & 15 : fb_a[y][x >> 1] >> 4; }
static unsigned seed = 5;
static unsigned rnd(unsigned n) { seed = seed * 1103515245u + 12345u; return (seed >> 16) % n; }

int main(int argc, char **argv) {
    assert(argc >= 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f && fread(font, 1, sizeof font, f) == sizeof font);
    fclose(f);
    for (int y = 0; y < H; ++y) { rows_a[y] = fb_a[y]; rows_b[y] = fb_b[y]; }
    assert(vds_init(&v, rows_a, W, H, font));
    vds_proto_init(&p, &v, cell_mem, send, 0);
    p.push_events = 1;                                  /* these checks watch events as they happen */
    vds_ui_init(&u, &p);
    uint8_t deco = VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW;
    for (uint8_t s = 0; s < 3; ++s) assert(vds_surface_init(&v, s, 40, 20, cells[s], 0x30));
    assert(vds_window_open(&v, 1, 0, 40, 40, 30, 12, deco, "One"));
    assert(vds_window_open(&v, 2, 1, 200, 120, 30, 12, deco, "Two"));
    assert(vds_window_open(&v, 3, 2, 360, 200, 30, 12, deco, "Three"));
    assert(vds_window_open(&v, 9, 2, 0, 400, 10, 2, VDS_W_VISIBLE, ""));      /* undecorated: never focused */
    finish();

    /* No focus yet: only F6 acts. */
    assert(!key('a', 0, 0) && !key(0, 0, VDS_VK_F5));
    assert(key(0, 0, VDS_VK_F6) && u.focus == 1 && top() == 1 && active(1));
    const uint8_t *e = last_event(VDS_EVENT_FOCUS);
    assert(e && e[2] == 1 && e[3] == VDS_NONE);
    assert(key(0, 0, VDS_VK_F6) && u.focus == 2 && top() == 2 && active(2) && !active(1));
    assert(key(0, 0, VDS_VK_F6) && u.focus == 3 && top() == 3);
    assert(key(0, 0, VDS_VK_F6) && u.focus == 1);                              /* full cycle */
    assert(key(0, VDS_MOD_SHIFT, VDS_VK_F6) && u.focus == 3 && top() == 3 && v.order[0] == 1);
    matches_full_render();

    /* Keyboard move/resize: arrows move, Shift+arrows resize, Enter keeps. */
    int16_t x0 = v.windows[3].x, y0 = v.windows[3].y;
    ev_n = 0;
    assert(key(0, VDS_MOD_CTRL, VDS_VK_F5) && u.wm_mode);
    assert(key(0, 0, VDS_VK_RIGHT) && key(0, 0, VDS_VK_RIGHT) && key(0, 0, VDS_VK_DOWN));
    assert(key(0, VDS_MOD_SHIFT, VDS_VK_RIGHT) && key(0, VDS_MOD_SHIFT, VDS_VK_UP));
    assert(key('x', 0, 0));                                                    /* swallowed in the mode */
    assert(!last_event(VDS_EVENT_WIN));                                        /* no event until Enter */
    assert(key(0, 0, VDS_VK_RETURN) && !u.wm_mode);
    e = last_event(VDS_EVENT_WIN);
    assert(e && e[2] == 3 && e[3] == VDS_WIN_MOVED && s16(e + 4) == x0 + 16 && s16(e + 6) == y0 + 8 && e[8] == 31 && e[9] == 11);
    matches_full_render();
    /* Esc reverts. */
    assert(key(0, VDS_MOD_CTRL, VDS_VK_F5) && key(0, 0, VDS_VK_LEFT) && key(0, VDS_MOD_SHIFT, VDS_VK_DOWN));
    assert(key(0, 0, VDS_VK_ESCAPE) && !u.wm_mode);
    assert(v.windows[3].x == x0 + 16 && v.windows[3].y == y0 + 8 && v.windows[3].cols == 31 && v.windows[3].rows == 11);
    matches_full_render();

    /* Zoom and restore. */
    assert(key(0, 0, VDS_VK_F5));
    e = last_event(VDS_EVENT_WIN);
    assert(e[3] == VDS_WIN_ZOOMED && v.windows[3].x == 8 && v.windows[3].y == 12 && v.windows[3].cols == 78 && v.windows[3].rows == 58);
    vds_rect o = vds_window_outer(&v, 3);
    assert(o.x >= 0 && o.y >= 0 && o.y + o.h - 4 <= H);                         /* body on screen; shadow may clip */
    matches_full_render();
    assert(key(0, 0, VDS_VK_F5));
    e = last_event(VDS_EVENT_WIN);
    assert(e[3] == VDS_WIN_RESTORED && v.windows[3].x == x0 + 16 && v.windows[3].cols == 31);
    matches_full_render();

    /* Close requests: the window stays until the CPU closes it. */
    assert(key(0, VDS_MOD_LALT, VDS_VK_F3));
    e = last_event(VDS_EVENT_WIN);
    assert(e[2] == 3 && e[3] == VDS_WIN_CLOSE && v.windows[3].used);
    ev_n = 0;
    assert(key(0x17, VDS_MOD_CTRL, 0) && last_event(VDS_EVENT_WIN)[3] == VDS_WIN_CLOSE);
    assert(!key(0, VDS_MOD_LALT, VDS_VK_F1));                                 /* not ours: forwarded */
    uint8_t close3[] = {0x57, 3, 0x30};
    assert(vds_proto_batch(&p, close3, sizeof close3) && u.focus == VDS_NONE && !v.windows[3].used);

    /* CPU focus over the wire, then a modal dialog keeps focus. */
    uint8_t focus2[] = {0x58, 2, 0x30};
    assert(vds_proto_batch(&p, focus2, sizeof focus2) && u.focus == 2 && top() == 2);
    assert(vds_window_open(&v, 4, 1, 240, 200, 20, 6, deco | VDS_W_MODAL, "Confirm"));
    assert(key(0, 0, VDS_VK_F6) && u.focus == 4 && active(4) && !active(2));
    assert(key(0, 0, VDS_VK_F6) && u.focus == 4);
    assert(!vds_ui_focus(&u, 1) && u.focus == 4);
    uint8_t focus1[] = {0x58, 1, 0x30};
    unsigned errors = p.errors;
    assert(vds_proto_batch(&p, focus1, sizeof focus1) && p.errors == errors + 1 && u.focus == 4);
    finish();
    assert(px(0, 0) == v.style.shade[v.style.desktop_b]);                     /* everything below is dimmed */
    matches_full_render();
    assert(vds_window_flags(&v, 4, (uint8_t)(v.windows[4].flags & ~VDS_W_MODAL)));
    finish();
    assert(px(0, 0) == v.style.desktop_b);
    matches_full_render();

    /* Random keys mixed with protocol traffic; the screen stays exact. */
    static const uint8_t vks[] = {VDS_VK_F6, VDS_VK_F5, VDS_VK_F3, VDS_VK_LEFT, VDS_VK_RIGHT, VDS_VK_UP, VDS_VK_DOWN,
                                  VDS_VK_RETURN, VDS_VK_ESCAPE, VDS_VK_F1, 0};
    static const uint8_t mods[] = {0, VDS_MOD_CTRL, VDS_MOD_SHIFT, VDS_MOD_LALT};
    vds_set_style(&v, VDS_STYLE_TURBO);
    for (int step = 0; step < 6000; ++step) {
        uint8_t id = (uint8_t)(1 + rnd(5));
        switch (rnd(8)) {
        case 0: case 1: case 2: case 3:
            key((uint8_t)(rnd(4) ? 0 : 'w'), mods[rnd(4)], vks[rnd(sizeof vks)]);
            break;
        case 4:
            if (!v.windows[id].used)
                vds_window_open(&v, id, (uint8_t)rnd(3), (int16_t)rnd(600), (int16_t)rnd(440), (uint8_t)(1 + rnd(40)), (uint8_t)(1 + rnd(20)),
                                (uint8_t)(deco | (rnd(8) ? 0 : VDS_W_MODAL)), "W");
            else { uint8_t b[] = {0x57, id}; vds_proto_batch(&p, b, sizeof b); }
            break;
        case 5: { uint8_t b[] = {0x58, id}; vds_proto_batch(&p, b, sizeof b); break; }
        case 6: { uint8_t b[] = {0x10, (uint8_t)rnd(3), (uint8_t)rnd(40), (uint8_t)rnd(20), (uint8_t)rnd(256), 2, 'h', 'i', 0x30};
                  vds_proto_batch(&p, b, sizeof b); break; }
        default: vds_proto_frame(&p, rnd(40), 1 + rnd(60000)); break;
        }
        if (u.focus != VDS_NONE) assert(v.windows[u.focus].used && active(u.focus));
        unsigned actives = 0;
        for (unsigned i = 0; i < VDS_MAX_WINDOWS; ++i) actives += v.windows[i].used && active((uint8_t)i);
        assert(actives <= 1);
        if (step % 200 == 199) matches_full_render();
    }
    matches_full_render();
    printf("vds intrinsics checks passed: focus %u, events seen %u\n", u.focus, (unsigned)ev_n);
    return 0;
}

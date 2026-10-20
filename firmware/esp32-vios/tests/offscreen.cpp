/* Host test of the off-screen presenter (include/vios_offscreen.h) on the
 * real compositor: a large repaint never shows a half-drawn screen (the
 * screen keeps the old picture until one present), small changes still go
 * straight to the screen, and every path ends equal to a full render. */
#include <cassert>
#include <cstdio>
#include <cstring>
#include "vios_offscreen.h"
using vios_offscreen::Presenter;

static uint8_t screen[480][320], shadow[480 * 320], ref[480][320], before[480][320], font[256 * 8];
static uint8_t *screen_rows[480], *ref_rows[480];
static vds_cell wb_cells[80 * 60], small_cells[30 * 8];
static vds v;
static Presenter p;
static const uint32_t BUDGET = 640 * 40;   // ~1/12 of the screen per frame, like the device's 3 ms

static void reference() {
    uint8_t **rows = v.rows;
    vds_rect saved[VDS_MAX_DAMAGE]; uint8_t n = v.ndamage; memcpy(saved, v.damage, sizeof saved);
    v.rows = ref_rows; vds_damage_all(&v);
    while (!vds_frame(&v, UINT32_MAX)) {}
    v.rows = rows; v.ndamage = n; memcpy(v.damage, saved, sizeof saved);
}
/* Run frames until no damage is left; with `atomic`, the screen may only
 * ever equal the old picture or the final one. Returns the frames used. */
static unsigned run(bool atomic) {
    memcpy(before, screen, sizeof screen);
    unsigned frames = 0;
    bool changed = false;
    for (;;) {
        int done = p.frame(&v, BUDGET, UINT32_MAX);
        ++frames;
        assert(frames < 200);
        if (atomic && memcmp(screen, before, sizeof screen)) {
            assert(!changed || done);   // one change only: the present
            changed = true;
        }
        if (done) break;
    }
    return frames;
}
static void matches_reference() { reference(); assert(!memcmp(screen, ref, sizeof screen)); }
/* Frames until the pass composes in the shadow (the screen copy is done). */
static void until_offscreen() {
    for (int i = 0; i < 20 && !p.composing_offscreen(); ++i) {
        memcpy(before, screen, sizeof screen);
        assert(p.frame(&v, BUDGET, UINT32_MAX) == 0);
        assert(!memcmp(screen, before, sizeof screen));   // copying, not composing
    }
    assert(p.composing_offscreen());
}

int main(int argc, char **argv) {
    assert(argc >= 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f && fread(font, 1, sizeof font, f) == sizeof font);
    fclose(f);
    for (int y = 0; y < 480; ++y) { screen_rows[y] = screen[y]; ref_rows[y] = ref[y]; }
    assert(vds_init(&v, screen_rows, 640, 480, font));
    p.attach(shadow, 480, 320);
    p.reset(&v, screen_rows);

    /* The first desktop is a full repaint: it appears in one present. */
    unsigned frames = run(true);
    assert(frames > 3 && p.presents == 1 && p.last_rows == 480 && !p.active() && v.rows == screen_rows);
    matches_reference();

    /* A small window, and a console-sized one, go straight to the screen. */
    assert(vds_surface_init(&v, 3, 30, 8, small_cells, 0x30));
    assert(vds_put(&v, 3, 1, 1, 0x30, "small", 5));
    assert(vds_window_open(&v, 3, 3, 64, 64, 30, 8, VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW, "Small"));
    run(false);
    assert(p.presents == 1);
    matches_reference();
    assert(vds_window_open(&v, 4, 3, 16, 228, 74, 28, VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW, "Console"));
    run(false);
    assert(vds_window_close(&v, 4));
    run(false);
    assert(p.presents == 1);
    matches_reference();

    /* The full-screen workbench opening and closing: atomic both ways. */
    assert(vds_surface_init(&v, 10, 80, 60, wb_cells, 0x03));
    for (int r = 0; r < 60; ++r) assert(vds_put(&v, 10, 0, (uint8_t)r, 0x03, "workbench row", 13));
    assert(vds_window_open(&v, 26, 10, 0, 0, 80, 60, VDS_W_VISIBLE, "Workbench"));
    run(true);
    assert(p.presents == 2 && p.last_rows == 480);
    matches_reference();
    assert(vds_window_close(&v, 26));
    run(true);
    assert(p.presents == 3);
    matches_reference();

    /* Outside a pass a scroll is a row copy (blit) ... */
    uint32_t blits = v.stats.blits;
    assert(vds_scroll_rect(&v, 3, 0, 0, 30, 8, 1, 0x30));
    assert(v.stats.blits == blits + 1 && !v.no_blit);
    run(false);
    matches_reference();
    /* ... during one it recomposes instead (no row copies in the shadow, no
     * screen moving under the copy), while composing and while copying. */
    assert(vds_window_open(&v, 26, 10, 0, 0, 80, 60, VDS_W_VISIBLE, "Workbench"));
    assert(vds_window_raise(&v, 3));
    until_offscreen();
    assert(p.frame(&v, BUDGET, UINT32_MAX) == 0 && v.no_blit);
    blits = v.stats.blits;
    assert(vds_scroll_rect(&v, 3, 0, 0, 30, 8, 1, 0x30) && v.stats.blits == blits);
    run(true);
    assert(p.presents == 4 && !v.no_blit);
    matches_reference();
    assert(vds_window_close(&v, 26));
    assert(p.frame(&v, BUDGET, UINT32_MAX) == 0 && p.active() && !p.composing_offscreen() && v.no_blit);
    assert(vds_scroll_rect(&v, 3, 0, 0, 30, 8, 1, 0x30) && v.stats.blits == blits);
    run(true);
    assert(p.presents == 5);
    matches_reference();

    /* The frame cap presents what is done; the rest is drawn on the screen. */
    p.max_frames = 2;
    p.sync_rows = 480;
    assert(vds_window_open(&v, 26, 10, 0, 0, 80, 60, VDS_W_VISIBLE, "Workbench"));
    run(false);
    assert(p.forced == 1 && !p.active());
    matches_reference();
    /* A copy that cannot finish within the cap is abandoned: drawn directly. */
    p.sync_rows = 100;
    assert(vds_window_close(&v, 26));
    run(false);
    assert(p.abandoned == 1);
    matches_reference();
    p.max_frames = 30;
    p.sync_rows = 120;
    assert(vds_window_open(&v, 26, 10, 0, 0, 80, 60, VDS_W_VISIBLE, "Workbench"));
    run(true);

    /* A new framebuffer mid-pass drops the pass; the caller re-damages. */
    assert(vds_window_close(&v, 26));
    until_offscreen();
    assert(v.rows != screen_rows);
    p.reset(&v, screen_rows);
    assert(!p.active() && v.rows == screen_rows && !v.row_written);
    vds_damage_all(&v);
    run(true);
    matches_reference();

    /* A finished pass presents only in a frame that may (started at vblank). */
    assert(vds_window_open(&v, 26, 10, 0, 0, 80, 60, VDS_W_VISIBLE, "Workbench"));
    {unsigned presents = p.presents, guard = 0;
     while (p.presents == presents && guard++ < 100) {
         memcpy(before, screen, sizeof screen);
         p.frame(&v, [&]() { return vds_frame_within(&v, BUDGET, UINT32_MAX); }, false);
         assert(!memcmp(screen, before, sizeof screen));   // never presents
         if (!v.ndamage && p.composing_offscreen()) break;
     }
     assert(p.presents == presents && p.deferred > 0 && p.composing_offscreen());
     p.frame(&v, [&]() { return vds_frame_within(&v, BUDGET, UINT32_MAX); }, true);
     assert(p.presents == presents + 1);}
    matches_reference();
    assert(vds_window_close(&v, 26));
    run(true);

    /* Without shadow memory the presenter is the plain compositor. */
    Presenter plain;
    plain.reset(&v, screen_rows);
    vds_damage_all(&v);
    while (!plain.frame(&v, BUDGET, UINT32_MAX)) assert(!plain.active());
    matches_reference();
    puts("offscreen presenter checks passed");
    return 0;
}

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

    /* A small window goes straight to the screen (no off-screen pass). */
    assert(vds_surface_init(&v, 3, 30, 8, small_cells, 0x30));
    assert(vds_put(&v, 3, 1, 1, 0x30, "small", 5));
    assert(vds_window_open(&v, 3, 3, 64, 64, 30, 8, VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW, "Small"));
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

    /* A scroll blit during an off-screen pass moves shadow rows: all rows are presented. */
    assert(vds_window_open(&v, 26, 10, 0, 0, 80, 60, VDS_W_VISIBLE, "Workbench"));
    assert(p.frame(&v, BUDGET, UINT32_MAX) == 0 && p.active());
    assert(vds_scroll(&v, 3, 1, 0x30));
    run(true);
    assert(p.presents == 4 && p.last_rows == 480);
    matches_reference();
    assert(vds_window_close(&v, 26));
    run(true);

    /* The frame cap presents what is done; the rest is drawn on the screen. */
    p.max_frames = 2;
    assert(vds_window_open(&v, 26, 10, 0, 0, 80, 60, VDS_W_VISIBLE, "Workbench"));
    run(false);
    assert(p.forced == 1 && !p.active());
    matches_reference();
    p.max_frames = 30;

    /* A new framebuffer mid-pass drops the pass; the caller re-damages. */
    assert(vds_window_close(&v, 26));
    assert(p.frame(&v, BUDGET, UINT32_MAX) == 0 && p.active() && v.rows != screen_rows);
    p.reset(&v, screen_rows);
    assert(!p.active() && v.rows == screen_rows && !v.row_written);
    vds_damage_all(&v);
    run(true);
    matches_reference();

    /* Without shadow memory the presenter is the plain compositor. */
    Presenter plain;
    plain.reset(&v, screen_rows);
    vds_damage_all(&v);
    while (!plain.frame(&v, BUDGET, UINT32_MAX)) assert(!plain.active());
    matches_reference();
    puts("offscreen presenter checks passed");
    return 0;
}

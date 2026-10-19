/* Host test of the console terminal drawn into a display-server surface
 * (include/vios_console_vds.h): the surface mirrors the terminal's text
 * through output, scrolling and harness framing; the cursor is inverted; the
 * console window follows the newest row; the composited screen stays exact. */
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
static void vios_probe_glyphs(const char *, size_t) {} /* firmware latency probe (vios_bridge.h) */
#include "vios_console_vds.h"
using namespace vios_console;

static uint8_t fb_a[480][320], fb_b[480][320], *rows_a[480], *rows_b[480], font[256 * 8];
static vds_cell cells[80 * 59];
static vds v;
static const uint8_t SURFACE = 16, WINDOW = 32;

static std::string surface_row(unsigned y) {
    std::string s;
    for (unsigned x = 0; x < COLS; ++x) s += (char)cells[y * COLS + x].ch;
    s.erase(s.find_last_not_of(' ') + 1);
    return s;
}
static void finish() { for (int i = 0; i < 1000; ++i) if (vds_frame(&v, UINT32_MAX)) return; assert(0); }
static void matches_full_render() {
    finish();
    v.rows = rows_b; vds_damage_all(&v); finish(); v.rows = rows_a;
    assert(!memcmp(fb_a, fb_b, sizeof fb_a));
}
static void feed(Terminal &t, const char *s, uint32_t now = 0) { t.feed((const uint8_t *)s, strlen(s), now); }
static void drain(Terminal &t) { for (int i = 0; i < 100 && t.pending(); ++i) t.service(2800); t.service(2800); }
/* Every terminal row is mirrored in the surface (ignoring the cursor cell's attr). */
static void agree(const Terminal &t) {
    for (unsigned y = 0; y < ROWS; ++y) {
        char b[COLS + 1];
        t.row_text(y, b, sizeof b);
        assert(surface_row(y) == b);
    }
}

int main(int argc, char **argv) {
    assert(argc >= 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f && fread(font, 1, sizeof font, f) == sizeof font);
    fclose(f);
    for (int y = 0; y < 480; ++y) { rows_a[y] = fb_a[y]; rows_b[y] = fb_b[y]; }
    assert(vds_init(&v, rows_a, 640, 480, font));
    assert(vds_surface_init(&v, SURFACE, COLS, ROWS + 1, cells, 0x30));
    assert(vds_window_open(&v, WINDOW, SURFACE, 16, 200, 78, 20, VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW, "Console"));
    VdsConsoleRenderer renderer(&v, SURFACE, WINDOW);
    Terminal t(&renderer);
    t.show(0);
    drain(t);
    assert(surface_row(STATUS_ROW).find("VIOS console") != std::string::npos);
    assert(cells[STATUS_ROW * COLS].attr == 0x03);                       /* status bar inverted */

    feed(t, "boot confirmed B\r\n@12345678 !ABCDEF01 1 2 + .\r\n3 \r\nok\r\nvios> ");
    drain(t);
    agree(t);
    assert(surface_row(0) == "boot confirmed B" && surface_row(1) == "1 2 + .");   /* framing hidden */
    assert(t.cursor_row() == 4 && cells[4 * COLS + 6].attr == 0x03 && v.windows[WINDOW].scroll_row == 0);  /* cursor after "vios> " */
    matches_full_render();

    /* Output past the window scrolls it to follow the cursor ... */
    for (int i = 0; i < 40; ++i) { char line[32]; snprintf(line, sizeof line, "line %d\r\n", i); feed(t, line); }
    drain(t);
    agree(t);
    assert(t.cursor_row() == 44 && v.windows[WINDOW].scroll_row == t.cursor_row() - 20 + 1);
    matches_full_render();
    /* ... and past the terminal's 58 rows the surface scrolls with it. */
    for (int i = 40; i < 120; ++i) { char line[32]; snprintf(line, sizeof line, "line %d\r\n", i); feed(t, line); }
    drain(t);
    agree(t);
    assert(surface_row(ROWS - 2) == "line 119" && t.cursor_row() == ROWS - 1 && v.windows[WINDOW].scroll_row == ROWS - 1 - 20 + 1);
    matches_full_render();
    /* A whole screen at once (pending scroll >= ROWS) redraws instead. */
    std::string flood;
    for (int i = 0; i < 70; ++i) flood += "flood " + std::to_string(i) + "\r\n";
    feed(t, flood.c_str());
    drain(t);
    agree(t);
    matches_full_render();
    /* Colours change on a style switch: redraw everything in the new attrs. */
    renderer.colours(0x42, 0x24);
    t.show(0);
    drain(t);
    agree(t);
    assert(cells[0].attr == 0x42 || cells[0].ch == ' ');
    matches_full_render();
    /* The workbench window (26, full screen, opened later) stacks above the console. */
    static vds_cell screen[80 * 60];
    assert(vds_surface_init(&v, 10, 80, 60, screen, 0x30));
    assert(vds_window_open(&v, 26, 10, 0, 0, 80, 60, VDS_W_VISIBLE, ""));
    assert(v.order[v.count - 1] == 26 && vds_hit(&v, 320, 300, nullptr) == 26);
    printf("console on vds checks passed\n");
    return 0;
}

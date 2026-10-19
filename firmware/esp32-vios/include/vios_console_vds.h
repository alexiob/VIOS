/* The ESP console terminal (vios_console.h) drawn into a display-server
 * surface instead of the framebuffer, for the GUI: the console becomes a
 * window like any other. Rows 0..57 and the status row 58 map to surface rows;
 * scrolls are surface rectangle scrolls; the window keeps the cursor row in
 * view. Portable C++ over the C display-server core (lib/vios-vds). MIT. */
#pragma once
#include "vios_console.h"
extern "C" {
#include "vds.h"
}

class VdsConsoleRenderer : public vios_console::Renderer {
    vds *v;
    uint8_t surface, window, normal = 0x30, inverse = 0x03;
public:
    VdsConsoleRenderer(vds *display, uint8_t surface_id, uint8_t window_id)
        : v(display), surface(surface_id), window(window_id) {}
    /* Text colours (attr = fg | bg << 4) for ordinary and inverted cells. */
    void colours(uint8_t ordinary, uint8_t inverted) { normal = ordinary; inverse = inverted; }
    uint8_t ordinary() const { return normal; }
    void cells(unsigned x, unsigned y, const char *text, unsigned count, bool inverted) override {
        vds_put(v, surface, (uint8_t)x, (uint8_t)y, inverted ? inverse : normal, text, count);
        vios_probe_glyphs(text, count);   /* latency probe: key -> console surface (composited next frame step) */
        if (inverted && y < vios_console::ROWS && count == 1) follow(y);   /* the cursor */
    }
    void scroll_up(unsigned rows) override {
        vds_scroll_rect(v, surface, 0, 0, vios_console::COLS, vios_console::ROWS, (int)rows, normal);
    }
    void clear_tail() override {}   /* the surface has no pixels outside its cells */
    /* Scroll the window so surface row y is visible (the newest output). */
    void follow(unsigned y) {
        if (window >= VDS_MAX_WINDOWS || !v->windows[window].used) return;
        const vds_window *w = &v->windows[window];
        unsigned top = y >= w->rows ? y - w->rows + 1 : 0;
        if (top != w->scroll_row) vds_window_scroll(v, window, 0, (uint8_t)top);
    }
};

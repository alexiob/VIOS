/* VDP binding of the ESP console terminal (vios_console.h). MIT.
 * Runs only on the VDP task: E0 records, VDU 23,0,E5 and the loop tick all
 * execute there, the same task that owns the fabgl canvas. Mode 1 (80x60,
 * four colours) is set by the CPU before it shows the view; text is white
 * (logical colour 3) on black (0), the cursor and status bar inverted. */
#pragma once
#include "vios_console.h"

class VdpConsoleRenderer : public vios_console::Renderer {
    static RGB888 colour(uint8_t c) { return colourLookup[palette[c % getVGAColourDepth()]]; }
    void prepare(bool inverse) {
        vios_glyph_state_valid = false; // plotCharacter must restore its own state next time
        canvas->setClippingRect(Rect(0, 0, canvas->getWidth() - 1, canvas->getHeight() - 1));
        canvas->setPaintOptions(fabgl::PaintOptions());
        canvas->setPenColor(colour(inverse ? 0 : 3));
        canvas->setBrushColor(colour(inverse ? 3 : 0));
    }
public:
    void cells(unsigned x, unsigned y, const char *text, unsigned count, bool inverse) override {
        if (ttxtMode) return;
        prepare(inverse);
        for (unsigned i = 0; i < count; ++i) {
            vios_probe_glyph(text[i]);
            canvas->drawChar((x + i) * fontW, y * fontH, text[i]);
        }
    }
    void scroll_up() override {
        if (ttxtMode) return;
        prepare(false);
        canvas->setScrollingRegion(0, 0, canvas->getWidth() - 1, vios_console::ROWS * fontH - 1);
        canvas->scroll(0, -fontH);
    }
    void clear_screen() override {
        if (ttxtMode) return;
        canvas->waitCompletion(); // A preceding mode change/CLS must finish first.
        prepare(false);
        canvas->clear();
    }
};

static VdpConsoleRenderer vios_console_renderer;
static vios_console::Terminal vios_console_terminal(&vios_console_renderer);

void vios_console_feed(const uint8_t *data, size_t length) { vios_console_terminal.feed(data, length, millis()); }
void vios_console_view(uint8_t op) {
    switch (op) {
        case 0: vios_console_terminal.hide(); break;
        case 1: vios_console_terminal.show(millis()); break;
        case 2: vios_console_terminal.reset(); break;
    }
}
void vios_console_tick() {
    static uint32_t last;
    uint32_t now = millis();
    if (now - last >= 50) { last = now; vios_console_terminal.tick(now); }
}
unsigned vios_console_row(unsigned y, char *out, unsigned capacity) {
    if (y == vios_console::STATUS_ROW) {
        char line[vios_console::COLS]; vios_console::Terminal::status_line(line);
        unsigned n = 0; for (unsigned x = 0; x < vios_console::COLS && n + 1 < capacity; ++x) if (line[x] != ' ' || x + 1 < vios_console::COLS) out[n++] = line[x];
        while (n && out[n - 1] == ' ') --n; out[n] = 0; return n;
    }
    if (y >= vios_console::ROWS) { out[0] = 0; return 0; }
    return vios_console_terminal.row_text(y, out, capacity);
}

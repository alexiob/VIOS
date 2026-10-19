/* VDP binding of the ESP console terminal (vios_console.h). MIT.
 * Runs only on the VDP task: E0 records, VDU 23,0,E5 and the loop tick all
 * execute there, the same task that owns the fabgl canvas. Mode 1 (80x60,
 * four colours) is set by the CPU before it shows the view; text is white
 * (logical colour 3) on black (0), the cursor and status bar inverted. */
#pragma once
#include "vios_console.h"

/* Framebuffer rows of the paletted VGA controllers (logical indices). */
struct VdpRows : fabgl::VGAPalettedController {
    static uint8_t *row(int y) { return sgetScanline(y); }
    /* The controller's row-pointer table. Single-buffered, the drawing and the
     * visible tables are the same array, which the line ISR also reads. */
    static volatile uint8_t **table() {
        auto self = reinterpret_cast<VdpRows *>(static_cast<fabgl::VGAPalettedController *>(fabgl::VGA16Controller::instance()));
        return self->m_viewPort == self->m_viewPortVisible ? self->m_viewPort : nullptr;
    }
};

class VdpConsoleRenderer : public vios_console::Renderer {
    uint32_t lut[2][256]; // 4 bpp: font byte -> 8 pixels, [0] white on black, [1] inverse
    bool lut_ready = false;
    static RGB888 colour(uint8_t c) { return colourLookup[palette[c % getVGAColourDepth()]]; }
    /* Mode 0 (16 colours, 4 bpp): draw straight into the framebuffer. Logical
     * colours 3 and 0 are pinned (vios_display_mode0), so their indices are fixed. */
    static bool direct() { return !ttxtMode && videoMode == 0 && getVGAColourDepth() == 16 && fontW == 8 && fontH == 8; }
    void build() {
        for (unsigned inv = 0; inv < 2; ++inv) {
            uint8_t fg = inv ? 0 : 3, bg = inv ? 3 : 0;
            for (unsigned b = 0; b < 256; ++b) {
                uint32_t v = 0;
                for (unsigned k = 0; k < 4; ++k) {
                    uint8_t left = (b >> (7 - 2 * k)) & 1 ? fg : bg, right = (b >> (6 - 2 * k)) & 1 ? fg : bg;
                    v |= (uint32_t)((left << 4) | right) << (8 * k);
                }
                lut[inv][b] = v;
            }
        }
        lut_ready = true;
    }
    void prepare(bool inverse) {
        vios_glyph_state_valid = false; // plotCharacter must restore its own state next time
        canvas->setClippingRect(Rect(0, 0, canvas->getWidth() - 1, canvas->getHeight() - 1));
        canvas->setPaintOptions(fabgl::PaintOptions());
        canvas->setPenColor(colour(inverse ? 0 : 3));
        canvas->setBrushColor(colour(inverse ? 3 : 0));
    }
public:
    /* Cells per VDP loop iteration: under 1 ms with the blitter (~720 cells/ms),
     * a few queued fabgl glyphs otherwise. */
    unsigned budget() const { return direct() ? 600 : 40; }
    void cells(unsigned x, unsigned y, const char *text, unsigned count, bool inverse) override {
        if (ttxtMode) return;
        if (direct()) {
            if (!lut_ready) build();
            canvas->waitCompletion(false); // run queued fabgl drawing now, not at the next vblank
            const uint32_t *table = lut[inverse ? 1 : 0];
            uint32_t *rows[8];
            for (unsigned r = 0; r < 8; ++r) rows[r] = (uint32_t *)VdpRows::row(y * 8 + r) + x;
            for (unsigned i = 0; i < count; ++i) {
                const uint8_t *glyph = &fabgl::FONT_AGON_DATA[(uint8_t)text[i] * 8];
                rows[0][i] = table[glyph[0]]; rows[1][i] = table[glyph[1]];
                rows[2][i] = table[glyph[2]]; rows[3][i] = table[glyph[3]];
                rows[4][i] = table[glyph[4]]; rows[5][i] = table[glyph[5]];
                rows[6][i] = table[glyph[6]]; rows[7][i] = table[glyph[7]];
            }
            vios_probe_glyphs(text, count);
            return;
        }
        prepare(inverse);
        for (unsigned i = 0; i < count; ++i) {
            vios_probe_glyph(text[i]);
            canvas->drawChar((x + i) * fontW, y * fontH, text[i]);
        }
    }
    void scroll_up(unsigned rows) override {
        if (ttxtMode) return;
        if (direct()) {
            canvas->waitCompletion(false);
            unsigned bytes = canvas->getWidth() / 2, last = vios_console::ROWS * 8, shift = rows * 8;
            volatile uint8_t **t = VdpRows::table();
            if (t) {
                /* Rotate the console's row pointers: O(rows), not 146 KB of copying
                 * through a core the line ISR shares. Exposed rows are cleared. */
                volatile uint8_t *moved[vios_console::ROWS * 8];
                for (unsigned i = 0; i < shift; ++i) moved[i] = t[i];
                for (unsigned py = 0; py + shift < last; ++py) t[py] = t[py + shift];
                for (unsigned i = 0; i < shift; ++i) t[last - shift + i] = moved[i];
            } else {
                for (unsigned py = 0; py + shift < last; ++py) memcpy(VdpRows::row(py), VdpRows::row(py + shift), bytes);
            }
            for (unsigned py = last - shift; py < last; ++py) memset(VdpRows::row(py), 0, bytes);
            return;
        }
        prepare(false);
        canvas->setScrollingRegion(0, 0, canvas->getWidth() - 1, vios_console::ROWS * fontH - 1);
        canvas->scroll(0, -(int)(rows * fontH));
    }
    void clear_tail() override {
        if (ttxtMode) return;
        canvas->waitCompletion(false); // A preceding mode change/CLS must finish first.
        int first = (vios_console::STATUS_ROW + 1) * fontH;
        if (direct()) {
            unsigned bytes = canvas->getWidth() / 2;
            for (int py = first; py < canvas->getHeight(); ++py) memset(VdpRows::row(py), 0, bytes);
            return;
        }
        prepare(false);
        canvas->fillRectangle(0, first, canvas->getWidth() - 1, canvas->getHeight() - 1);
    }
};

static VdpConsoleRenderer vios_console_renderer;
static vios_console::Terminal vios_console_terminal(&vios_console_renderer);

void vios_frame_kick();
void vios_console_feed(const uint8_t *data, size_t length) {
    vios_display_lock();
    vios_console_terminal.feed(data, length, millis());
    bool small = vios_console_terminal.small_pending(160);
    vios_display_unlock();
    if (small) vios_frame_kick(); // typing echo: draw now, not at the next vblank
}
bool vios_gui_console_view(uint8_t op);
bool vios_gui_owns_display();
void vios_console_view(uint8_t op) {
    if (vios_gui_console_view(op)) return;   // the console is a GUI window
    vios_display_lock();
    switch (op) {
        case 0: vios_console_terminal.hide(); break;
        case 1: vios_console_terminal.show(millis()); break;
        case 2: vios_console_terminal.reset(); break;
    }
    vios_display_unlock();
}
/* Every VDP loop iteration: draw pending console cells within the budget
 * (bounded loop gap), and the cursor blink at 50 ms resolution. */
void vios_console_tick() {} // drawing and blink run in the frame task (vios_frame.h)
unsigned vios_console_row(unsigned y, char *out, unsigned capacity) {
    if (y == vios_console::STATUS_ROW) {
        char line[vios_console::COLS]; vios_console::Terminal::status_line(line);
        unsigned n = 0; for (unsigned x = 0; x < vios_console::COLS && n + 1 < capacity; ++x) if (line[x] != ' ' || x + 1 < vios_console::COLS) out[n++] = line[x];
        while (n && out[n - 1] == ' ') --n; out[n] = 0; return n;
    }
    if (y >= vios_console::ROWS) { out[0] = 0; return 0; }
    return vios_console_terminal.row_text(y, out, capacity);
}

/* Mode 0 (640x480, 16 colours) with logical 0-3 = mode 1's colours, so every
 * existing attr (fg | bg<<4, 0-3) keeps its look; theme roles use 4-15. The
 * VDP's palette[] (used to resolve COLOUR) and the controller stay in step. */
void vios_display_mode0() {
    if (vios_gui_owns_display()) return;      // already mode 0 under the GUI: never reallocate it
    vios_display_lock();
    if (canvas) canvas->waitCompletion(); // at boot set_mode creates the canvas
    set_mode(0);
    if (videoMode != 0 || getVGAColourDepth() != 16) { vios_display_unlock(); return; }
    static const uint8_t pinned[4] = {0x00, 0x30, 0x3C, 0x3F};
    for (uint8_t i = 0; i < 4; ++i) { palette[i] = pinned[i]; setPaletteItem(i, colourLookup[pinned[i]]); }
    setTextColour(3); setTextColour(128);
    vios_glyph_state_valid = false;
    vios_display_unlock();
}

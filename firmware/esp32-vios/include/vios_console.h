/* VIOS console terminal on the ESP (GUI Stage 1c). MIT.
 *
 * The CPU's console output already reaches the ESP as VDU 23,0,E0 records on
 * its way to USB. This terminal interprets the same bytes into a shadow of
 * the console text and draws it on the VGA screen while the console view is
 * shown, so the CPU no longer taps, buffers or repaints console output.
 *
 * Behaviour is a port of the former CPU view (kernel/workbench_runtime.c,
 * con_*): rows 0..57 scroll, row 58 is the status bar; harness framing is
 * hidden (a line-start or after-prompt "@NONCE " and the following
 * "!CRC32XX " are dropped, a bare "@NONCE" frame line is skipped); output
 * that arrives while the editor is shown is kept and drawn on return; a block
 * cursor blinks with a 500 ms phase and stays solid while output arrives.
 *
 * Portable: no Arduino/fabgl dependencies; drawing goes through Renderer.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace vios_console {

static const unsigned COLS = 80, ROWS = 58, STATUS_ROW = 58;
static const uint32_t BLINK_MS = 500;

struct Renderer {
    virtual ~Renderer() {}
    virtual void cells(unsigned x, unsigned y, const char *text, unsigned count, bool inverse) = 0;
    virtual void scroll_up() = 0;   // rows 0..ROWS-1 up one row; the exposed row cleared
    virtual void clear_screen() = 0;
};

class Terminal {
public:
    explicit Terminal(Renderer *renderer = nullptr) : out(renderer) { reset(); }
    void attach(Renderer *renderer) { out = renderer; }

    /* Forget everything captured (WB-CONSOLE-CLEAR); redraws when shown. */
    void reset() {
        for (unsigned y = 0; y < ROWS; ++y) for (unsigned x = 0; x < COLS; ++x) shadow[y][x] = ' ';
        top = col = row = 0; state = held = 0; bol = true; cursor_on = false;
        if (shown) redraw();
    }
    /* Console output bytes, in stream order. */
    void feed(const uint8_t *data, size_t length, uint32_t now) {
        if (!length) return;
        hide_cursor();
        for (size_t i = 0; i < length; ++i) byte(data[i]);
        blink_start = now; // Solid cursor while output arrives.
        if (shown) show_cursor();
    }
    /* View becomes visible (after the CPU reset the display mode). */
    void show(uint32_t now) { shown = true; blink_start = now; redraw(); }
    void hide() { cursor_on = false; shown = false; }
    bool visible() const { return shown; }
    void tick(uint32_t now) {
        if (!shown) return;
        bool on = ((now - blink_start) / BLINK_MS) % 2 == 0;
        if (on) show_cursor(); else hide_cursor();
    }
    /* Test/inspection access: visual row y without trailing spaces. */
    unsigned row_text(unsigned y, char *out_text, unsigned capacity) const {
        unsigned used = 0;
        for (unsigned x = 0; x < COLS; ++x) if (line(y)[x] != ' ') used = x + 1;
        if (used >= capacity) used = capacity - 1;
        for (unsigned x = 0; x < used; ++x) out_text[x] = line(y)[x];
        out_text[used] = 0; return used;
    }
    unsigned cursor_col() const { return col; }
    unsigned cursor_row() const { return row; }

    static void status_line(char out_line[COLS]) {
        static const char text[] = " VIOS console   F1: editor   Up/Down: history";
        unsigned n = 0;
        for (; text[n] && n < COLS; ++n) out_line[n] = text[n];
        for (; n < COLS; ++n) out_line[n] = ' ';
    }

private:
    Renderer *out;
    char shadow[ROWS][COLS];
    unsigned top, col, row, state, held, cursor_x = 0, cursor_y = 0;
    bool bol, cursor_on, shown = false;
    char hold[10];
    uint32_t blink_start = 0;

    char *line(unsigned y) { return shadow[(top + y) % ROWS]; }
    const char *line(unsigned y) const { return shadow[(top + y) % ROWS]; }

    void redraw() {
        if (!out) return;
        cursor_on = false;
        out->clear_screen();
        for (unsigned y = 0; y < ROWS; ++y) {
            unsigned used = 0;
            for (unsigned x = 0; x < COLS; ++x) if (line(y)[x] != ' ') used = x + 1;
            if (used) out->cells(0, y, line(y), used, false);
        }
        char status[COLS]; status_line(status);
        out->cells(0, STATUS_ROW, status, COLS, true);
        show_cursor();
    }
    void hide_cursor() {
        if (!cursor_on) return;
        cursor_on = false;
        if (shown && out) out->cells(cursor_x, cursor_y, &line(cursor_y)[cursor_x], 1, false);
    }
    void show_cursor() {
        if (!shown || !out) return;
        if (cursor_on && (cursor_x != col || cursor_y != row)) hide_cursor();
        if (cursor_on) return;
        cursor_on = true; cursor_x = col; cursor_y = row;
        out->cells(col, row, &line(row)[col], 1, true);
    }
    void newline() {
        col = 0;
        if (row + 1 < ROWS) { ++row; return; }
        for (unsigned x = 0; x < COLS; ++x) line(0)[x] = ' ';
        top = (top + 1) % ROWS;
        if (shown && out) out->scroll_up();
    }
    void draw(unsigned char c) {
        bol = c == '\n';
        if (c == '\r') { col = 0; return; }
        if (c == '\n') { newline(); return; }
        if (c == 8) { if (col) --col; else if (row) { --row; col = COLS - 1; } return; }
        if (c < 32 || c > 126) return;
        line(row)[col] = (char)c;
        if (shown && out) out->cells(col, row, &line(row)[col], 1, false);
        if (++col == COLS) newline();
    }
    static bool hex(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); }
    /* Host envelopes start at a line start or right after the REPL prompt. */
    bool after_prompt() const {
        static const char *const prompts[] = {"vios> ", "compile> "};
        for (unsigned k = 0; k < 2; ++k) {
            unsigned n = 0; while (prompts[k][n]) ++n;
            if (col != n) continue;
            unsigned i = 0; while (i < n && line(row)[i] == prompts[k][i]) ++i;
            if (i == n) return true;
        }
        return false;
    }
    void release() { unsigned n = held; held = 0; state = 0; for (unsigned i = 0; i < n; ++i) draw((unsigned char)hold[i]); }
    void byte(unsigned char c) {
        /* 0 text, 1 nonce digits, 2 CRC digits, 3 skip to LF, 4 after nonce. */
        if (state == 3) { if (c == '\n') { state = 0; bol = true; } return; }
        if (state == 4) { state = 0; if (c == '!') { state = 2; hold[0] = '!'; held = 1; return; } }
        if (state == 1 || state == 2) {
            hold[held++] = (char)c;
            if (held <= 9 && hex(c)) return;
            if (held == 10 && c == ' ') { held = 0; state = state == 1 ? 4 : 0; return; }
            if (held == 10 && state == 1 && c == '\r') { held = 0; state = 3; return; }
            release(); return;
        }
        if (c == '@' && (bol || after_prompt())) { state = 1; hold[0] = '@'; held = 1; return; }
        draw(c);
    }
};

} // namespace vios_console

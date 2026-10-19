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
 * Drawing is deferred and bounded (GUI G1 loop-gap rule): output marks dirty
 * column spans per row and service(budget) draws them top to bottom within a
 * cell budget per VDP loop iteration; show() marks every row at full width
 * (drawn spaces clear old pixels) and draws nothing itself: a full-screen
 * clear costs ~5 ms in mode 0, where the line ISR shares the core. Scrolls are counted and applied by service() as one row move (or a
 * clear when a whole screen scrolled); dirty spans are kept in the shadow's
 * coordinates. The cursor is drawn only when nothing is pending.
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
    virtual void scroll_up(unsigned rows) = 0; // rows 0..ROWS-1 up by 1..ROWS-1; exposed rows cleared
    virtual void clear_tail() = 0;  // pixels below the status row (not covered by any cell)
};

class Terminal {
public:
    explicit Terminal(Renderer *renderer = nullptr) : out(renderer) { reset(); }
    void attach(Renderer *renderer) { out = renderer; }

    /* Forget everything captured (WB-CONSOLE-CLEAR); redraws when shown. */
    void reset() {
        for (unsigned y = 0; y < ROWS; ++y) { for (unsigned x = 0; x < COLS; ++x) shadow[y][x] = ' '; dirty_lo[y] = dirty_hi[y] = 0; }
        top = col = row = 0; state = held = 0; bol = true; cursor_on = false; pending_scroll = 0;
        if (shown && out) mark_all();
    }
    /* Console output bytes, in stream order. */
    void feed(const uint8_t *data, size_t length, uint32_t now) {
        if (!length) return;
        hide_cursor();
        for (size_t i = 0; i < length; ++i) byte(data[i]);
        blink_start = now; cursor_phase = true; // Solid cursor while output arrives.
    }
    /* View becomes visible (after the CPU reset the screen): clear now, draw
     * the captured text through service(). */
    void show(uint32_t now) {
        shown = true; blink_start = now; cursor_phase = true; cursor_on = false; pending_scroll = 0;
        if (!out) return;
        mark_all();
        out->clear_tail();
    }
    void hide() { cursor_on = false; shown = false; }
    bool visible() const { return shown; }
    bool pending() const {
        if (status_dirty || pending_scroll) return true;
        for (unsigned y = 0; y < ROWS; ++y) if (dirty_hi[y] > dirty_lo[y]) return true;
        return false;
    }
    /* Small updates (an echoed key, a short line, no scroll) cannot visibly
     * tear, so they may be drawn at once instead of at the next vertical blank. */
    bool small_pending(unsigned limit) const {
        if (status_dirty || pending_scroll) return false;
        unsigned cells = 0;
        for (unsigned y = 0; y < ROWS; ++y) if (dirty_hi[y] > dirty_lo[y]) cells += dirty_hi[y] - dirty_lo[y];
        return cells && cells <= limit;
    }
    /* Draw pending spans top to bottom, at most `budget` cells (at least one
     * span); then the cursor if nothing is left. Returns cells drawn. */
    unsigned service(unsigned budget) {
        if (!shown || !out) return 0;
        unsigned spent = 0;
        if (pending_scroll) {
            if (pending_scroll >= ROWS) mark_all();
            else out->scroll_up(pending_scroll);
            pending_scroll = 0;
        }
        if (status_dirty) { char status[COLS]; status_line(status); out->cells(0, STATUS_ROW, status, COLS, true); spent += COLS; status_dirty = false; }
        for (unsigned y = 0; y < ROWS; ++y) {
            unsigned lo = dirty_lo[y], hi = dirty_hi[y];
            if (hi <= lo) continue;
            if (spent && spent + (hi - lo) > budget) return spent;
            out->cells(lo, y, &line(y)[lo], hi - lo, false);
            spent += hi - lo; dirty_lo[y] = dirty_hi[y] = 0;
        }
        if (cursor_phase) show_cursor();
        return spent;
    }
    void tick(uint32_t now) {
        if (!shown) return;
        cursor_phase = ((now - blink_start) / BLINK_MS) % 2 == 0;
        if (pending()) return;
        if (cursor_phase) show_cursor(); else hide_cursor();
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
    bool bol, cursor_on, shown = false, status_dirty = false, cursor_phase = true;
    unsigned pending_scroll = 0; // screen lags the shadow by this many rows
    uint8_t dirty_lo[ROWS], dirty_hi[ROWS]; // pending column span per visual row
    char hold[10];
    uint32_t blink_start = 0;

    char *line(unsigned y) { return shadow[(top + y) % ROWS]; }
    const char *line(unsigned y) const { return shadow[(top + y) % ROWS]; }

    void mark_all() {
        for (unsigned y = 0; y < ROWS; ++y) { dirty_lo[y] = 0; dirty_hi[y] = (uint8_t)COLS; }
        status_dirty = true; cursor_on = false;
    }
    void mark(unsigned y, unsigned x) {
        if (!shown) return;
        if (dirty_hi[y] <= dirty_lo[y]) { dirty_lo[y] = (uint8_t)x; dirty_hi[y] = (uint8_t)(x + 1); return; }
        if (x < dirty_lo[y]) dirty_lo[y] = (uint8_t)x;
        if (x + 1 > dirty_hi[y]) dirty_hi[y] = (uint8_t)(x + 1);
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
        if (shown && out) {
            ++pending_scroll;
            for (unsigned y = 0; y + 1 < ROWS; ++y) { dirty_lo[y] = dirty_lo[y + 1]; dirty_hi[y] = dirty_hi[y + 1]; }
            dirty_lo[ROWS - 1] = dirty_hi[ROWS - 1] = 0;
        }
    }
    void draw(unsigned char c) {
        bol = c == '\n';
        if (c == '\r') { col = 0; return; }
        if (c == '\n') { newline(); return; }
        if (c == 8) { if (col) --col; else if (row) { --row; col = COLS - 1; } return; }
        if (c < 32 || c > 126) return;
        line(row)[col] = (char)c;
        mark(row, col);
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

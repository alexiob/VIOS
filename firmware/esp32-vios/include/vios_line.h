/* VIOS cooked line editor on the ESP (GUI Stage 2a). MIT.
 *
 * While the CPU's REPL waits for a line, physical keys are edited here
 * instead of on the CPU, so typing and its echo no longer wait for CPU time
 * slices. Semantics are a port of drivers/agon-vdp/transport.c local() and
 * boards/agon-light-2/platform_resident.c echo_local():
 *  - insert at cursor, Left/Right/Home/End, Backspace/Delete, Esc or Ctrl-C
 *    clear, Up/Down recall from a 4-line history, Enter submits;
 *  - the echo uses only printable bytes, spaces and BS and redraws only the
 *    changed tail, so any terminal (USB or the VGA console) renders it;
 *  - output from elsewhere while a line is shown makes the next echo redisplay
 *    the line on a fresh row (like readline);
 *  - a submitted line is erased from the echo before the CPU sees it, so the
 *    REPL's own echo is the only copy; it is handed over as bytes plus CR.
 * Portable: no Arduino/fabgl; echo and submit go through callbacks.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace vios_line {

static const unsigned LENGTH = 235, HISTORY = 4;
/* Virtual keys as sent in 0x81 packets (include/platform/input.h; the host
 * test checks every value against that header). */
enum Key : uint8_t { K_ESCAPE = 125, K_DELETE = 130, K_BACKSPACE = 132, K_HOME = 133, K_KP_HOME = 134,
                     K_END = 135, K_KP_END = 136, K_RETURN = 143, K_KP_ENTER = 144,
                     K_UP = 150, K_KP_UP = 151, K_DOWN = 152, K_KP_DOWN = 153,
                     K_LEFT = 154, K_KP_LEFT = 155, K_RIGHT = 156, K_KP_RIGHT = 157 };

struct Sink {
    virtual ~Sink() {}
    virtual void echo(const uint8_t *bytes, size_t n) = 0;    // console output (USB and VGA)
    virtual bool submit(const uint8_t *line, size_t n) = 0;   // line + CR to the CPU; false = no room, retry
};

class Field {
public:
    explicit Field(Sink *sink = nullptr) : out(sink) {}
    void attach(Sink *sink) { out = sink; }

    /* One key-down. Returns true when the editor consumed it. */
    bool key(uint8_t c, uint8_t vk) {
        if (pending) { if (is_edit(c, vk)) { ++dropped; return true; } return false; }
        /* Keys first, ASCII after: the VDP gives Down ASCII 10 and Left ASCII 8,
         * so a bare '\n' or 8 is only Enter or Backspace when no key says otherwise. */
        if (vk == K_LEFT || vk == K_KP_LEFT) { if (cursor) --cursor; }
        else if (vk == K_RIGHT || vk == K_KP_RIGHT) { if (cursor < used) ++cursor; }
        else if (vk == K_HOME || vk == K_KP_HOME) cursor = 0;
        else if (vk == K_END || vk == K_KP_END) cursor = used;
        else if (vk == K_UP || vk == K_KP_UP) recall(true);
        else if (vk == K_DOWN || vk == K_KP_DOWN) recall(false);
        else if (vk == K_DELETE) { if (cursor < used) erase_at(cursor); }
        else if (vk == K_RETURN || vk == K_KP_ENTER || c == '\r' || c == '\n') { remember(); history_pos = 0; pending = true; try_submit(); return true; }
        else if (vk == K_BACKSPACE || c == 8 || c == 127) { if (cursor) { --cursor; erase_at(cursor); } }
        else if (vk == K_ESCAPE || c == 3 || c == 27) { used = cursor = 0; history_pos = 0; }
        else if (c >= 32 && c < 127) {
            if (used < LENGTH) { for (unsigned i = used; i > cursor; --i) line[i] = line[i - 1]; line[cursor++] = (char)c; ++used; }
            else ++dropped;
        }
        else return false;
        render();
        return true;
    }
    /* Console output from elsewhere arrived while a line may be shown. */
    void foreign_output() { if (shown_length) foreign = true; }
    /* Retry a submit that found no room (call regularly). */
    void poll() { if (pending) try_submit(); }
    /* Line mode switched off by the CPU: a partial line stays for later. */
    bool busy() const { return pending; }
    unsigned length() const { return used; }
    unsigned dropped_keys() const { return dropped; }
    const char *text() const { return line; }

private:
    Sink *out;
    char line[LENGTH], shown[LENGTH];
    char history[HISTORY][LENGTH];
    uint8_t history_length[HISTORY] = {}, history_count = 0, history_next = 0, history_pos = 0;
    unsigned used = 0, cursor = 0, shown_length = 0, shown_cursor = 0, dropped = 0;
    bool pending = false, foreign = false;

    static bool is_edit(uint8_t c, uint8_t vk) {
        return (c >= 32 && c < 127) || c == 8 || c == 127 || c == 27 || c == 3 || c == '\r' || c == '\n' ||
               vk == K_LEFT || vk == K_RIGHT || vk == K_UP || vk == K_DOWN || vk == K_HOME || vk == K_END ||
               vk == K_DELETE || vk == K_BACKSPACE || vk == K_RETURN || vk == K_ESCAPE ||
               vk == K_KP_LEFT || vk == K_KP_RIGHT || vk == K_KP_UP || vk == K_KP_DOWN ||
               vk == K_KP_HOME || vk == K_KP_END || vk == K_KP_ENTER;
    }
    void erase_at(unsigned at) { for (unsigned i = at; i + 1 < used; ++i) line[i] = line[i + 1]; --used; }
    void remember() {
        if (!used) return;
        unsigned last = (history_next + HISTORY - 1) % HISTORY;
        if (history_count && history_length[last] == used) {
            bool same = true;
            for (unsigned i = 0; i < used; ++i) if (history[last][i] != line[i]) same = false;
            if (same) return;
        }
        for (unsigned i = 0; i < used; ++i) history[history_next][i] = line[i];
        history_length[history_next] = (uint8_t)used;
        history_next = (uint8_t)((history_next + 1) % HISTORY);
        if (history_count < HISTORY) ++history_count;
    }
    void recall(bool older) {
        /* history_pos 0 is the line being edited; 1 is the newest entry. */
        unsigned pos = history_pos;
        if (older) { if (pos >= history_count) return; ++pos; } else { if (!pos) return; --pos; }
        history_pos = (uint8_t)pos; used = 0;
        if (pos) {
            unsigned e = (history_next + HISTORY - pos) % HISTORY;
            for (unsigned i = 0; i < history_length[e]; ++i) line[i] = history[e][i];
            used = history_length[e];
        }
        cursor = used;
    }
    void try_submit() {
        if (!out) return;
        // Erase the shown line first: the REPL echoes what it reads.
        if (shown_length && !foreign) {
            uint8_t buf[3 * LENGTH]; size_t n = 0;
            for (unsigned i = 0; i < shown_cursor; ++i) buf[n++] = 8;
            for (unsigned i = 0; i < shown_length; ++i) buf[n++] = ' ';
            for (unsigned i = 0; i < shown_length; ++i) buf[n++] = 8;
            out->echo(buf, n);
        }
        shown_length = shown_cursor = 0; foreign = false; // erased once, even if the submit must wait
        uint8_t data[LENGTH + 1];
        for (unsigned i = 0; i < used; ++i) data[i] = (uint8_t)line[i];
        data[used] = '\r';
        if (!out->submit(data, used + 1)) return;
        used = cursor = 0; pending = false;
    }
    /* Show the edited line, redrawing only the changed tail. */
    void render() {
        if (!out) return;
        uint8_t buf[4 * LENGTH + 4]; size_t n = 0;
        if (foreign) {
            foreign = false;
            if (shown_length) { buf[n++] = '\r'; buf[n++] = '\n'; }
            shown_length = shown_cursor = 0;
        }
        unsigned i = 0;
        while (i < used && i < shown_length && shown[i] == line[i]) ++i;
        if (i == used && used == shown_length && cursor == shown_cursor) { if (n) out->echo(buf, n); return; }
        if (shown_cursor > i) for (unsigned k = i; k < shown_cursor; ++k) buf[n++] = 8;
        else for (unsigned k = shown_cursor; k < i; ++k) buf[n++] = (uint8_t)shown[k];
        for (unsigned k = i; k < used; ++k) buf[n++] = (uint8_t)line[k];
        unsigned end = used;
        for (; end < shown_length; ++end) buf[n++] = ' ';
        for (unsigned k = cursor; k < end; ++k) buf[n++] = 8;
        for (unsigned k = 0; k < used; ++k) shown[k] = line[k];
        shown_length = used; shown_cursor = cursor;
        out->echo(buf, n);
    }
};

} // namespace vios_line

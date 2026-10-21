/* /dev/win and /dev/ui/tree: the display server's windows and widgets as
 * files (spec 2026-10-19-vios-gui-toolkit.md section 10). The ESP owns their
 * state and also renders and parses the text (vds_files.c has the formats):
 * a read asks for the next bytes at the handle's cursor (op 9C), a write
 * sends its text in chunks cut at line ends (op 9D). The CPU keeps the
 * cursors of open handles (kernel/vios_handles.c) and the id /dev/win/new
 * made last, nothing else.
 *   /dev/win            id kind parent name rect, a line per node
 *   /dev/win/new        write "kind parent name"; read the id made
 *   /dev/win/N/ctl      properties; write commands (rect, text, show, focus, ...)
 *   /dev/win/N/text     the content; a handle's first write replaces it
 *   /dev/win/N/event    event lines, routed to a pipe (vios_handles.c)
 *   /dev/ui/tree        Forth source that rebuilds the tree (UI-SNAPSHOT)
 * N is a widget id (0..63) or w and a window id (w0..w26). */
#include "vios/win.h"
#include "vios/ui.h"
#include "vios/runtime.h"

static char error_text[28];
static unsigned char last_made = VIOS_WIN_NONE;

const char *vios_win_error(void) { return error_text; }
static void note(const unsigned char *s, unsigned n) {
    if (n > sizeof error_text - 1) n = sizeof error_text - 1;
    for (unsigned i = 0; i < n; ++i) error_text[i] = (char)s[i];
    error_text[n] = 0;
}
static int fail(const char *message) { note((const unsigned char *)message, (unsigned)vios_length(message)); return -1; }

int vios_win_path(const char *d, unsigned char *file, unsigned char *node) {
    unsigned n = 0, k = 0, window;
    *node = VIOS_WIN_NONE;
    if (vios_same(d, "win")) { *file = VIOS_WIN_LIST; return 1; }
    if (vios_same(d, "win/new")) { *file = VIOS_WIN_NEW; return 1; }
    if (vios_same(d, "ui/tree")) { *file = VIOS_WIN_TREE; return 1; }
    if (d[0] != 'w' || d[1] != 'i' || d[2] != 'n' || d[3] != '/') return 0;
    d += 4;
    window = *d == 'w';
    for (d += window; d[k] >= '0' && d[k] <= '9' && k < 3; ++k) n = n * 10 + (unsigned)(d[k] - '0');
    if (!k || d[k] != '/' || n >= (window ? VIOS_UI_WINDOWS : VIOS_UI_WIDGETS)) return 0;
    d += k + 1;
    if (vios_same(d, "ctl")) *file = VIOS_WIN_CTL;
    else if (vios_same(d, "text")) *file = VIOS_WIN_TEXT;
    else if (vios_same(d, "event")) *file = VIOS_WIN_EVENT;
    else return 0;
    *node = (unsigned char)(window ? VIOS_WIN_WINDOW | n : n);
    return 1;
}

/* Reply 9C status step(u16) sub(u16) n ch*n for the cursor; -1 with the reason noted. */
static int chunk(const struct vios_win_cursor *c, unsigned char *r) {
    unsigned char o[8] = {0x9C, c->file, c->node, (unsigned char)c->step, (unsigned char)(c->step >> 8),
                          (unsigned char)c->sub, (unsigned char)(c->sub >> 8), c->col};
    unsigned n = 0;
    if (!vios_ui_request(o, sizeof o, r, 32, &n) || n < 7 || 7u + r[6] > n) return fail("display server did not answer");
    if (r[1] == 3) { note(r + 7, r[6]); return -1; }
    return r[1];
}

int vios_win_open(struct vios_win_cursor *c) {
    unsigned char r[32];
    c->step = c->sub = 0;
    c->written = c->col = 0;
    if (c->file == VIOS_WIN_NEW) c->node = last_made;
    return c->node == VIOS_WIN_NONE || c->file == VIOS_WIN_NEW || chunk(c, r) >= 0;
}

int vios_win_read(struct vios_win_cursor *c, unsigned char *out, unsigned want) {
    unsigned char r[32];
    unsigned done = 0;
    while (done < want) {
        int status = chunk(c, r);
        if (status < 0) return done ? (int)done : -1;
        if (status == 2) break;                             /* end of file */
        unsigned k = r[6] < want - done ? r[6] : want - done;
        for (unsigned i = 0; i < k; ++i) out[done++] = r[7 + i];
        c->col = (unsigned char)(c->col + k);
        if (k == r[6] && status == 1) {                     /* the line is done: on to the next */
            c->step = (unsigned short)(r[2] | r[3] << 8);
            c->sub = (unsigned short)(r[4] | r[5] << 8);
            c->col = 0;
        }
    }
    return (int)done;
}

int vios_win_write(struct vios_win_cursor *c, fcell addr, fcell len) {
    unsigned char o[5 + 200 + 1], r[32];
    fcell done = 0;
    do {
        unsigned k = len - done > 200 ? 200u : (unsigned)(len - done), n = 0;
        for (unsigned i = 0; i < k; ++i) o[5 + i] = fvm_read8(addr + done + i);
        if (fvm_error) return fail("invalid memory") + 1;
        if (done + k < len)                                 /* whole lines per chunk where possible */
            for (unsigned j = k; j > 1; --j) if (o[5 + j - 1] == '\n') { k = j; break; }
        o[0] = 0x9D; o[1] = c->file; o[2] = c->node; o[3] = (unsigned char)(c->written || done); o[4] = (unsigned char)k;
        o[5 + k] = 0x30;                                    /* commit: show it at once */
        if (!vios_ui_request(o, 6 + k, r, sizeof r, &n) || n < 4 || 4u + r[3] > n) return fail("display server did not answer") + 1;
        if (!r[1]) { note(r + 4, r[3]); return 0; }
        c->node = r[2];
        if (c->file == VIOS_WIN_NEW) last_made = r[2];
        done += k;
    } while (done < len);
    c->written = 1;
    return 1;
}

void vios_win_route(unsigned node, int on) {
    const unsigned char o[3] = {0x9E, (unsigned char)node, (unsigned char)(on != 0)};
    vios_ui_op(o, sizeof o);
}

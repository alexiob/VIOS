/* Host test of the tool widgets (lib/vios-vds/src/vds_text.c, vds_widget.c):
 * TextView editing, selection, clipboard, evaluation events and read-back;
 * tabs; tables, tree and pager lists; large list pools; the key policy and
 * the event nudge; wire ops; random traffic checked against full renders.
 * Usage: vds_text FONT.bin [DUMP_DIR] */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vds_ui.h"

#define W 640
#define H 480
static uint8_t fb_a[H][W / 2], fb_b[H][W / 2], *rows_a[H], *rows_b[H];
static uint8_t font[256 * 8];
static vds_cell cells[VDS_MAX_SURFACES][80 * 60];
static vds v;
static vds_proto p;
static vds_ui u;

static vds_cell *cell_mem(void *ctx, uint8_t id, uint16_t n) { (void)ctx; return id < VDS_MAX_SURFACES && n <= 80 * 60 ? cells[id] : 0; }
static uint8_t last_reply[32], reply_n;
static unsigned nudges;
static void send(void *ctx, uint8_t code, const uint8_t *d, uint8_t n) {
    (void)ctx;
    if (code == VDS_PACKET_EVENT) { assert(n == 1); ++nudges; }
    else { memcpy(last_reply, d, n); reply_n = n; }
}
static void finish(void) { for (int i = 0; i < 1000; ++i) if (vds_frame(&v, UINT32_MAX)) return; assert(0); }
static void matches_full_render(void) {
    finish();
    v.rows = rows_b; vds_damage_all(&v); finish(); v.rows = rows_a;
    for (int y = 0; y < H; ++y) if (memcmp(fb_a[y], fb_b[y], W / 2)) { fprintf(stderr, "stale row %d\n", y); abort(); }
}
static int key(uint8_t ascii, uint8_t mods, uint8_t vk) {
    int a = vds_ui_key(&u, ascii, mods, vk, 1), b = vds_ui_key(&u, ascii, mods, vk, 0);
    assert(a == b);
    return a;
}
/* FabGL VK_a is 25; Ctrl+letter carries the control code as ASCII. */
static int ctrl(char c) { return key((uint8_t)(c - 'a' + 1), VDS_MOD_CTRL, (uint8_t)(25 + c - 'a')); }
static void type(const char *s) { for (; *s; ++s) assert(key((uint8_t)*s, 0, 0)); }
static void batch(const uint8_t *b, size_t n) { assert(vds_proto_batch(&p, b, n)); }

/* Pulled events: the next one of `kind` (others skipped); 0 if none. */
static uint8_t evbuf[32];
static const uint8_t *next_event(uint8_t kind) {
    for (;;) {
        uint8_t poll[] = {0x36, 1};
        batch(poll, sizeof poll);
        if (last_reply[0] != 0x36 || !last_reply[2]) return 0;
        memcpy(evbuf, last_reply + 4, last_reply[3]);
        if (evbuf[1] == kind) return evbuf;
    }
}
static void drain(void) { while (next_event(0xFF)) {} }
static uint16_t u16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }
static void text_at(uint8_t s, int col, int row, int n, char *out) {
    for (int i = 0; i < n; ++i) out[i] = (char)v.surfaces[s].cells[row * v.surfaces[s].cols + col + i].ch;
    out[n] = 0;
}
static uint8_t attr_at(uint8_t s, int col, int row) { return v.surfaces[s].cells[row * v.surfaces[s].cols + col].attr; }
/* Read a TextView back through op 8C, as the CPU does. */
static size_t read_back(uint8_t id, char *out, size_t max) {
    size_t n = 0;
    for (;;) {
        uint8_t op[] = {0x8C, id, (uint8_t)n, (uint8_t)(n >> 8), 25};
        batch(op, sizeof op);
        assert(last_reply[0] == 0x8C && u16(last_reply + 2) == n);
        if (!last_reply[4]) return n;
        assert(n + last_reply[4] <= max);
        memcpy(out + n, last_reply + 5, last_reply[4]);
        n += last_reply[4];
    }
}
static unsigned seed = 77;
static unsigned rnd(unsigned n) { seed = seed * 1103515245u + 12345u; return (seed >> 16) % n; }

enum { ROOT = 1, TABS, TABLE, EDIT, VIEW, TREE, PAGER };

int main(int argc, char **argv) {
    assert(argc >= 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f && fread(font, 1, sizeof font, f) == sizeof font);
    fclose(f);
    for (int y = 0; y < H; ++y) { rows_a[y] = fb_a[y]; rows_b[y] = fb_b[y]; }
    assert(vds_init(&v, rows_a, W, H, font));
    vds_proto_init(&p, &v, cell_mem, send, 0);
    vds_ui_init(&u, &p);
    uint8_t ink = (uint8_t)(v.style.title_fg | v.style.body << 4);
    assert(vds_surface_init(&v, 0, 60, 30, cells[0], ink));
    assert(vds_window_open(&v, 1, 0, 40, 40, 60, 30, VDS_W_VISIBLE | VDS_W_DECOR, "Tools"));
    assert(vds_ui_focus(&u, 1));
    const vds_widget *w = u.widgets;
    const vds_style *st = &v.style;
    uint8_t sel = (uint8_t)(st->menu_sel_fg | st->menu_sel_bg << 4);
    char t[128], back[VDS_TEXT_MAX + 1];

    /* Layout: tabs over a table, an editor, a read-only view. */
    assert(vds_ui_widget(&u, ROOT, 1, VDS_WK_BOX, VDS_NONE, 0, 0, 0, 0, VDS_WF_FILL) && vds_ui_widget_layout(&u, ROOT, VDS_LAYOUT_COLUMN, 0));
    assert(vds_ui_widget(&u, TABS, 1, VDS_WK_TABS, ROOT, 0, 0, 0, 0, 0) && vds_ui_widget_size(&u, TABS, 1, 0));
    assert(vds_ui_widget_text(&u, TABS, "Stack|Words|Tasks", 17));
    assert(vds_ui_widget(&u, TABLE, 1, VDS_WK_LIST, ROOT, 0, 0, 0, 0, 0) && vds_ui_widget_size(&u, TABLE, 8, 0));
    assert(vds_ui_widget(&u, EDIT, 1, VDS_WK_TEXT, ROOT, 0, 0, 0, 0, 0) && vds_ui_widget_size(&u, EDIT, 0, 2));
    assert(vds_ui_widget(&u, VIEW, 1, VDS_WK_TEXT, ROOT, 0, 0, 0, 0, VDS_WF_READONLY) && vds_ui_widget_size(&u, VIEW, 0, 1));
    assert(w[TABS].row == 0 && w[TABLE].row == 1 && w[TABLE].h == 8 && w[EDIT].row == 9 && w[EDIT].h == 14 && w[VIEW].h == 7);
    assert(u.widget_focus[1] == TABS);
    text_at(0, 0, 0, 22, t);
    assert(!strcmp(t, " Stack   Words   Tasks") && attr_at(0, 1, 0) == sel);

    /* Tabs: arrows when focused, Ctrl+Tab from anywhere; SELECT events. */
    assert(key(0, 0, VDS_VK_RIGHT) && w[TABS].value == 1 && attr_at(0, 9, 0) == sel);
    const uint8_t *e = next_event(VDS_EVENT_SELECT);
    assert(e && e[2] == TABS && u16(e + 3) == 1 && e[5] == 1);
    assert(key(0, 0, VDS_VK_RIGHT) && key(0, 0, VDS_VK_RIGHT) && w[TABS].value == 2);   /* stops at the last */
    assert(key(0, 0, VDS_VK_TAB) && u.widget_focus[1] == TABLE);
    assert(key(9, VDS_MOD_CTRL, VDS_VK_TAB) && w[TABS].value == 0 && u.widget_focus[1] == TABLE);   /* wraps */
    assert(key(9, VDS_MOD_CTRL | VDS_MOD_SHIFT, VDS_VK_TAB) && w[TABS].value == 2);
    drain();

    /* Table: header row, '\t' cells clipped to the column widths. */
    uint8_t cols[] = {6, 10, 8};
    assert(vds_ui_widget_columns(&u, TABLE, cols, 3) && vds_ui_widget_text(&u, TABLE, "ID\tNAME\tSTATE", 13));
    assert(vds_ui_widget_append(&u, TABLE, "1\tAUTORUN\trunning", 17) && vds_ui_widget_append(&u, TABLE, "2\tA-VERY-LONG-NAME\tx", 20));
    text_at(0, 0, 1, 25, t);
    assert(!strcmp(t, " ID    NAME      STATE   "));
    text_at(0, 0, 3, 25, t);
    assert(!strcmp(t, " 2     A-VERY-LO x       "));
    assert(attr_at(0, 3, 2) == sel);                         /* the focused selection */
    for (int i = 0; i < 30; ++i) { snprintf(t, sizeof t, "%d\tW%d\t-", i + 3, i); assert(vds_ui_widget_append(&u, TABLE, t, strlen(t))); }
    assert(w[TABLE].count == 32);
    assert(key(0, 0, VDS_VK_PAGEDOWN) && w[TABLE].value == 7 && w[TABLE].top == 1);   /* 7 rows under the header */
    assert(key(0, 0, VDS_VK_END) && w[TABLE].value == 31 && w[TABLE].top == 25);
    text_at(0, 0, 8, 9, t);
    assert(!strcmp(t, " 32    W2"));
    assert(key('3', 0, 0) && w[TABLE].value == 2);          /* type-ahead on the first cell */
    matches_full_render();

    /* A big pool: 600 rows in one list, u16 ops on the wire. */
    assert(vds_ui_widget_count(&u, TABLE, 600) && w[TABLE].count == 600 && u.items_used >= 600);
    uint8_t item[] = {0x90, TABLE, 0x57, 0x02, 4, 'L', 'A', 'S', 'T'};    /* row 599 */
    batch(item, sizeof item);
    assert(!strcmp(u.items[w[TABLE].first + 599], "LAST"));
    uint8_t count[] = {0x91, TABLE, 0x58, 0x02};                           /* 600 again: no change */
    batch(count, sizeof count);
    uint8_t append[] = {0x92, TABLE, 2, 1, 'a', 2, 'b', 'c'};
    batch(append, sizeof append);
    assert(w[TABLE].count == 602 && !strcmp(u.items[w[TABLE].first + 601], "bc") && !p.errors);
    uint8_t bad[] = {0x92, TABLE, 2, 1, 'a', 9, 'b'};                      /* truncated: dropped whole */
    assert(!vds_proto_batch(&p, bad, sizeof bad) && w[TABLE].count == 602);
    assert(vds_ui_widget_count(&u, TABLE, 0) && vds_ui_widget_columns(&u, TABLE, cols, 0));
    drain();
    p.errors = p.desync = 0;

    /* TextView: typing, lines, caret, selection, clipboard. */
    assert(vds_ui_widget_focus(&u, EDIT));
    assert(u.texts[w[EDIT].slot].used && u.texts[w[VIEW].slot].used && w[EDIT].slot != w[VIEW].slot);
    type(": SQ DUP * ;");
    assert(key(0, 0, VDS_VK_RETURN));
    type("3 SQ .");
    const vds_text *x = vds_ui_text(&u, EDIT);
    assert(x->len == 19 && x->caret == 19 && x->dirty && !memcmp(x->buf, ": SQ DUP * ;\n3 SQ .", 19));
    e = next_event(VDS_EVENT_CHANGE);                        /* once, when it became dirty */
    assert(e && e[2] == EDIT && !next_event(VDS_EVENT_CHANGE));
    text_at(0, 0, 10, 8, t);
    assert(!strcmp(t, "3 SQ .  ") && v.surfaces[0].cur_row == 10 && v.surfaces[0].cur_col == 6);
    assert(key(0, 0, VDS_VK_UP) && x->caret == 6);         /* same column */
    assert(key(0, 0, VDS_VK_END) && x->caret == 12 && key(0, 0, VDS_VK_DOWN) && x->caret == 19);   /* clamped */
    assert(key(0, VDS_MOD_SHIFT, VDS_VK_HOME) && x->anchor == 19 && x->caret == 13 && attr_at(0, 0, 10) == sel);
    assert(ctrl('c') && u.clip_len == 6 && !memcmp(u.clip, "3 SQ .", 6));
    assert(key(0, VDS_MOD_CTRL, VDS_VK_END) && ctrl('v') && x->len == 25 && !memcmp(x->buf + 19, "3 SQ .", 6));
    assert(key(0, VDS_MOD_CTRL, VDS_VK_HOME) && x->caret == 0 && x->anchor == 0);
    assert(key(0, VDS_MOD_SHIFT, VDS_VK_RIGHT) && key(0, VDS_MOD_SHIFT, VDS_VK_RIGHT) && ctrl('x') && x->len == 23 && x->buf[0] == 'S');
    assert(key(0, 0, VDS_VK_DELETE) && x->buf[0] == 'Q' && key(0, 0, VDS_VK_BACKSPACE) && x->len == 22);   /* nothing before */
    assert(ctrl('a') && x->anchor == 0 && x->caret == x->len && key('z', 0, 0) && x->len == 1 && x->buf[0] == 'z');
    matches_full_render();

    /* Evaluation keys: the selection, else the caret's line (then selected). */
    assert(vds_ui_text_set(&u, EDIT, 0, "1 2 +\r\n3 4 *\n", 13) && x->len == 12 && !x->dirty);   /* '\r' dropped */
    assert(vds_ui_text_select(&u, EDIT, 8, 8));
    drain();
    assert(ctrl('d') && x->anchor == 6 && x->caret == 11);
    e = next_event(VDS_EVENT_TEXT);
    assert(e && e[2] == EDIT && e[3] == VDS_TEXT_DO && u16(e + 4) == 6 && u16(e + 6) == 11 && e[8] == 1);
    assert(vds_ui_text_select(&u, EDIT, 0, 3) && ctrl('p'));
    e = next_event(VDS_EVENT_TEXT);
    assert(e && e[3] == VDS_TEXT_PRINT && u16(e + 4) == 0 && u16(e + 6) == 3);
    assert(ctrl('i') && (e = next_event(VDS_EVENT_TEXT)) && e[3] == VDS_TEXT_INSPECT);
    assert(ctrl('s') && (e = next_event(VDS_EVENT_TEXT)) && e[3] == VDS_TEXT_ACCEPT);
    /* Print it: the CPU inserts the result after the selection, selected. */
    assert(vds_ui_text_select(&u, EDIT, 5, 5) && vds_ui_text_insert(&u, EDIT, " 3", 2));
    assert(!memcmp(x->buf, "1 2 + 3\n", 8) && x->anchor == 5 && x->caret == 7 && !x->dirty);
    size_t n = read_back(EDIT, back, sizeof back);
    assert(n == x->len && !memcmp(back, x->buf, n));
    uint8_t info[] = {0x8E, EDIT};
    batch(info, sizeof info);
    assert(last_reply[0] == 0x8E && u16(last_reply + 2) == x->len && u16(last_reply + 4) == 7 && u16(last_reply + 6) == 5 &&
           u16(last_reply + 8) == x->rev && last_reply[10] == 0);
    assert(vds_ui_widget_value(&u, EDIT, 1) && x->dirty && vds_ui_widget_value(&u, EDIT, 0) && !x->dirty);

    /* Long text: wire chunks, scrolling, the limit, exact read-back. */
    uint8_t clear[] = {0x8A, EDIT, 0, 0};
    batch(clear, sizeof clear);
    assert(x->len == 0);
    char line[64];
    size_t want = 0;
    for (int i = 0; i < 90; ++i) {
        int k = snprintf(line, sizeof line, ": W%03d %d DUP * ; \\ line %d of the module\n", i, i, i);
        uint8_t op[4 + 64] = {0x8A, EDIT, 1, (uint8_t)k};
        memcpy(op + 4, line, (size_t)k);
        batch(op, 4u + (unsigned)k);
        want += (size_t)k;
    }
    assert(x->len == want && want > 3700 && want <= VDS_TEXT_MAX && !p.errors);
    n = read_back(EDIT, back, sizeof back);
    assert(n == want && !memcmp(back, x->buf, n));
    assert(vds_ui_widget_focus(&u, EDIT) && key(0, VDS_MOD_CTRL, VDS_VK_END));
    unsigned top_line = 0;
    for (unsigned i = 0; i < x->top; ++i) top_line += x->buf[i] == '\n';
    assert(top_line == 90 - 13);                             /* last line (empty) at the bottom row */
    for (int i = 0; i < 400; ++i) key('x', 0, 0);            /* the buffer fills, then refuses */
    assert(x->len == VDS_TEXT_MAX);
    assert(key(0, 0, VDS_VK_END) && x->left > 0);           /* horizontal scroll keeps the caret visible */
    matches_full_render();
    assert(key(0, VDS_MOD_CTRL, VDS_VK_HOME) && x->top == 0 && x->left == 0);
    assert(key(0, 0, VDS_VK_PAGEDOWN) && x->caret == vds_ui_text(&u, EDIT)->caret);
    matches_full_render();

    /* Read-only: moves and selects, never edits, still evaluates. */
    assert(vds_ui_text_set(&u, VIEW, 0, "SEE SQ\n: SQ DUP * ;", 19) && vds_ui_widget_focus(&u, VIEW));
    const vds_text *ro = vds_ui_text(&u, VIEW);
    assert(key('a', 0, 0) == 0 && ro->len == 19 && ctrl('a') && ctrl('x') && ro->len == 19 && ctrl('v') && ro->len == 19);
    assert(ctrl('c') && u.clip_len == 19 && key(0, 0, VDS_VK_DOWN));
    drain();
    assert(ctrl('d') && (e = next_event(VDS_EVENT_TEXT)) && e[2] == VIEW);

    /* Tree and pager lists. */
    assert(vds_ui_widget_delete(&u, VIEW) && !u.texts[0].used + !u.texts[1].used == 1);
    assert(vds_ui_widget(&u, TREE, 1, VDS_WK_LIST, ROOT, 0, 0, 0, 0, VDS_WF_TREE) && vds_ui_widget_size(&u, TREE, 4, 0));
    assert(vds_ui_widget_append(&u, TREE, "+ DEMO", 6) && vds_ui_widget_append(&u, TREE, "- WBM1P", 7) && vds_ui_widget_append(&u, TREE, "    SQUARE", 10));
    assert(vds_ui_widget_focus(&u, TREE) && key('s', 0, 0) && w[TREE].value == 2);   /* type-ahead past the indent */
    assert(key(0, 0, VDS_VK_UP) && key(0, 0, VDS_VK_LEFT));
    e = next_event(VDS_EVENT_EXPAND);
    assert(e && e[2] == TREE && u16(e + 3) == 1 && e[5] == 1 && e[6] == 0);
    assert(key(0, 0, VDS_VK_RIGHT) && (e = next_event(VDS_EVENT_EXPAND)) && e[6] == 1);
    assert(vds_ui_widget(&u, PAGER, 1, VDS_WK_LIST, ROOT, 0, 0, 0, 0, VDS_WF_PAGER) && vds_ui_widget_size(&u, PAGER, 3, 0));
    for (int i = 0; i < 3; ++i) assert(vds_ui_widget_append(&u, PAGER, "040000: 00", 10));
    assert(vds_ui_widget_focus(&u, PAGER));
    drain();
    assert(key(0, 0, VDS_VK_UP) && (e = next_event(VDS_EVENT_PAGE)) && e[2] == PAGER && (int8_t)e[3] == -1);
    assert(key(0, 0, VDS_VK_PAGEUP) && (e = next_event(VDS_EVENT_PAGE)) && (int8_t)e[3] == -2);
    assert(key(0, 0, VDS_VK_DOWN) && w[PAGER].value == 1 && !next_event(VDS_EVENT_PAGE));   /* inside: moves */
    assert(key(0, 0, VDS_VK_PAGEDOWN) && w[PAGER].value == 2 && key(0, 0, VDS_VK_PAGEDOWN) &&
           (e = next_event(VDS_EVENT_PAGE)) && (int8_t)e[3] == 2);
    matches_full_render();

    /* TextView slots are bounded and released with their widget or window. */
    assert(vds_ui_widget(&u, 20, 1, VDS_WK_TEXT, VDS_NONE, 0, 0, 10, 2, 0) && vds_ui_widget(&u, 21, 1, VDS_WK_TEXT, VDS_NONE, 0, 0, 10, 2, 0));
    assert(vds_ui_widget(&u, 22, 1, VDS_WK_TEXT, VDS_NONE, 0, 0, 10, 2, 0));
    assert(!vds_ui_widget(&u, 23, 1, VDS_WK_TEXT, VDS_NONE, 0, 0, 10, 2, 0) && !w[23].used);   /* 4 in use */
    assert(vds_ui_widget(&u, 22, 1, VDS_WK_LABEL, VDS_NONE, 0, 0, 10, 1, 0) && vds_ui_widget(&u, 23, 1, VDS_WK_TEXT, VDS_NONE, 0, 0, 10, 2, 0));

    /* Key policy: a widget window keeps unused keys from the CPU. */
    assert(vds_ui_widget_focus(&u, TABS));
    assert(!key(0, 0, VDS_VK_F3) && !key('q', 0, 0));
    uint8_t policy[] = {0x5A, 1, VDS_KEYS_DROP};
    batch(policy, sizeof policy);
    assert(key(0, 0, VDS_VK_F3) && key('q', 0, 0) && !key(0, 0, VDS_VK_F1));   /* F1 still goes through */
    uint8_t refused[] = {0x5A, 30, 1};                                          /* a server window */
    batch(refused, sizeof refused);
    assert(p.errors == 1);
    p.errors = 0;

    /* Nudge: one packet per batch of queued events, re-armed by a poll. */
    drain();
    uint8_t nudge_on[] = {0x74, 3, 1, 0};
    batch(nudge_on, sizeof nudge_on);
    unsigned before = nudges;
    assert(key(0, 0, VDS_VK_RIGHT) && key(0, 0, VDS_VK_LEFT) && nudges == before + 1);
    assert(next_event(VDS_EVENT_SELECT) && key(0, 0, VDS_VK_RIGHT) && nudges == before + 2);
    uint8_t nudge_off[] = {0x74, 3, 0, 0};
    batch(nudge_off, sizeof nudge_off);
    drain();
    assert(key(0, 0, VDS_VK_LEFT) && nudges == before + 2);

    /* Closing the window releases everything it had. */
    assert(vds_window_close(&v, 1));
    vds_ui_forget(&u, 1);
    for (unsigned i = 0; i < VDS_TEXT_SLOTS; ++i) assert(!u.texts[i].used);
    assert(!u.items_used);

    /* Random traffic over the tool widgets; the screen stays exact. */
    assert(vds_window_open(&v, 1, 0, 40, 40, 60, 30, VDS_W_VISIBLE | VDS_W_DECOR, "Tools") && vds_ui_focus(&u, 1));
    static const uint8_t vks[] = {VDS_VK_TAB, VDS_VK_UP, VDS_VK_DOWN, VDS_VK_LEFT, VDS_VK_RIGHT, VDS_VK_HOME, VDS_VK_END,
                                  VDS_VK_PAGEUP, VDS_VK_PAGEDOWN, VDS_VK_RETURN, VDS_VK_BACKSPACE, VDS_VK_DELETE, 0, 0, 0, 0};
    for (int step = 0; step < 8000; ++step) {
        uint8_t id = (uint8_t)rnd(12);
        switch (rnd(12)) {
        case 0: vds_ui_widget(&u, id, 1, (uint8_t)(rnd(3) ? VDS_WK_TEXT + rnd(2) : VDS_WK_LIST), rnd(3) ? VDS_NONE : (uint8_t)rnd(12),
                              (uint8_t)rnd(40), (uint8_t)rnd(25), (uint8_t)rnd(40), (uint8_t)rnd(12), (uint8_t)rnd(64)); break;
        case 1: vds_ui_widget_text(&u, id, "One|Two|Three\tx", rnd(16)); break;
        case 2: { static const char sample[] = "abc def\nghi\r\n\n jkl mno pqr stu vwx yz 0123456789 abcdefghij\n";
                  vds_ui_text_set(&u, id, (int)rnd(2), sample, rnd(sizeof sample)); break; }
        case 3: vds_ui_text_select(&u, id, (uint16_t)rnd(300), (uint16_t)rnd(300)); break;
        case 4: vds_ui_text_insert(&u, id, "inserted\ntext", rnd(14)); break;
        case 5: { uint8_t c[3] = {(uint8_t)rnd(12), (uint8_t)rnd(12), (uint8_t)rnd(12)}; vds_ui_widget_columns(&u, id, c, (uint8_t)rnd(4)); break; }
        case 6: vds_ui_widget_append(&u, id, "a\tbb\tccc", rnd(9)); break;
        case 7: if (!rnd(5)) vds_ui_widget_delete(&u, id); else vds_ui_widget_focus(&u, id); break;
        case 8: vds_window_resize(&v, 1, (uint8_t)(1 + rnd(60)), (uint8_t)(1 + rnd(30))); { uint8_t b[] = {0x30}; batch(b, 1); } break;
        default: {
            uint8_t mods = (uint8_t)(rnd(3) ? 0 : rnd(2) ? VDS_MOD_SHIFT : VDS_MOD_CTRL);
            uint8_t ascii = (uint8_t)(rnd(3) ? 0 : mods == VDS_MOD_CTRL ? 1 + rnd(26) : 'a' + rnd(26));
            key(ascii, mods, vks[rnd(sizeof vks)]);
        }
        }
        for (unsigned i = 0; i < VDS_TEXT_SLOTS; ++i) {
            const vds_text *s = &u.texts[i];
            if (!s->used) continue;
            assert(w[s->widget].used && w[s->widget].kind == VDS_WK_TEXT && w[s->widget].slot == i);
            assert(s->len <= VDS_TEXT_MAX && s->caret <= s->len && s->anchor <= s->len && s->top <= s->len && !memchr(s->buf, '\r', s->len));
        }
        if (step % 50 == 0) drain();
        if (step % 400 == 399) matches_full_render();
    }
    matches_full_render();
    if (argc > 2) {
        char path[512];
        snprintf(path, sizeof path, "%s/vds-text.txt", argv[2]);
        FILE *o = fopen(path, "w");
        if (o) { fprintf(o, "ok\n"); fclose(o); }
    }
    printf("vds text widget checks passed\n");
    return 0;
}

/* Host test of VDS widgets (lib/vios-vds/src/vds_widget.c, G4): box layout,
 * focus order, buttons, checks, radio groups, input editing, lists, progress,
 * pointer clicks, the list pool, wire ops, and random traffic checked against
 * full renders. Usage: vds_widget FONT.bin [DUMP_DIR] */
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
static uint8_t ev[256][32], last_reply[32];
static unsigned ev_n;
static void send(void *ctx, uint8_t code, const uint8_t *d, uint8_t n) {
    (void)ctx;
    if (code == VDS_PACKET_EVENT) { memset(ev[ev_n & 255], 0, 32); memcpy(ev[ev_n & 255], d, n); ++ev_n; }
    else memcpy(last_reply, d, n);
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
static unsigned mark;
static const uint8_t *event_since(uint8_t kind) {
    for (unsigned i = ev_n; i > mark; --i) if (ev[(i - 1) & 255][1] == kind) return ev[(i - 1) & 255];
    return 0;
}
static int16_t s16(const uint8_t *b) { return (int16_t)(uint16_t)(b[0] | b[1] << 8); }
static void text_at(uint8_t s, int col, int row, int n, char *out) {
    for (int i = 0; i < n; ++i) out[i] = (char)v.surfaces[s].cells[row * v.surfaces[s].cols + col + i].ch;
    out[n] = 0;
}
static uint8_t attr_at(uint8_t s, int col, int row) { return v.surfaces[s].cells[row * v.surfaces[s].cols + col].attr; }
static unsigned seed = 31;
static unsigned rnd(unsigned n) { seed = seed * 1103515245u + 12345u; return (seed >> 16) % n; }
static void dump(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    assert(f);
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t i = (x & 1) ? fb_a[y][x >> 1] & 15 : fb_a[y][x >> 1] >> 4, c = v.style.palette[i];
            uint8_t rgb[3] = {(uint8_t)((c >> 4 & 3) * 85), (uint8_t)((c >> 2 & 3) * 85), (uint8_t)((c & 3) * 85)};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

enum { ROOT, TITLE, NAME, BUTTONS, OK, CANCEL, LIST, OPTIONS, CHECK, RADIO_A, RADIO_B, BAR, NOTE };

int main(int argc, char **argv) {
    assert(argc >= 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f && fread(font, 1, sizeof font, f) == sizeof font);
    fclose(f);
    for (int y = 0; y < H; ++y) { rows_a[y] = fb_a[y]; rows_b[y] = fb_b[y]; }
    assert(vds_init(&v, rows_a, W, H, font));
    vds_proto_init(&p, &v, cell_mem, send, 0);
    p.push_events = 1;
    vds_ui_init(&u, &p);
    uint8_t ink = (uint8_t)(v.style.title_fg | v.style.body << 4);
    assert(vds_surface_init(&v, 0, 60, 30, cells[0], ink));
    assert(vds_window_open(&v, 1, 0, 96, 40, 40, 20, VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW, "Open Module"));
    assert(vds_ui_focus(&u, 1));

    /* A dialog laid out by nested boxes. */
    assert(vds_ui_widget(&u, ROOT, 1, VDS_WK_BOX, VDS_NONE, 0, 0, 0, 0, VDS_WF_FILL) && vds_ui_widget_layout(&u, ROOT, VDS_LAYOUT_COLUMN, 1));
    assert(vds_ui_widget(&u, TITLE, 1, VDS_WK_LABEL, ROOT, 0, 0, 0, 0, 0) && vds_ui_widget_size(&u, TITLE, 1, 0));
    assert(vds_ui_widget_text(&u, TITLE, "Module &name:", 13));
    assert(vds_ui_widget(&u, NAME, 1, VDS_WK_INPUT, ROOT, 0, 0, 0, 0, 0) && vds_ui_widget_size(&u, NAME, 1, 0));
    assert(vds_ui_widget(&u, LIST, 1, VDS_WK_LIST, ROOT, 0, 0, 0, 0, 0) && vds_ui_widget_size(&u, LIST, 0, 1));
    assert(vds_ui_widget(&u, OPTIONS, 1, VDS_WK_BOX, ROOT, 0, 0, 0, 0, 0) && vds_ui_widget_size(&u, OPTIONS, 1, 0));
    assert(vds_ui_widget_layout(&u, OPTIONS, VDS_LAYOUT_ROW, 1));
    assert(vds_ui_widget(&u, CHECK, 1, VDS_WK_CHECK, OPTIONS, 0, 0, 0, 0, 0) && vds_ui_widget_text(&u, CHECK, "&Pinned", 7));
    assert(vds_ui_widget(&u, RADIO_A, 1, VDS_WK_RADIO, OPTIONS, 0, 0, 0, 0, 0) && vds_ui_widget_text(&u, RADIO_A, "DIRECT", 6));
    assert(vds_ui_widget(&u, RADIO_B, 1, VDS_WK_RADIO, OPTIONS, 0, 0, 0, 0, 0) && vds_ui_widget_text(&u, RADIO_B, "TOKEN", 5));
    assert(vds_ui_widget_value(&u, RADIO_A, 1));
    assert(vds_ui_widget(&u, BAR, 1, VDS_WK_PROGRESS, ROOT, 0, 0, 0, 0, 0) && vds_ui_widget_size(&u, BAR, 1, 0) && vds_ui_widget_value(&u, BAR, 42));
    assert(vds_ui_widget(&u, BUTTONS, 1, VDS_WK_BOX, ROOT, 0, 0, 0, 0, 0) && vds_ui_widget_size(&u, BUTTONS, 1, 0));
    assert(vds_ui_widget_layout(&u, BUTTONS, VDS_LAYOUT_ROW, 2));
    assert(vds_ui_widget(&u, OK, 1, VDS_WK_BUTTON, BUTTONS, 0, 0, 0, 0, 0) && vds_ui_widget_text(&u, OK, "&Open", 5));
    assert(vds_ui_widget(&u, CANCEL, 1, VDS_WK_BUTTON, BUTTONS, 0, 0, 0, 0, 0) && vds_ui_widget_text(&u, CANCEL, "Cancel", 6));
    assert(vds_ui_widget(&u, NOTE, 1, VDS_WK_BUTTON, BUTTONS, 0, 0, 0, 0, VDS_WF_DISABLED) && vds_ui_widget_text(&u, NOTE, "Help", 4));
    static const char *mods[] = {"LIVE1D", "LIVE1I", "LIVE1P", "WBM1P", "WORDS", "TABLES", "GRAPHICS", "SPRITES", "MENUS", "LISTS"};
    assert(vds_ui_widget_count(&u, LIST, 10));
    for (uint8_t i = 0; i < 10; ++i) assert(vds_ui_widget_item(&u, LIST, i, mods[i], strlen(mods[i])));
    /* Column of 20 rows, gap 1, six children: 1 + 1 + list + 1 + 1 + 1 + 5 gaps = 20, so the list gets 10. */
    const vds_widget *w = u.widgets;
    assert(w[ROOT].w == 40 && w[ROOT].h == 20);
    assert(w[TITLE].row == 0 && w[NAME].row == 2 && w[LIST].row == 4 && w[LIST].h == 10);
    assert(w[OPTIONS].row == 15 && w[BAR].row == 17 && w[BUTTONS].row == 19 && w[NAME].w == 40);
    assert(w[OK].col == 0 && w[OK].w == 12 && w[CANCEL].col == 14 && w[CANCEL].w == 12 && w[NOTE].col == 28 && w[NOTE].w == 12);
    char t[64];
    text_at(0, 0, 0, 12, t);
    assert(!strcmp(t, "Module name:"));
    text_at(0, 1, 4, 6, t);
    assert(!strcmp(t, "LIVE1D"));
    text_at(0, 18, 17, 3, t);                                               /* centred: (40 - 3) / 2 */
    assert(!strcmp(t, "42%"));
    assert(attr_at(0, 0, 17) == (v.style.menu_sel_fg | v.style.menu_sel_bg << 4) && attr_at(0, 39, 17) == ink);   /* 16 of 40 filled */
    text_at(0, w[OK].col + 2, 19, 8, t);
    assert(!strcmp(t, "[ Open ]"));
    text_at(0, 0, 19, 40, t);                                               /* gaps hold nothing stale */
    assert(!strcmp(t, "  [ Open ]     [ Cancel ]     [ Help ]  "));
    /* Pane dividers: the gap row between the list and the options box is a
     * one-pixel line, black on the white body; none next to the input or
     * between the buttons (they are not panes). */
    const vds_cell *sc = v.surfaces[0].cells;unsigned sw = v.surfaces[0].cols;
    for (unsigned x = 0; x < 40; ++x) assert(sc[14 * sw + x].ch == VDS_GLYPH_HLINE && sc[14 * sw + x].attr == (0 | v.style.body << 4));
    assert(sc[3 * sw].ch == ' ' && sc[19 * sw + 12].ch == ' ');
    assert(attr_at(0, 1, 4) == (v.style.menu_fg | v.style.desktop_a << 4));                /* unfocused selection shows */
    finish();
    if (argc > 2) dump(argv[2], "vds-widgets-smalltalk");
    matches_full_render();

    /* Focus order skips labels, progress bars and disabled buttons; Shift+Tab goes back. */
    assert(u.widget_focus[1] == NAME);
    static const uint8_t order[] = {LIST, CHECK, RADIO_A, RADIO_B, OK, CANCEL, NAME};
    for (unsigned i = 0; i < sizeof order; ++i) assert(key(9, 0, VDS_VK_TAB) && u.widget_focus[1] == order[i]);
    assert(key(9, VDS_MOD_SHIFT, VDS_VK_TAB) && u.widget_focus[1] == CANCEL);
    assert(attr_at(0, w[CANCEL].col + 4, 19) == (v.style.menu_sel_fg | v.style.menu_sel_bg << 4));             /* focus shows */

    /* Buttons: Space or Enter activate. */
    mark = ev_n;
    assert(key(' ', 0, VDS_VK_SPACE));
    const uint8_t *e = event_since(VDS_EVENT_ACTIVATE);
    assert(e && e[2] == CANCEL && e[3] == 1);
    mark = ev_n;
    assert(key(0, 0, VDS_VK_RETURN) && event_since(VDS_EVENT_ACTIVATE));

    /* Check box and radio group. */
    assert(vds_ui_widget_focus(&u, CHECK));
    mark = ev_n;
    assert(key(' ', 0, VDS_VK_SPACE) && w[CHECK].value == 1);
    e = event_since(VDS_EVENT_CHANGE);
    assert(e && e[2] == CHECK && s16(e + 3) == 1);
    text_at(0, w[CHECK].col, 15, 3, t);
    assert(!strcmp(t, "[x]"));
    assert(vds_ui_widget_focus(&u, RADIO_A) && key(0, 0, VDS_VK_DOWN));
    assert(u.widget_focus[1] == RADIO_B && w[RADIO_B].value == 1 && w[RADIO_A].value == 0);
    assert(key(0, 0, VDS_VK_UP) && w[RADIO_A].value == 1 && w[RADIO_B].value == 0);
    matches_full_render();

    /* Input: editing with the caret, events, read back over the wire, scrolling. */
    assert(vds_ui_widget_focus(&u, NAME));
    mark = ev_n;
    for (const char *s = "WBMP"; *s; ++s) assert(key((uint8_t)*s, 0, 0));
    assert(key(0, 0, VDS_VK_LEFT) && key('1', 0, 0));
    assert(!strcmp(w[NAME].text, "WBM1P") && w[NAME].value == 4);
    assert(key(0, 0, VDS_VK_HOME) && key(0, 0, VDS_VK_DELETE) && key(0, 0, VDS_VK_END) && key(0, 0, VDS_VK_BACKSPACE));
    assert(!strcmp(w[NAME].text, "BM1") && w[NAME].value == 3);
    e = event_since(VDS_EVENT_CHANGE);
    assert(e && e[2] == NAME && s16(e + 3) == 3);
    assert(v.surfaces[0].cur_shape == VDS_CURSOR_UNDERLINE && v.surfaces[0].cur_col == 3 && v.surfaces[0].cur_row == 2);
    uint8_t get[] = {0x89, NAME};
    assert(vds_proto_batch(&p, get, sizeof get));
    assert(last_reply[0] == 0x89 && last_reply[1] == NAME && last_reply[4] == 3 && !memcmp(last_reply + 5, "BM1", 3));
    for (int i = 0; i < 50; ++i) key((uint8_t)('a' + i % 26), 0, 0);
    assert(strlen(w[NAME].text) == VDS_WIDGET_TEXT - 1 && w[NAME].top == VDS_WIDGET_TEXT - 40);  /* full; scrolled in the 40-cell field */
    assert(vds_ui_widget(&u, 40, 1, VDS_WK_INPUT, VDS_NONE, 45, 6, 8, 1, 0));    /* a narrow field scrolls */
    assert(vds_ui_widget_text(&u, 40, "0123456789ABCDEFGHIJ", 20) && vds_ui_widget_focus(&u, 40));
    assert(key(0, 0, VDS_VK_END) && w[40].value == 20 && w[40].top == 13 && v.surfaces[0].cur_col == 45 + 7);
    text_at(0, 45, 6, 7, t);
    assert(!strcmp(t, "DEFGHIJ"));
    assert(key(0, 0, VDS_VK_HOME) && w[40].top == 0);
    assert(vds_ui_widget_delete(&u, 40) && vds_ui_widget_focus(&u, NAME));
    mark = ev_n;
    assert(key(0, 0, VDS_VK_RETURN) && event_since(VDS_EVENT_ACTIVATE)[2] == NAME);
    matches_full_render();
    assert(key(9, 0, VDS_VK_TAB) && v.surfaces[0].cur_shape == VDS_CURSOR_OFF);   /* caret leaves with focus */

    /* List: keys, paging, type-ahead, scrolling, Enter. */
    assert(u.widget_focus[1] == LIST);
    mark = ev_n;
    assert(key(0, 0, VDS_VK_DOWN) && w[LIST].value == 1);
    e = event_since(VDS_EVENT_SELECT);
    assert(e && e[2] == LIST && s16(e + 3) == 1);
    assert(key(0, 0, VDS_VK_END) && w[LIST].value == 9 && w[LIST].top == 0);
    assert(key(0, 0, VDS_VK_HOME) && key('w', 0, 0) && w[LIST].value == 3 && key('W', 0, 0) && w[LIST].value == 4);
    assert(key(0, 0, VDS_VK_PAGEDOWN) && w[LIST].value == 9);
    assert(vds_ui_widget_count(&u, LIST, 14));
    for (uint8_t i = 10; i < 14; ++i) assert(vds_ui_widget_item(&u, LIST, i, "EXTRA", 5));
    assert(key(0, 0, VDS_VK_END) && w[LIST].value == 13 && w[LIST].top == 4);  /* scrolled to show it */
    text_at(0, 1, 4 + 9, 5, t);
    assert(!strcmp(t, "EXTRA"));
    mark = ev_n;
    assert(key(0, 0, VDS_VK_RETURN) && event_since(VDS_EVENT_ACTIVATE)[2] == LIST);
    matches_full_render();

    /* The pointer: click a list row, a check box and a button. */
    mark = ev_n;
    vds_ui_pointer(&u, (int16_t)(96 + 3 * 8), (int16_t)(40 + 6 * 8 + 3), 1);
    vds_ui_pointer(&u, (int16_t)(96 + 3 * 8), (int16_t)(40 + 6 * 8 + 3), 0);
    assert(u.widget_focus[1] == LIST && w[LIST].value == w[LIST].top + 2);
    vds_ui_pointer(&u, (int16_t)(96 + 1 * 8), (int16_t)(40 + 15 * 8), 1);
    vds_ui_pointer(&u, (int16_t)(96 + 1 * 8), (int16_t)(40 + 15 * 8), 0);
    assert(u.widget_focus[1] == CHECK && w[CHECK].value == 0);
    vds_ui_pointer(&u, (int16_t)(96 + (w[OK].col + 3) * 8), (int16_t)(40 + 19 * 8), 1);
    vds_ui_pointer(&u, (int16_t)(96 + (w[OK].col + 3) * 8), (int16_t)(40 + 19 * 8), 0);
    e = event_since(VDS_EVENT_ACTIVATE);
    assert(e && e[2] == OK);
    vds_ui_pointer(&u, (int16_t)(96 + (w[NOTE].col + 3) * 8), (int16_t)(40 + 19 * 8), 1);   /* disabled: nothing */
    vds_ui_pointer(&u, (int16_t)(96 + (w[NOTE].col + 3) * 8), (int16_t)(40 + 19 * 8), 0);
    assert(u.widget_focus[1] == OK);
    vds_sprite_show(&v, VDS_CURSOR_SPRITE, 0);
    matches_full_render();

    /* Resizing the window re-lays out the widgets (keyboard move/resize mode). */
    assert(key(0, VDS_MOD_CTRL, 163) && key(0, VDS_MOD_SHIFT, VDS_VK_DOWN) && key(0, VDS_MOD_SHIFT, VDS_VK_DOWN) && key(0, 0, VDS_VK_RETURN));
    assert(v.windows[1].rows == 22 && w[ROOT].h == 22 && w[LIST].h == 12 && w[BUTTONS].row == 21);
    matches_full_render();

    /* Style switch: widgets re-render in the new colours. */
    uint8_t turbo[] = {0x74, 2, VDS_STYLE_TURBO, 0};
    assert(vds_proto_batch(&p, turbo, sizeof turbo));
    assert(attr_at(0, 0, 0) == (v.style.title_fg | v.style.body << 4));
    unsigned divider = (unsigned)w[LIST].row + w[LIST].h;                     /* dark blue body: a white divider */
    assert(sc[divider * sw].ch == VDS_GLYPH_HLINE && sc[divider * sw].attr == (3 | v.style.body << 4));
    finish();
    if (argc > 2) dump(argv[2], "vds-widgets-turbo");
    matches_full_render();

    /* Wire ops, reserved windows refused, and deleting a box takes its children. */
    uint8_t ops[] = {0x80, 20, 1, VDS_WK_LABEL, VDS_NONE, 45, 0, 10, 1, 0,
                     0x81, 20, 4, 'W', 'i', 'r', 'e',
                     0x80, 21, 28, VDS_WK_LABEL, VDS_NONE, 0, 0, 5, 1, 0,
                     0x82, 20, 7, 0, 0x30};
    unsigned errors = p.errors;
    assert(vds_proto_batch(&p, ops, sizeof ops) && p.errors == errors + 1);
    text_at(0, 45, 0, 4, t);
    assert(!strcmp(t, "Wire") && w[20].value == 7 && !w[21].used);
    assert(vds_ui_widget_delete(&u, OPTIONS) && !w[CHECK].used && !w[RADIO_A].used && !w[RADIO_B].used);
    assert(w[LIST].h == 14 && w[BAR].row == 19 && w[BUTTONS].row == 21);    /* the column closed up (22 rows) */
    matches_full_render();

    /* List pool: growing lists move and compact; contents survive. */
    assert(vds_ui_widget(&u, 30, 1, VDS_WK_LIST, VDS_NONE, 45, 2, 12, 4, 0) && vds_ui_widget_count(&u, 30, 20));
    for (uint8_t i = 0; i < 20; ++i) { char s[8]; snprintf(s, sizeof s, "L%u", i); assert(vds_ui_widget_item(&u, 30, i, s, strlen(s))); }
    assert(vds_ui_widget_count(&u, LIST, 30));                              /* grows past list 30 */
    assert(!strcmp(u.items[w[LIST].first], "LIVE1D") && !strcmp(u.items[w[30].first + 19], "L19"));
    assert(!vds_ui_widget_count(&u, 30, VDS_LIST_POOL - 29));               /* pool full: refused, unchanged */
    assert(w[30].count == 20 && !strcmp(u.items[w[30].first + 19], "L19"));
    assert(vds_ui_widget_delete(&u, LIST) && u.items_used == 20 && !strcmp(u.items[w[30].first], "L0"));
    matches_full_render();

    /* Regressions from review: no parent cycles, no negative list index, a
     * trailing '&' stays inside the string. */
    assert(vds_ui_widget(&u, 41, 1, VDS_WK_BOX, VDS_NONE, 45, 10, 10, 6, 0) && vds_ui_widget_layout(&u, 41, VDS_LAYOUT_COLUMN, 0));
    assert(vds_ui_widget(&u, 42, 1, VDS_WK_BOX, 41, 0, 0, 0, 0, 0));
    assert(!vds_ui_widget(&u, 41, 1, VDS_WK_BOX, 42, 0, 0, 0, 0, 0));       /* 42 died with the old 41 */
    assert(!w[41].used && !w[42].used);
    assert(vds_ui_widget(&u, 41, 1, VDS_WK_BOX, VDS_NONE, 45, 10, 10, 6, 0) && vds_ui_widget(&u, 42, 1, VDS_WK_BOX, 41, 0, 0, 0, 0, 0));
    assert(!vds_ui_widget(&u, 41, 1, VDS_WK_BOX, 42, 0, 0, 0, 0, 0) && vds_ui_widget(&u, 42, 1, VDS_WK_BOX, VDS_NONE, 45, 10, 1, 1, 0));
    assert(vds_ui_widget_delete(&u, 42) && !vds_ui_widget_delete(&u, 41));
    assert(vds_ui_widget(&u, 43, 1, VDS_WK_LIST, VDS_NONE, 45, 10, 10, 3, 0) && vds_ui_widget_count(&u, 43, 3));
    for (uint8_t i = 0; i < 3; ++i) assert(vds_ui_widget_item(&u, 43, i, "abc", 3));
    assert(vds_ui_widget_value(&u, 43, -5) && w[43].value == 0 && vds_ui_widget_value(&u, 43, 99) && w[43].value == 2);
    u.widgets[43].value = -7;                                                     /* even if it got there somehow */
    assert(vds_ui_widget_focus(&u, 43) && w[43].value == 0 && key('a', 0, 0));
    assert(vds_ui_widget(&u, 44, 1, VDS_WK_BUTTON, VDS_NONE, 45, 14, 10, 1, 0) && vds_ui_widget_text(&u, 44, "Go&", 3));
    text_at(0, 45, 14, 10, t);
    assert(strstr(t, "Go&"));
    assert(vds_ui_widget_delete(&u, 43) && vds_ui_widget_delete(&u, 44));
    matches_full_render();

    /* Random widget traffic, keys and resizes; the screen stays exact. */
    static const uint8_t vks[] = {VDS_VK_TAB, VDS_VK_UP, VDS_VK_DOWN, VDS_VK_LEFT, VDS_VK_RIGHT, VDS_VK_HOME, VDS_VK_END,
                                  VDS_VK_PAGEUP, VDS_VK_PAGEDOWN, VDS_VK_RETURN, VDS_VK_BACKSPACE, VDS_VK_DELETE, 0, 0, 0};
    for (int step = 0; step < 6000; ++step) {
        uint8_t id = (uint8_t)rnd(VDS_MAX_WIDGETS);
        switch (rnd(13)) {
        case 0: vds_ui_widget(&u, id, 1, (uint8_t)(1 + rnd(10)), rnd(3) ? VDS_NONE : (uint8_t)rnd(VDS_MAX_WIDGETS),
                              (uint8_t)rnd(40), (uint8_t)rnd(25), (uint8_t)rnd(30), (uint8_t)rnd(6), (uint8_t)rnd(64)); break;
        case 1: vds_ui_widget_text(&u, id, "Some &text here", rnd(16)); break;
        case 2: vds_ui_widget_value(&u, id, (int16_t)rnd(120)); break;
        case 3: vds_ui_widget_layout(&u, id, (uint8_t)rnd(3), (uint8_t)rnd(3)); break;
        case 4: vds_ui_widget_size(&u, id, (uint8_t)rnd(4), (uint8_t)rnd(3)); break;
        case 5: if (rnd(2)) vds_ui_widget_count(&u, id, (uint8_t)rnd(30)); else vds_ui_widget_item(&u, id, (uint8_t)rnd(30), "item", 4); break;
        case 6: if (!rnd(4)) vds_ui_widget_delete(&u, id); else vds_ui_widget_focus(&u, id); break;
        case 7: { vds_geometry g = {(int16_t)rnd(400), (int16_t)rnd(300), (uint8_t)(1 + rnd(60)), (uint8_t)(1 + rnd(30))};
                  vds_window_resize(&v, 1, g.cols, g.rows); uint8_t b[] = {0x30}; vds_proto_batch(&p, b, 1); break; }
        case 8: vds_ui_pointer(&u, (int16_t)rnd(640), (int16_t)rnd(480), (uint8_t)rnd(2)); break;
        case 9:                                                 /* the tool widgets' ops */
            switch (rnd(5)) {
            case 0: vds_ui_text_set(&u, id, (int)rnd(2), "one\ntwo lines\r\n", rnd(17)); break;
            case 1: vds_ui_text_select(&u, id, (uint16_t)rnd(40), (uint16_t)rnd(40)); break;
            case 2: vds_ui_text_insert(&u, id, "ins\n", rnd(5)); break;
            case 3: { uint8_t c[3] = {(uint8_t)rnd(9), (uint8_t)rnd(9), (uint8_t)rnd(9)}; vds_ui_widget_columns(&u, id, c, (uint8_t)rnd(4)); break; }
            default: vds_ui_widget_append(&u, id, "a\tb\tc", rnd(6)); break;
            }
            break;
        default: key((uint8_t)(rnd(3) ? 0 : 'a' + rnd(26)), (uint8_t)(rnd(4) ? 0 : rnd(2) ? VDS_MOD_SHIFT : VDS_MOD_CTRL), vks[rnd(sizeof vks)]); break;
        }
        assert(u.items_used <= VDS_LIST_POOL);
        for (unsigned i = 0; i < VDS_MAX_WIDGETS; ++i)
            if (w[i].used && w[i].kind == VDS_WK_LIST) assert(w[i].first + w[i].count <= u.items_used);
        if (step % 300 == 299) matches_full_render();
    }
    matches_full_render();
    printf("vds widget checks passed: %u events\n", ev_n);
    return 0;
}

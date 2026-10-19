/* Host test of VDS commands and menus (lib/vios-vds/src/vds_ui.c, G3): the
 * Turbo Vision menu bar and status line, pulldowns, cascades, mnemonics,
 * accelerators, disabled items, the command palette, Smalltalk world and
 * window pop-ups, reserved ids and overlay ordering, with the screen checked
 * against full renders. Usage: vds_menu FONT.bin [DUMP_DIR] */
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
static uint8_t ev[256][32];
static unsigned ev_n;
static void send(void *ctx, uint8_t code, const uint8_t *d, uint8_t n) {
    (void)ctx;
    if (code == VDS_PACKET_EVENT) { memset(ev[ev_n & 255], 0, 32); memcpy(ev[ev_n & 255], d, n); ++ev_n; }
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
/* The last COMMAND event: returns the command id, source in *source; 0 if none since mark. */
static unsigned mark;
static unsigned command_since(uint8_t *source) {
    for (unsigned i = ev_n; i > mark; --i) {
        const uint8_t *e = ev[(i - 1) & 255];
        if (e[1] == VDS_EVENT_COMMAND) { if (source) *source = e[5]; return (unsigned)(e[3] | e[4] << 8); }
    }
    return 0;
}
static void batch(const uint8_t *b, size_t n) { assert(vds_proto_batch(&p, b, n)); }
static size_t op_cmd(uint8_t *b, unsigned id, uint8_t flags, uint8_t vk, uint8_t mods, uint8_t ascii, const char *label) {
    size_t n = strlen(label);
    b[0] = 0x60; b[1] = (uint8_t)id; b[2] = (uint8_t)(id >> 8); b[3] = flags; b[4] = vk; b[5] = mods; b[6] = ascii; b[7] = (uint8_t)n;
    memcpy(b + 8, label, n);
    return 8 + n;
}
static size_t op_menu(uint8_t *b, uint8_t menu, const char *title, const unsigned *items, uint8_t count) {
    size_t tn = strlen(title), at = 0;
    b[at++] = 0x61; b[at++] = menu; b[at++] = (uint8_t)tn;
    memcpy(b + at, title, tn); at += tn;
    b[at++] = count;
    for (unsigned i = 0; i < count; ++i) { b[at++] = (uint8_t)items[i]; b[at++] = (uint8_t)(items[i] >> 8); }
    return at;
}
static void row_text(uint8_t sid, uint8_t row, char *out) {
    const vds_surface *s = &v.surfaces[sid];
    for (unsigned i = 0; i < s->cols; ++i) out[i] = (char)s->cells[row * s->cols + i].ch;
    out[s->cols] = 0;
}
static unsigned seed = 77;
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

int main(int argc, char **argv) {
    assert(argc >= 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f && fread(font, 1, sizeof font, f) == sizeof font);
    fclose(f);
    for (int y = 0; y < H; ++y) { rows_a[y] = fb_a[y]; rows_b[y] = fb_b[y]; }
    assert(vds_init(&v, rows_a, W, H, font));
    vds_proto_init(&p, &v, cell_mem, send, 0);
    vds_ui_init(&u, &p);
    uint8_t b[255];
    size_t n = 0;

    /* Turbo Vision, an editor window and its commands, all over the wire. */
    b[n++] = 0x74; b[n++] = 2; b[n++] = VDS_STYLE_TURBO; b[n++] = 0;
    b[n++] = 0x01; b[n++] = 0; b[n++] = 60; b[n++] = 20; b[n++] = 0x42;
    b[n++] = 0x10; b[n++] = 0; b[n++] = 1; b[n++] = 1; b[n++] = 0x42; b[n++] = 8; memcpy(b + n, ": SQUARE", 8); n += 8;
    b[n++] = 0x50; b[n++] = 1; b[n++] = 0; b[n++] = 80; b[n++] = 0; b[n++] = 40; b[n++] = 0; b[n++] = 60; b[n++] = 20;
    b[n++] = VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW;
    b[n++] = 0x54; b[n++] = 1; b[n++] = 9; memcpy(b + n, "WBM1P.FTH", 9); n += 9;
    b[n++] = 0x58; b[n++] = 1; b[n++] = 0x30;
    batch(b, n);
    n = 0;
    n += op_cmd(b + n, 1, VDS_CMD_ENABLED | VDS_CMD_STATUS, 0, VDS_MOD_CTRL, 'n', "&New");
    n += op_cmd(b + n, 2, VDS_CMD_ENABLED | VDS_CMD_STATUS, VDS_VK_F3, 0, 0, "&Open...");
    n += op_cmd(b + n, 3, VDS_CMD_ENABLED | VDS_CMD_STATUS, VDS_VK_F1 + 1, 0, 0, "&Save");
    n += op_cmd(b + n, 4, 0, 0, VDS_MOD_CTRL | VDS_MOD_SHIFT, 's', "Save &as...");
    n += op_cmd(b + n, 5, VDS_CMD_ENABLED, 0, VDS_MOD_LALT, 'x', "E&xit");
    n += op_cmd(b + n, 6, VDS_CMD_ENABLED, 0, VDS_MOD_CTRL, 't', "&Stage");
    n += op_cmd(b + n, 7, VDS_CMD_ENABLED | VDS_CMD_CHECKED, 0, 0, 0, "&Activate");
    batch(b, n);
    n = 0;
    unsigned file[] = {1, 2, 3, 4, VDS_ITEM_SEPARATOR, 5}, module[] = {6, 7, VDS_ITEM_SUBMENU | 3}, recent[] = {1, 2};
    unsigned bar[] = {VDS_ITEM_SUBMENU | 1, VDS_ITEM_SUBMENU | 2};
    n += op_menu(b + n, 1, "&File", file, 6);
    n += op_menu(b + n, 2, "&Module", module, 3);
    n += op_menu(b + n, 3, "&Recent", recent, 2);
    n += op_menu(b + n, 0, "", bar, 2);
    b[n++] = 0x30;
    batch(b, n);
    assert(!p.errors && u.focus == 1);
    char text[81];
    assert(u.bar_shown && v.windows[31].used && v.windows[31].y == 0);
    row_text(15, 0, text);
    assert(!strncmp(text, " File  Module ", 14));
    assert(u.status_shown && v.windows[30].used && v.windows[30].y == H - 8);
    row_text(14, 0, text);
    assert(strstr(text, "Ctrl+N New") && strstr(text, "F3 Open...") && strstr(text, "F2 Save"));
    matches_full_render();

    /* F10: File pulldown under its title; arrows; Enter on a disabled item does nothing. */
    mark = ev_n;
    assert(key(0, 0, VDS_VK_F10) && u.depth == 1 && u.bar_index == 0 && v.windows[27].used);
    assert(v.windows[27].x == 8 && v.windows[27].y == 9);
    assert(v.surfaces[15].cells[1].attr == (v.style.menu_sel_fg | v.style.menu_sel_bg << 4));   /* highlighted title */
    row_text(11, 0, text);
    assert(!strncmp(text, "  New", 5) && strstr(text, "Ctrl+N"));
    row_text(11, 4, text);
    assert(text[0] == '-');
    assert(key(0, 0, VDS_VK_DOWN) && key(0, 0, VDS_VK_DOWN) && key(0, 0, VDS_VK_DOWN) && u.open[0].sel == 3);
    assert(key(0, 0, VDS_VK_RETURN) && u.depth == 1 && !command_since(0));     /* disabled */
    assert(key(0, 0, VDS_VK_DOWN) && u.open[0].sel == 5);                       /* skips the separator */
    assert(key(0, 0, VDS_VK_UP) && key(0, 0, VDS_VK_UP) && u.open[0].sel == 2);
    matches_full_render();
    uint8_t source = 0;
    assert(key(0, 0, VDS_VK_RETURN) && u.depth == 0 && !v.windows[27].used && u.bar_index == VDS_NONE);
    assert(command_since(&source) == 3 && source == VDS_FROM_MENU);
    assert(v.surfaces[15].cells[2].attr == (v.style.menu_fg | v.style.menu_bg << 4));          /* "i": normal again */
    matches_full_render();

    /* Alt+M, a cascade by mnemonic, Esc unwinds one level, Left moves along the bar. */
    assert(key('m', VDS_MOD_LALT, 0) && u.depth == 1 && u.bar_index == 1);
    assert(key('r', 0, 0) && u.depth == 2 && v.windows[28].used);
    assert(v.windows[28].x > v.windows[27].x);
    matches_full_render();
    assert(key(0, 0, VDS_VK_ESCAPE) && u.depth == 1);
    assert(key(0, 0, VDS_VK_LEFT) && u.depth == 1 && u.bar_index == 0);
    assert(key(0, 0, VDS_VK_ESCAPE) && u.depth == 0);
    mark = ev_n;
    assert(key(0, 0, VDS_VK_F10) && key('o', 0, 0) && u.depth == 0 && command_since(0) == 2);
    /* Menus stay above windows even when one is raised under them. */
    assert(key(0, 0, VDS_VK_F10) && vds_window_raise(&v, 1) && v.order[v.count - 1] == 27);
    assert(key(0, 0, VDS_VK_ESCAPE));
    matches_full_render();

    /* Accelerators, including control codes for Ctrl+letter; disabled ones pass through. */
    mark = ev_n;
    assert(key(0x0E, VDS_MOD_CTRL, 0) && command_since(&source) == 1 && source == VDS_FROM_ACCELERATOR);
    mark = ev_n;
    assert(key(0, 0, VDS_VK_F3) && command_since(0) == 2);
    mark = ev_n;
    assert(key('x', VDS_MOD_LALT, 0) && command_since(0) == 5);
    mark = ev_n;
    assert(!key(0x13, VDS_MOD_CTRL | VDS_MOD_SHIFT, 0) && !command_since(0));
    uint8_t enable4[] = {0x62, 4, 0, VDS_CMD_ENABLED, 0x30};
    batch(enable4, sizeof enable4);
    assert(key(0x13, VDS_MOD_CTRL | VDS_MOD_SHIFT, 0) && command_since(0) == 4);

    /* Command palette: subsequence filter, backspace, Enter. */
    mark = ev_n;
    assert(key(0x0B, VDS_MOD_CTRL, 0) && u.depth == 1 && u.palette);
    assert(key('s', 0, 0) && key('v', 0, 0) && u.open[0].count == 2);         /* Save, Save as... */
    row_text(11, 0, text);
    assert(!strncmp(text, " > sv_", 6));
    assert(key('q', 0, 0) && u.open[0].count == 0);
    assert(key(0, 0, VDS_VK_BACKSPACE) && u.open[0].count == 2);
    matches_full_render();
    assert(key(0, 0, VDS_VK_DOWN) && key(0, 0, VDS_VK_RETURN) && u.depth == 0 && !u.palette);
    assert(command_since(&source) == 4 && source == VDS_FROM_PALETTE);
    matches_full_render();

    /* A command's state change redraws the open menu and the status line. */
    assert(key(0, 0, VDS_VK_F10));
    uint8_t disable1[] = {0x62, 1, 0, 0, 0x30};
    batch(disable1, sizeof disable1);
    assert(v.surfaces[11].cells[2].attr == (v.style.menu_disabled | v.style.menu_sel_bg << 4));
    row_text(14, 0, text);
    assert(!strstr(text, "New"));
    assert(key(0, 0, VDS_VK_ESCAPE));
    matches_full_render();
    if (argc > 2) {                                    /* a screenshot with the File menu open */
        assert(key(0, 0, VDS_VK_F10) && key(0, 0, VDS_VK_DOWN));
        finish();
        dump(argv[2], "vds-menu-turbo");
        assert(key(0, 0, VDS_VK_ESCAPE));
    }

    /* Reserved ids are refused over the wire. */
    unsigned errors = p.errors;
    uint8_t bad_win[] = {0x52, 31, 0x01, 12, 10, 1, 0x30, 0x58, 28};
    batch(bad_win, sizeof bad_win);
    assert(p.errors == errors + 3 && v.surfaces[12].cells != cells[12]);       /* not handed to the CPU */

    /* Smalltalk: no bar or status line; F10 is the world menu, Shift+F10 the window's menu. */
    uint8_t st[] = {0x74, 2, VDS_STYLE_SMALLTALK, 0, 0x59, 1, 2, 0x30};
    batch(st, sizeof st);
    assert(!u.bar_shown && !v.windows[31].used && !u.status_shown && !v.windows[30].used);
    assert(key(0, 0, VDS_VK_F10) && u.depth == 1 && u.open[0].menu == 0 && v.windows[27].x == 16);
    row_text(11, 0, text);
    assert(strstr(text, "File") && strchr(text, '>'));
    assert(key(0, 0, VDS_VK_RIGHT) && u.depth == 2 && u.open[1].menu == 1);
    matches_full_render();
    if (argc > 2) { finish(); dump(argv[2], "vds-menu-smalltalk"); }
    assert(key(0, 0, VDS_VK_ESCAPE) && key(0, 0, VDS_VK_ESCAPE) && u.depth == 0);
    assert(key(0, VDS_MOD_SHIFT, VDS_VK_F10) && u.depth == 1 && u.open[0].menu == 2);
    assert(v.windows[27].x == v.windows[1].x + 16);
    assert(key(0, 0, VDS_VK_ESCAPE));
    matches_full_render();

    /* Reset drops commands and menus with the windows. */
    uint8_t reset[] = {0x34, 0x30};
    batch(reset, sizeof reset);
    assert(!u.commands[0].flags && !u.menus[0].used && u.depth == 0 && u.focus == VDS_NONE);
    assert(!key(0, 0, VDS_VK_F10));

    /* Random keys with style and state changes; the screen stays exact. */
    n = 0;
    n += op_cmd(b + n, 1, VDS_CMD_ENABLED | VDS_CMD_STATUS, 0, VDS_MOD_CTRL, 'n', "&New");
    n += op_cmd(b + n, 2, VDS_CMD_ENABLED | VDS_CMD_STATUS, VDS_VK_F3, 0, 0, "&Open...");
    n += op_cmd(b + n, 3, VDS_CMD_ENABLED, 0, 0, 0, "&Save");
    n += op_cmd(b + n, 6, VDS_CMD_ENABLED, 0, VDS_MOD_CTRL, 't', "&Stage");
    n += op_menu(b + n, 1, "&File", file, 3);
    n += op_menu(b + n, 2, "&Module", module, 3);
    n += op_menu(b + n, 3, "&Recent", recent, 2);
    n += op_menu(b + n, 0, "", bar, 2);
    batch(b, n);
    assert(vds_surface_init(&v, 0, 60, 20, cells[0], 0x30));
    assert(vds_window_open(&v, 1, 0, 80, 40, 60, 20, VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW, "W") && vds_ui_focus(&u, 1));
    static const uint8_t vks[] = {VDS_VK_F10, VDS_VK_UP, VDS_VK_DOWN, VDS_VK_LEFT, VDS_VK_RIGHT, VDS_VK_RETURN,
                                  VDS_VK_ESCAPE, VDS_VK_BACKSPACE, VDS_VK_F3, VDS_VK_F6, VDS_VK_F5, 0, 0, 0};
    static const char letters[] = "fmrsaonx k";
    for (int step = 0; step < 5000; ++step) {
        switch (rnd(10)) {
        case 0: { uint8_t s[] = {0x74, 2, (uint8_t)rnd(2), 0}; batch(s, sizeof s); break; }
        case 1: { uint8_t s[] = {0x62, (uint8_t)(1 + rnd(7)), 0, (uint8_t)rnd(16)}; vds_proto_batch(&p, s, sizeof s); break; }
        case 2: vds_proto_frame(&p, rnd(40), 1 + rnd(60000)); break;
        default: {
            uint8_t vk = vks[rnd(sizeof vks)], mods = (uint8_t)(rnd(3) ? 0 : (rnd(2) ? VDS_MOD_SHIFT : rnd(2) ? VDS_MOD_CTRL : VDS_MOD_LALT));
            key(vk ? 0 : (uint8_t)letters[rnd(sizeof letters - 1)], mods, vk);
        }
        }
        assert(u.depth <= VDS_MENU_DEPTH);
        for (unsigned l = 0; l < VDS_MENU_DEPTH; ++l) assert(v.windows[27 + l].used == (l < u.depth));
        if (u.depth) assert(v.order[v.count - 1] == 27 + u.depth - 1);
        if (step % 250 == 249) matches_full_render();
    }
    matches_full_render();
    printf("vds menu checks passed: %u events\n", ev_n);
    return 0;
}

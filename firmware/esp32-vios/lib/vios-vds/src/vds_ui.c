/* VIOS Display Server intrinsics: see vds_ui.h. */
#include "vds_ui.h"
#include <string.h>

#define BAR_WINDOW (VDS_UI_FIRST_WINDOW + 4)
#define STATUS_WINDOW (VDS_UI_FIRST_WINDOW + 3)
#define BAR_SURFACE (VDS_UI_FIRST_SURFACE + 4)
#define STATUS_SURFACE (VDS_UI_FIRST_SURFACE + 3)
#define POPUP_FLAGS (VDS_W_VISIBLE | VDS_W_OVERLAY | VDS_W_MENU | VDS_W_SHADOW)

static vds *core(vds_ui *u) { return u->p->v; }
static uint8_t lower(uint8_t c) { return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c; }
static uint8_t attr(uint8_t fg, uint8_t bg) { return (uint8_t)(fg | bg << 4); }

/* ------------------------------------------------------------ windows */

static int focusable(vds *v, uint8_t id) {
    return id < VDS_MAX_WINDOWS && v->windows[id].used &&
           (v->windows[id].flags & (VDS_W_VISIBLE | VDS_W_DECOR)) == (VDS_W_VISIBLE | VDS_W_DECOR);
}

static uint8_t modal(vds *v) {
    for (int i = v->count - 1; i >= 0; --i) {
        const vds_window *w = &v->windows[v->order[i]];
        if ((w->flags & (VDS_W_VISIBLE | VDS_W_MODAL)) == (VDS_W_VISIBLE | VDS_W_MODAL)) return v->order[i];
    }
    return VDS_NONE;
}

static vds_geometry geometry_of(vds *v, uint8_t id) {
    vds_geometry g = {v->windows[id].x, v->windows[id].y, v->windows[id].cols, v->windows[id].rows};
    return g;
}

static void widgets_check(vds_ui *u);

/* Move and resize a window; widgets inside re-lay out to the new size. */
static void apply(vds_ui *u, uint8_t id, vds_geometry g) {
    vds_window_move(core(u), id, g.x, g.y);
    vds_window_resize(core(u), id, g.cols, g.rows);
    widgets_check(u);
}

static void win_event(vds_ui *u, uint8_t what, uint8_t id) {
    const vds_window *w = &core(u)->windows[id];
    uint8_t args[7] = {what, (uint8_t)w->x, (uint8_t)((uint16_t)w->x >> 8), (uint8_t)w->y, (uint8_t)((uint16_t)w->y >> 8),
                       w->cols, w->rows};
    vds_proto_event(u->p, VDS_EVENT_WIN, id, args, sizeof args);
}

static void widgets_check(vds_ui *u) {                       /* re-lay out resized windows */
    vds *v = core(u);
    for (uint8_t id = 0; id < VDS_UI_FIRST_WINDOW; ++id) {
        if (!v->windows[id].used || (u->widget_cols[id] == v->windows[id].cols && u->widget_rows[id] == v->windows[id].rows)) continue;
        for (unsigned i = 0; i < VDS_MAX_WIDGETS; ++i)
            if (u->widgets[i].used && u->widgets[i].window == id) { vds_ui_widgets_refresh(u, id); break; }
    }
}

static int has_widgets(vds_ui *u, uint8_t window) {
    for (unsigned i = 0; i < VDS_MAX_WIDGETS; ++i) if (u->widgets[i].used && u->widgets[i].window == window) return 1;
    return 0;
}

void vds_ui_forget(vds_ui *u, uint8_t id) {
    if (id >= VDS_MAX_WINDOWS) return;
    vds_ui_widgets_forget(u, id);
    if (u->focus == id) { u->focus = VDS_NONE; u->wm_mode = 0; }
    u->zoomed &= ~((uint64_t)1 << id);
    u->window_menu[id] = VDS_NONE;
}

int vds_ui_focus(vds_ui *u, uint8_t id) {
    vds *v = core(u);
    if (id != VDS_NONE && !focusable(v, id)) return 0;
    uint8_t m = modal(v);
    if (m != VDS_NONE && id != m) return 0;
    uint8_t previous = u->focus;
    if (previous != id && focusable(v, previous))
        vds_window_flags(v, previous, (uint8_t)(v->windows[previous].flags & ~VDS_W_ACTIVE));
    if (id != VDS_NONE) {
        vds_window_flags(v, id, (uint8_t)(v->windows[id].flags | VDS_W_ACTIVE));
        vds_window_raise(v, id);
    }
    u->focus = id;
    u->wm_mode = 0;
    if (previous != id) {
        if (previous < VDS_UI_FIRST_WINDOW && v->windows[previous].used && has_widgets(u, previous)) vds_ui_widgets_refresh(u, previous);
        if (id != VDS_NONE && has_widgets(u, id)) vds_ui_widgets_refresh(u, id);
        vds_proto_event(u->p, VDS_EVENT_FOCUS, id, &previous, 1);
    }
    return 1;
}

static int cycle(vds_ui *u, int backwards) {
    vds *v = core(u);
    if (modal(v) != VDS_NONE) return vds_ui_focus(u, modal(v));
    if (backwards && focusable(v, u->focus)) vds_window_lower(v, u->focus);
    if (backwards) {                                        /* new topmost focusable window */
        for (int i = v->count - 1; i >= 0; --i)
            if (focusable(v, v->order[i])) return vds_ui_focus(u, v->order[i]);
    } else {                                                /* bring up the bottom-most one */
        for (unsigned i = 0; i < v->count; ++i)
            if (focusable(v, v->order[i]) && v->order[i] != u->focus) return vds_ui_focus(u, v->order[i]);
    }
    return 1;
}

static void zoom(vds_ui *u) {
    vds *v = core(u);
    uint8_t id = u->focus;
    if (u->zoomed & ((uint64_t)1 << id)) {
        apply(u, id, u->zoom_saved[id]);
        u->zoomed &= ~((uint64_t)1 << id);
        win_event(u, VDS_WIN_RESTORED, id);
        return;
    }
    u->zoom_saved[id] = geometry_of(v, id);
    int turbo = v->style.kind == VDS_STYLE_TURBO;
    int top = (turbo ? 8 : 12) + (u->bar_shown ? 8 : 0), bottom = (turbo ? 8 : 2) + (u->status_shown ? 8 : 0);
    vds_geometry g = {8, (int16_t)top, (uint8_t)((v->width - 16) / 8), (uint8_t)((v->height - top - bottom) / 8)};
    apply(u, id, g);
    u->zoomed |= (uint64_t)1 << id;
    win_event(u, VDS_WIN_ZOOMED, id);
}

static int wm_key(vds_ui *u, uint8_t modifiers, uint8_t vk) {
    vds *v = core(u);
    uint8_t id = u->focus;
    if (!focusable(v, id)) { u->wm_mode = 0; return 1; }
    vds_geometry g = geometry_of(v, id);
    int resize = (modifiers & VDS_MOD_SHIFT) != 0;
    switch (vk) {
    case VDS_VK_LEFT: case VDS_VK_KP_LEFT:
        if (resize) { if (g.cols > 1) --g.cols; } else g.x = (int16_t)(g.x - 8);
        break;
    case VDS_VK_RIGHT: case VDS_VK_KP_RIGHT:
        if (resize) { if (g.cols < 255) ++g.cols; } else g.x = (int16_t)(g.x + 8);
        break;
    case VDS_VK_UP: case VDS_VK_KP_UP:
        if (resize) { if (g.rows > 1) --g.rows; } else g.y = (int16_t)(g.y - 8);
        break;
    case VDS_VK_DOWN: case VDS_VK_KP_DOWN:
        if (resize) { if (g.rows < 255) ++g.rows; } else g.y = (int16_t)(g.y + 8);
        break;
    case VDS_VK_RETURN: case VDS_VK_KP_ENTER:
        u->wm_mode = 0;
        win_event(u, VDS_WIN_MOVED, id);
        return 1;
    case VDS_VK_ESCAPE:
        apply(u, id, u->wm_saved);
        u->wm_mode = 0;
        return 1;
    default:
        return 1;                                           /* the mode swallows other keys */
    }
    apply(u, id, g);
    return 1;
}

/* ------------------------------------------------------------ commands */

static vds_command *command(vds_ui *u, uint16_t id) {
    for (unsigned i = 0; i < VDS_MAX_COMMANDS; ++i)
        if ((u->commands[i].flags & VDS_CMD_USED) && u->commands[i].id == id) return &u->commands[i];
    return 0;
}

int vds_ui_command(vds_ui *u, uint16_t id, uint8_t flags, uint8_t vk, uint8_t mods, uint8_t ascii, const char *label) {
    if (id >= VDS_ITEM_SUBMENU || !label) return 0;
    vds_command *c = command(u, id);
    for (unsigned i = 0; !c && i < VDS_MAX_COMMANDS; ++i)
        if (!(u->commands[i].flags & VDS_CMD_USED)) c = &u->commands[i];
    if (!c) return 0;
    c->id = id;
    c->flags = (uint8_t)(flags | VDS_CMD_USED);
    c->vk = vk;
    c->mods = mods;
    c->ascii = ascii;
    size_t n = strlen(label);
    if (n >= VDS_LABEL_MAX) n = VDS_LABEL_MAX - 1;
    memcpy(c->label, label, n);
    c->label[n] = 0;
    vds_ui_refresh(u);
    return 1;
}

int vds_ui_command_state(vds_ui *u, uint16_t id, uint8_t flags) {
    vds_command *c = command(u, id);
    if (!c) return 0;
    c->flags = (uint8_t)(flags | VDS_CMD_USED);
    vds_ui_refresh(u);
    return 1;
}

int vds_ui_menu(vds_ui *u, uint8_t menu, const char *title, const uint16_t *items, uint8_t count) {
    if (menu >= VDS_MAX_MENUS || count > VDS_MENU_ITEMS || (count && !items)) return 0;
    for (unsigned i = 0; i < count; ++i)
        if (items[i] != VDS_ITEM_SEPARATOR && items[i] >= VDS_ITEM_SUBMENU && (items[i] & 0xFF) >= VDS_MAX_MENUS) return 0;
    vds_menu *m = &u->menus[menu];
    m->used = 1;
    m->count = count;
    if (count) memcpy(m->items, items, count * sizeof *items);
    size_t n = title ? strlen(title) : 0;
    if (n >= sizeof m->title) n = sizeof m->title - 1;
    if (n) memcpy(m->title, title, n);
    m->title[n] = 0;
    vds_ui_refresh(u);
    return 1;
}

int vds_ui_window_menu(vds_ui *u, uint8_t window, uint8_t menu) {
    if (window >= VDS_MAX_WINDOWS || (menu != VDS_NONE && menu >= VDS_MAX_MENUS)) return 0;
    u->window_menu[window] = menu;
    return 1;
}

void vds_ui_accel_text(const vds_command *c, char *out) {
    char key[6] = {0};
    if (c->vk >= VDS_VK_F1 && c->vk <= VDS_VK_F12) {
        unsigned f = c->vk - VDS_VK_F1 + 1u;
        key[0] = 'F';
        if (f >= 10) { key[1] = '1'; key[2] = (char)('0' + f - 10); } else key[1] = (char)('0' + f);
    } else if (c->vk == VDS_VK_ESCAPE) memcpy(key, "Esc", 4);
    else if (c->vk == VDS_VK_RETURN) memcpy(key, "Enter", 6);
    else if (c->vk == VDS_VK_TAB) memcpy(key, "Tab", 4);
    else if (c->ascii > 32 && c->ascii < 127) key[0] = (char)(c->ascii >= 'a' && c->ascii <= 'z' ? c->ascii - 32 : c->ascii);
    const char *parts[4] = {c->mods & VDS_MOD_CTRL ? "Ctrl+" : "", c->mods & VDS_MOD_SHIFT ? "Shift+" : "",
                            c->mods & (VDS_MOD_LALT | VDS_MOD_RALT) ? "Alt+" : "", key};
    size_t n = 0;
    out[0] = 0;
    if (!key[0]) return;
    for (unsigned i = 0; i < 4; ++i)                        /* bounded: never past VDS_ACCEL_MAX */
        for (const char *s = parts[i]; *s && n < VDS_ACCEL_MAX - 1; ++s) out[n++] = *s;
    out[n] = 0;
}

/* Label without '&'; *mnemonic gets the display index of the marked char or -1. */
static size_t display(const char *label, char *out, int *mnemonic) {
    size_t n = 0;
    *mnemonic = -1;
    for (; *label && n < VDS_LABEL_MAX - 1; ++label) {
        if (*label == '&' && label[1] && label[1] != '&') { *mnemonic = (int)n; continue; }
        if (*label == '&' && label[1] == '&') ++label;
        out[n++] = *label;
    }
    out[n] = 0;
    return n;
}

static int mnemonic_of(const char *label) {
    char text[VDS_LABEL_MAX];
    int m;
    display(label, text, &m);
    return m >= 0 ? lower((uint8_t)text[m]) : -1;
}

static const char *item_label(vds_ui *u, uint16_t item) {
    if (item == VDS_ITEM_SEPARATOR) return "";
    if (item >= VDS_ITEM_SUBMENU) return u->menus[item & 0xFF].title;
    vds_command *c = command(u, item);
    return c ? c->label : "?";
}

static int item_enabled(vds_ui *u, uint16_t item) {
    if (item == VDS_ITEM_SEPARATOR) return 0;
    if (item >= VDS_ITEM_SUBMENU) return u->menus[item & 0xFF].used;
    vds_command *c = command(u, item);
    return c && (c->flags & VDS_CMD_ENABLED);
}

static void emit_command(vds_ui *u, uint16_t id, uint8_t source) {
    uint8_t args[3] = {(uint8_t)id, (uint8_t)(id >> 8), source};
    vds_proto_event(u->p, VDS_EVENT_COMMAND, u->focus, args, sizeof args);
}

/* ------------------------------------------------------------ menus */

static int matches(const char *label, const char *filter) {      /* case-insensitive subsequence */
    for (; *filter; ++filter) {
        while (*label && lower((uint8_t)*label) != lower((uint8_t)*filter)) ++label;
        if (!*label) return 0;
        ++label;
    }
    return 1;
}

static void palette_items(vds_ui *u, vds_open_menu *om) {
    om->count = 0;
    for (unsigned i = 0; i < VDS_MAX_COMMANDS && om->count < VDS_MENU_ITEMS; ++i) {
        const vds_command *c = &u->commands[i];
        char text[VDS_LABEL_MAX];
        int m;
        display(c->label, text, &m);
        if ((c->flags & (VDS_CMD_USED | VDS_CMD_ENABLED)) == (VDS_CMD_USED | VDS_CMD_ENABLED) && matches(text, u->filter))
            om->items[om->count++] = c->id;
    }
    if (om->sel >= om->count) om->sel = om->count ? (uint8_t)(om->count - 1) : 0;
}

static void snapshot(vds_ui *u, unsigned level) {
    vds_open_menu *om = &u->open[level];
    if (u->palette && level == 0) { palette_items(u, om); return; }
    const vds_menu *m = &u->menus[om->menu];
    om->count = m->count;
    memcpy(om->items, m->items, sizeof om->items);
    if (om->sel >= om->count) om->sel = om->count ? (uint8_t)(om->count - 1) : 0;
}

static uint8_t popup_width(vds_ui *u, unsigned level) {
    const vds_open_menu *om = &u->open[level];
    unsigned width = u->palette && level == 0 ? 4u + VDS_FILTER_MAX : 10;
    for (unsigned i = 0; i < om->count; ++i) {
        char text[VDS_LABEL_MAX], accel[VDS_ACCEL_MAX] = {0};
        int m;
        size_t n = display(item_label(u, om->items[i]), text, &m);
        const vds_command *c = om->items[i] < VDS_ITEM_SUBMENU ? command(u, om->items[i]) : 0;
        if (c) vds_ui_accel_text(c, accel);
        size_t need = 2 + n + (accel[0] ? strlen(accel) + 2 : 0) + (om->items[i] >= VDS_ITEM_SUBMENU ? 3 : 2);
        if (need > width) width = (unsigned)need;
    }
    return (uint8_t)(width > VDS_POPUP_COLS ? VDS_POPUP_COLS : width);
}

static uint8_t popup_rows(vds_ui *u, unsigned level) {
    return (uint8_t)((u->open[level].count ? u->open[level].count : 1) + (u->palette && level == 0));
}

static void render_popup(vds_ui *u, unsigned level) {
    vds *v = core(u);
    const vds_style *st = &v->style;
    const vds_open_menu *om = &u->open[level];
    uint8_t sid = (uint8_t)(VDS_UI_FIRST_SURFACE + level), width = v->surfaces[sid].cols;
    unsigned header = u->palette && level == 0;
    vds_fill(v, sid, 0, 0, width, v->surfaces[sid].rows, ' ', attr(st->menu_fg, st->menu_bg));
    if (header) {
        char line[VDS_POPUP_COLS + 1] = "> ";
        strcat(line, u->filter);
        strcat(line, "_");
        vds_put(v, sid, 1, 0, attr(st->menu_key, st->menu_bg), line, strlen(line));
    }
    if (!om->count) {
        vds_put(v, sid, 2, (uint8_t)header, attr(st->menu_disabled, st->menu_bg), "(none)", 6);
        return;
    }
    for (unsigned i = 0; i < om->count; ++i) {
        uint8_t row = (uint8_t)(i + header);
        uint16_t item = om->items[i];
        if (item == VDS_ITEM_SEPARATOR) {
            vds_fill(v, sid, 0, row, width, 1, '-', attr(st->menu_disabled, st->menu_bg));
            continue;
        }
        int sel = i == om->sel, on = item_enabled(u, item);
        uint8_t bg = sel ? st->menu_sel_bg : st->menu_bg;
        uint8_t fg = !on ? st->menu_disabled : sel ? st->menu_sel_fg : st->menu_fg;
        vds_fill(v, sid, 0, row, width, 1, ' ', attr(fg, bg));
        char text[VDS_LABEL_MAX], accel[VDS_ACCEL_MAX] = {0};
        int m;
        size_t n = display(item_label(u, item), text, &m);
        if (n > (size_t)width - 3) n = (size_t)width - 3;
        vds_put(v, sid, 2, row, attr(fg, bg), text, n);
        if (m >= 0 && (size_t)m < n && on) vds_put(v, sid, (uint8_t)(2 + m), row, attr(sel ? fg : st->menu_key, bg), text + m, 1);
        const vds_command *c = item < VDS_ITEM_SUBMENU ? command(u, item) : 0;
        if (c && (c->flags & VDS_CMD_CHECKED)) vds_put(v, sid, 0, row, attr(fg, bg), "*", 1);
        if (item >= VDS_ITEM_SUBMENU) vds_put(v, sid, (uint8_t)(width - 2), row, attr(fg, bg), ">", 1);
        else if (c) {
            vds_ui_accel_text(c, accel);
            size_t a = strlen(accel);
            if (a && 2 + n + 1 + a < width) vds_put(v, sid, (uint8_t)(width - 1 - a), row, attr(fg, bg), accel, a);
        }
    }
}

static void close_from(vds_ui *u, unsigned level) {
    while (u->depth > level) {
        --u->depth;
        vds_window_close(core(u), (uint8_t)(VDS_UI_FIRST_WINDOW + u->depth));
    }
    if (!level) { u->palette = 0; u->bar_index = VDS_NONE; }
}

/* Open menu `menu` (or the palette) as the next level at (x, y), clamped to the screen. */
static int open_level(vds_ui *u, uint8_t menu, int x, int y) {
    vds *v = core(u);
    if (u->depth >= VDS_MENU_DEPTH) return 0;
    unsigned level = u->depth;
    vds_open_menu *om = &u->open[level];
    om->menu = menu;
    om->sel = 0;
    snapshot(u, level);
    while (om->sel < om->count && !item_enabled(u, om->items[om->sel])) ++om->sel;
    if (om->sel >= om->count) om->sel = 0;
    uint8_t width = popup_width(u, level), rows = popup_rows(u, level);
    if (x > v->width - width * 8 - 8) x = v->width - width * 8 - 8;
    if (y > v->height - rows * 8 - 8) y = v->height - rows * 8 - 8;
    if (x < 8) x = 8;
    if (y < 1) y = 1;
    uint8_t sid = (uint8_t)(VDS_UI_FIRST_SURFACE + level), wid = (uint8_t)(VDS_UI_FIRST_WINDOW + level);
    vds_surface_init(v, sid, width, rows, u->popup_cells[level], attr(v->style.menu_fg, v->style.menu_bg));
    ++u->depth;
    render_popup(u, level);
    vds_window_close(v, wid);
    return vds_window_open(v, wid, sid, (int16_t)x, (int16_t)y, width, rows, POPUP_FLAGS, "");
}

/* Re-open the palette with fresh contents (its size follows the matches). */
static void reopen_palette(vds_ui *u) {
    vds *v = core(u);
    int x = v->windows[VDS_UI_FIRST_WINDOW].x, y = v->windows[VDS_UI_FIRST_WINDOW].y;
    uint8_t sel = u->open[0].sel;
    while (u->depth) { --u->depth; vds_window_close(v, (uint8_t)(VDS_UI_FIRST_WINDOW + u->depth)); }
    open_level(u, 0, x, y);
    u->open[0].sel = sel < u->open[0].count ? sel : 0;
    render_popup(u, 0);
}

static void bar_titles(vds_ui *u, uint8_t *cols, unsigned *n) {
    const vds_menu *bar = &u->menus[0];
    unsigned col = 1;
    *n = 0;
    for (unsigned i = 0; i < bar->count; ++i) {
        char text[VDS_LABEL_MAX];
        int m;
        cols[(*n)++] = (uint8_t)(col < 255 ? col : 255);
        col += (unsigned)display(item_label(u, bar->items[i]), text, &m) + 2;
    }
}

static void render_bar(vds_ui *u) {
    vds *v = core(u);
    const vds_style *st = &v->style;
    const vds_menu *bar = &u->menus[0];
    uint8_t cols[VDS_MENU_ITEMS];
    unsigned n;
    bar_titles(u, cols, &n);
    vds_fill(v, BAR_SURFACE, 0, 0, 80, 1, ' ', attr(st->menu_fg, st->menu_bg));
    for (unsigned i = 0; i < n; ++i) {
        char text[VDS_LABEL_MAX];
        int m;
        size_t len = display(item_label(u, bar->items[i]), text, &m);
        int sel = i == u->bar_index;
        uint8_t fg = sel ? st->menu_sel_fg : st->menu_fg, bg = sel ? st->menu_sel_bg : st->menu_bg;
        if (cols[i] + len + 1 > 80) break;
        vds_fill(v, BAR_SURFACE, (uint8_t)(cols[i] - 1), 0, (uint8_t)(len + 2), 1, ' ', attr(fg, bg));
        vds_put(v, BAR_SURFACE, cols[i], 0, attr(fg, bg), text, len);
        if (m >= 0) vds_put(v, BAR_SURFACE, (uint8_t)(cols[i] + m), 0, attr(sel ? fg : st->menu_key, bg), text + m, 1);
    }
}

static void render_status(vds_ui *u) {
    vds *v = core(u);
    const vds_style *st = &v->style;
    unsigned col = 1;
    vds_fill(v, STATUS_SURFACE, 0, 0, 80, 1, ' ', attr(st->menu_fg, st->menu_bg));
    for (unsigned i = 0; i < VDS_MAX_COMMANDS; ++i) {
        const vds_command *c = &u->commands[i];
        if ((c->flags & (VDS_CMD_USED | VDS_CMD_STATUS)) != (VDS_CMD_USED | VDS_CMD_STATUS)) continue;
        char accel[VDS_ACCEL_MAX] = {0}, text[VDS_LABEL_MAX];
        int m;
        vds_ui_accel_text(c, accel);
        size_t a = strlen(accel), n = display(c->label, text, &m);
        if (!a || col + a + 1 + n > 79) continue;
        vds_put(v, STATUS_SURFACE, (uint8_t)col, 0, attr(st->menu_key, st->menu_bg), accel, a);
        vds_put(v, STATUS_SURFACE, (uint8_t)(col + a + 1), 0, attr(st->menu_fg, st->menu_bg), text, n);
        col += (unsigned)(a + n + 3);
    }
}

void vds_ui_refresh(vds_ui *u) {
    vds *v = core(u);
    u->style_seen = v->style.kind;
    int turbo = v->style.kind == VDS_STYLE_TURBO;
    int want_bar = turbo && u->menus[0].used && u->menus[0].count;
    if (u->depth && u->bar_index != VDS_NONE && (!want_bar || u->bar_index >= u->menus[0].count))
        close_from(u, 0);                                   /* a bar pulldown cannot outlive its bar entry */
    if (want_bar && !u->bar_shown) {
        vds_surface_init(v, BAR_SURFACE, 80, 1, u->bar_cells, attr(v->style.menu_fg, v->style.menu_bg));
        vds_window_open(v, BAR_WINDOW, BAR_SURFACE, 0, 0, 80, 1, VDS_W_VISIBLE | VDS_W_OVERLAY, "");
    } else if (!want_bar && u->bar_shown) vds_window_close(v, BAR_WINDOW);
    u->bar_shown = (uint8_t)want_bar;
    if (want_bar) render_bar(u);
    int any_status = 0;
    for (unsigned i = 0; i < VDS_MAX_COMMANDS; ++i)
        any_status |= (u->commands[i].flags & (VDS_CMD_USED | VDS_CMD_STATUS)) == (VDS_CMD_USED | VDS_CMD_STATUS);
    int want_status = turbo && any_status;
    if (want_status && !u->status_shown) {
        vds_surface_init(v, STATUS_SURFACE, 80, 1, u->status_cells, attr(v->style.menu_fg, v->style.menu_bg));
        vds_window_open(v, STATUS_WINDOW, STATUS_SURFACE, 0, (int16_t)(v->height - 8), 80, 1, VDS_W_VISIBLE | VDS_W_OVERLAY, "");
    } else if (!want_status && u->status_shown) vds_window_close(v, STATUS_WINDOW);
    u->status_shown = (uint8_t)want_status;
    if (want_status) render_status(u);
    for (unsigned level = 0; level < u->depth; ++level)     /* open menus stay above the bars */
        if (v->order[v->count - u->depth + level] != VDS_UI_FIRST_WINDOW + level)
            vds_window_raise(v, (uint8_t)(VDS_UI_FIRST_WINDOW + level));
    for (unsigned level = 0; level < u->depth; ++level) {
        if (u->palette && level == 0) { render_popup(u, 0); continue; }
        snapshot(u, level);
        if (popup_width(u, level) != v->surfaces[VDS_UI_FIRST_SURFACE + level].cols ||
            popup_rows(u, level) != v->surfaces[VDS_UI_FIRST_SURFACE + level].rows) {
            close_from(u, level);                       /* contents changed shape: close it */
            if (!level && want_bar) render_bar(u);
            break;
        }
        render_popup(u, level);
    }
}

static int open_bar(vds_ui *u, unsigned index) {
    vds_menu *bar = &u->menus[0];
    if (index >= bar->count) return 1;
    uint8_t cols[VDS_MENU_ITEMS];
    unsigned n;
    bar_titles(u, cols, &n);
    close_from(u, 0);
    uint16_t item = bar->items[index];
    if (item >= VDS_ITEM_SUBMENU && item != VDS_ITEM_SEPARATOR) {
        u->bar_index = (uint8_t)index;
        render_bar(u);
        open_level(u, (uint8_t)(item & 0xFF), cols[index] * 8, 9);
        return 1;
    }
    render_bar(u);
    if (item_enabled(u, item)) emit_command(u, item, VDS_FROM_MENU);
    return 1;
}

static void move_sel(vds_ui *u, unsigned level, int dir) {
    vds_open_menu *om = &u->open[level];
    if (!om->count) return;
    for (unsigned tries = 0; tries < om->count; ++tries) {
        om->sel = (uint8_t)((om->sel + om->count + dir) % om->count);
        if (om->items[om->sel] != VDS_ITEM_SEPARATOR) break;
    }
    render_popup(u, level);
}

static void activate(vds_ui *u, unsigned level) {
    vds *v = core(u);
    vds_open_menu *om = &u->open[level];
    if (!om->count) return;
    uint16_t item = om->items[om->sel];
    if (!item_enabled(u, item)) return;
    if (item >= VDS_ITEM_SUBMENU) {
        const vds_window *w = &v->windows[VDS_UI_FIRST_WINDOW + level];
        int x = w->x + w->cols * 8 + 4, y = w->y + om->sel * 8;
        close_from(u, level + 1);
        open_level(u, (uint8_t)(item & 0xFF), x, y);
        return;
    }
    uint8_t source = u->palette ? VDS_FROM_PALETTE : VDS_FROM_MENU;
    int bar = u->bar_index != VDS_NONE;
    close_from(u, 0);
    if (bar) render_bar(u);
    emit_command(u, item, source);
}

static int menu_key(vds_ui *u, uint8_t ascii, uint8_t modifiers, uint8_t vk) {
    unsigned level = u->depth - 1u;
    vds_open_menu *om = &u->open[level];
    int bar = u->bar_index != VDS_NONE;
    switch (vk) {
    case VDS_VK_UP: case VDS_VK_KP_UP: move_sel(u, level, -1); return 1;
    case VDS_VK_DOWN: case VDS_VK_KP_DOWN: move_sel(u, level, 1); return 1;
    case VDS_VK_RIGHT: case VDS_VK_KP_RIGHT:
        if (om->count && om->items[om->sel] >= VDS_ITEM_SUBMENU && om->items[om->sel] != VDS_ITEM_SEPARATOR) activate(u, level);
        else if (bar && u->menus[0].count) open_bar(u, (u->bar_index + 1u) % u->menus[0].count);
        return 1;
    case VDS_VK_LEFT: case VDS_VK_KP_LEFT:
        if (level) close_from(u, level);
        else if (bar && u->menus[0].count) open_bar(u, (u->bar_index + u->menus[0].count - 1u) % u->menus[0].count);
        return 1;
    case VDS_VK_RETURN: case VDS_VK_KP_ENTER: activate(u, level); return 1;
    case VDS_VK_ESCAPE:
        close_from(u, level);
        if (!level && bar) render_bar(u);
        return 1;
    case VDS_VK_BACKSPACE:
        if (u->palette && u->filter_len) { u->filter[--u->filter_len] = 0; reopen_palette(u); }
        return 1;
    }
    if (modifiers & (VDS_MOD_CTRL | VDS_MOD_LALT | VDS_MOD_RALT) || ascii < 32 || ascii > 126) return 1;
    if (u->palette && level == 0) {
        if (u->filter_len < VDS_FILTER_MAX) { u->filter[u->filter_len++] = (char)ascii; u->filter[u->filter_len] = 0; reopen_palette(u); }
        return 1;
    }
    for (unsigned i = 0; i < om->count; ++i)                 /* mnemonic letter */
        if (om->items[i] != VDS_ITEM_SEPARATOR && mnemonic_of(item_label(u, om->items[i])) == lower(ascii)) {
            om->sel = (uint8_t)i;
            render_popup(u, level);
            activate(u, level);
            break;
        }
    return 1;
}

static int accelerator(vds_ui *u, uint8_t ascii, uint8_t modifiers, uint8_t vk) {
    uint8_t mods = modifiers & (VDS_MOD_CTRL | VDS_MOD_SHIFT | VDS_MOD_LALT | VDS_MOD_RALT);
    if (mods & VDS_MOD_RALT) mods = (uint8_t)((mods & ~VDS_MOD_RALT) | VDS_MOD_LALT);
    if ((mods & VDS_MOD_CTRL) && ascii >= 1 && ascii <= 26) ascii = (uint8_t)(ascii + 'a' - 1);
    for (unsigned i = 0; i < VDS_MAX_COMMANDS; ++i) {
        const vds_command *c = &u->commands[i];
        if ((c->flags & (VDS_CMD_USED | VDS_CMD_ENABLED)) != (VDS_CMD_USED | VDS_CMD_ENABLED)) continue;
        uint8_t want = c->mods & (VDS_MOD_CTRL | VDS_MOD_SHIFT | VDS_MOD_LALT | VDS_MOD_RALT);
        if (want & VDS_MOD_RALT) want = (uint8_t)((want & ~VDS_MOD_RALT) | VDS_MOD_LALT);
        int key = c->vk ? c->vk == vk : (c->ascii && lower(c->ascii) == lower(ascii));
        if (key && want == mods) { emit_command(u, c->id, VDS_FROM_ACCELERATOR); return 1; }
    }
    return 0;
}

/* ------------------------------------------------------------ animation */

/* Eased progress for t in 0..65536, as 16.16 fixed point (spring overshoots). */
static int32_t ease(uint8_t easing, int32_t t) {
    switch (easing) {
    case VDS_EASE_IN_OUT: return (int32_t)(((int64_t)t * t >> 16) * (3 * 65536 - 2 * t) >> 16);
    case VDS_EASE_OUT: { int32_t r = 65536 - t; return 65536 - (int32_t)((int64_t)r * r >> 16); }
    case VDS_EASE_SPRING: {                                 /* back-out: 1 + c3 u^3 + c1 u^2, u = t - 1 */
        int64_t q = t - 65536, q2 = q * q >> 16, q3 = q2 * q >> 16;
        return (int32_t)(65536 + (177050 * q3 >> 16) + (111514 * q2 >> 16));
    }
    }
    return t;
}

static int tween_target_ok(vds_ui *u, uint8_t target, uint8_t id) {
    vds *v = core(u);
    if (target == VDS_TWEEN_WINDOW) return id < VDS_UI_FIRST_WINDOW && v->windows[id].used;
    if (target == VDS_TWEEN_SPRITE) return id < VDS_MAX_SPRITES && v->sprites[id].used;
    return 0;
}

static void tween_apply(vds_ui *u, const vds_tween *t, int32_t value) {
    vds *v = core(u);
    if (value < -32768) value = -32768;
    if (value > 32767) value = 32767;
    if (t->target == VDS_TWEEN_SPRITE) {
        const vds_sprite *s = &v->sprites[t->id];
        vds_sprite_move(v, t->id, t->prop == VDS_PROP_X ? (int16_t)value : s->x, t->prop == VDS_PROP_Y ? (int16_t)value : s->y);
        return;
    }
    const vds_window *w = &v->windows[t->id];
    if (t->prop == VDS_PROP_X || t->prop == VDS_PROP_Y)
        vds_window_move(v, t->id, t->prop == VDS_PROP_X ? (int16_t)value : w->x, t->prop == VDS_PROP_Y ? (int16_t)value : w->y);
    else {
        uint8_t n = (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
        vds_window_scroll(v, t->id, t->prop == VDS_PROP_SCROLL_COL ? n : w->scroll_col, t->prop == VDS_PROP_SCROLL_ROW ? n : w->scroll_row);
    }
}

static void tween_done(vds_ui *u, vds_tween *t) {
    uint8_t args[2] = {t->target, t->prop};
    t->used = 0;
    vds_proto_event(u->p, VDS_EVENT_ANIMDONE, t->id, args, sizeof args);
}

int vds_ui_animate(vds_ui *u, uint8_t target, uint8_t id, uint8_t prop, int16_t from, int16_t to, uint16_t ms, uint8_t easing) {
    if (!tween_target_ok(u, target, id) || prop > VDS_PROP_SCROLL_COL || easing > VDS_EASE_SPRING ||
        (target == VDS_TWEEN_SPRITE && prop > VDS_PROP_Y)) return 0;
    vds_tween *slot = 0;
    for (unsigned i = 0; i < VDS_MAX_TWEENS && !slot; ++i)
        if (u->tweens[i].used && u->tweens[i].target == target && u->tweens[i].id == id && u->tweens[i].prop == prop) slot = &u->tweens[i];
    for (unsigned i = 0; i < VDS_MAX_TWEENS && !slot; ++i)
        if (!u->tweens[i].used) slot = &u->tweens[i];
    if (!slot) return 0;
    vds_tween t = {1, target, id, prop, easing, from, to, ms, 0};
    *slot = t;
    tween_apply(u, slot, ms ? from : to);
    if (!ms) tween_done(u, slot);
    return 1;
}

int vds_ui_stop(vds_ui *u, uint8_t target, uint8_t id, uint8_t prop) {
    for (unsigned i = 0; i < VDS_MAX_TWEENS; ++i)
        if (u->tweens[i].used && u->tweens[i].target == target && u->tweens[i].id == id && u->tweens[i].prop == prop) {
            u->tweens[i].used = 0;
            return 1;
        }
    return 0;
}

void vds_ui_tick(vds_ui *u, uint32_t ms) {
    for (unsigned i = 0; i < VDS_MAX_TWEENS; ++i) {
        vds_tween *t = &u->tweens[i];
        if (!t->used) continue;
        if (!tween_target_ok(u, t->target, t->id)) { t->used = 0; continue; }   /* its object went away */
        uint32_t e = t->elapsed + ms;
        t->elapsed = (uint16_t)(e > t->ms ? t->ms : e);
        int32_t s = ease(t->easing, (int32_t)((int64_t)t->elapsed * 65536 / t->ms));
        tween_apply(u, t, t->from + (int32_t)((int64_t)(t->to - t->from) * s >> 16));
        if (t->elapsed >= t->ms) tween_done(u, t);
    }
}

/* ------------------------------------------------------------ pointer */

void vds_ui_pointer(vds_ui *u, int16_t x, int16_t y, uint8_t buttons) {
    vds *v = core(u);
    if (!v->sprites[VDS_CURSOR_SPRITE].used)
        vds_sprite_define(v, VDS_CURSOR_SPRITE, VDS_ARROW_W, VDS_ARROW_H, 0, 0, VDS_ARROW_TRANSPARENT, vds_arrow_cursor);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= v->width) x = (int16_t)(v->width - 1);
    if (y >= v->height) y = (int16_t)(v->height - 1);
    vds_sprite_move(v, VDS_CURSOR_SPRITE, x, y);
    vds_sprite_show(v, VDS_CURSOR_SPRITE, 1);
    u->px = x;
    u->py = y;
    if (buttons == u->buttons) return;
    uint8_t part = 0, id = vds_hit(v, x, y, &part);
    int press = (buttons & 1) && !(u->buttons & 1);
    u->buttons = buttons;
    if (press && id != VDS_NONE && focusable(v, id) && id != u->focus) vds_ui_focus(u, id);
    if (press && id != VDS_NONE && part == VDS_PART_CLIENT && id == u->focus)
        vds_ui_widget_click(u, id, (x - v->windows[id].x) / 8 + v->windows[id].scroll_col, (y - v->windows[id].y) / 8 + v->windows[id].scroll_row);
    int lx = id == VDS_NONE ? x : x - v->windows[id].x, ly = id == VDS_NONE ? y : y - v->windows[id].y;
    uint8_t args[7] = {(uint8_t)x, (uint8_t)((uint16_t)x >> 8), (uint8_t)y, (uint8_t)((uint16_t)y >> 8),
                       (uint8_t)(part == VDS_PART_CLIENT ? lx / 8 + v->windows[id].scroll_col : 0),
                       (uint8_t)(part == VDS_PART_CLIENT ? ly / 8 + v->windows[id].scroll_row : 0), part};
    uint8_t all[8];
    memcpy(all, args, 7);
    all[7] = buttons;
    vds_proto_event(u->p, VDS_EVENT_POINTER, id, all, sizeof all);
}

static int pointer_key(vds_ui *u, uint8_t ascii, uint8_t modifiers, uint8_t vk) {
    int step = (modifiers & VDS_MOD_SHIFT) ? 1 : 8;
    switch (vk) {
    case VDS_VK_LEFT: case VDS_VK_KP_LEFT: vds_ui_pointer(u, (int16_t)(u->px - step), u->py, u->buttons); return 1;
    case VDS_VK_RIGHT: case VDS_VK_KP_RIGHT: vds_ui_pointer(u, (int16_t)(u->px + step), u->py, u->buttons); return 1;
    case VDS_VK_UP: case VDS_VK_KP_UP: vds_ui_pointer(u, u->px, (int16_t)(u->py - step), u->buttons); return 1;
    case VDS_VK_DOWN: case VDS_VK_KP_DOWN: vds_ui_pointer(u, u->px, (int16_t)(u->py + step), u->buttons); return 1;
    case VDS_VK_ESCAPE:
        u->pointer_mode = 0;
        vds_sprite_show(core(u), VDS_CURSOR_SPRITE, 0);
        return 1;
    case VDS_VK_RETURN: case VDS_VK_KP_ENTER: ascii = ' '; break;
    }
    if (ascii == ' ') {                                     /* click: press then release */
        vds_ui_pointer(u, u->px, u->py, (uint8_t)(u->buttons | 1));
        vds_ui_pointer(u, u->px, u->py, (uint8_t)(u->buttons & ~1));
    }
    return 1;
}

/* ------------------------------------------------------------ routing */

static int route(vds_ui *u, uint8_t ascii, uint8_t modifiers, uint8_t vk) {
    vds *v = core(u);
    int ctrl = (modifiers & VDS_MOD_CTRL) != 0, shift = (modifiers & VDS_MOD_SHIFT) != 0;
    int alt = (modifiers & (VDS_MOD_LALT | VDS_MOD_RALT)) != 0;
    if (u->focus != VDS_NONE && !focusable(v, u->focus)) vds_ui_forget(u, u->focus);
    if (u->depth) return menu_key(u, ascii, modifiers, vk);
    if (vk == VDS_VK_F12 && ctrl) {                         /* keyboard pointer on/off */
        u->pointer_mode = !u->pointer_mode;
        if (u->pointer_mode) vds_ui_pointer(u, u->px, u->py, u->buttons);
        else vds_sprite_show(v, VDS_CURSOR_SPRITE, 0);
        return 1;
    }
    if (u->pointer_mode) return pointer_key(u, ascii, modifiers, vk);
    if (u->wm_mode) return wm_key(u, modifiers, vk);
    if (vk == VDS_VK_F10 && !shift && !ctrl) {
        if (u->bar_shown) return open_bar(u, 0);
        if (u->menus[0].used) { open_level(u, 0, 16, 16); return 1; }
        return 0;
    }
    if (alt && !ctrl && u->bar_shown && ascii > 32 && ascii < 127)
        for (unsigned i = 0; i < u->menus[0].count; ++i)
            if (mnemonic_of(item_label(u, u->menus[0].items[i])) == lower(ascii)) return open_bar(u, i);
    if (vk == VDS_VK_F10 && shift && !ctrl) {
        uint8_t menu = u->focus != VDS_NONE && u->window_menu[u->focus] != VDS_NONE ? u->window_menu[u->focus] : 0;
        if (!u->menus[menu].used) return 0;
        int x = 16, y = 16;
        if (u->focus != VDS_NONE) { x = v->windows[u->focus].x + 16; y = v->windows[u->focus].y + 8; }
        open_level(u, menu, x, y);
        return 1;
    }
    uint8_t letter = ctrl && ascii >= 1 && ascii <= 26 ? (uint8_t)(ascii + 'a' - 1) : lower(ascii);
    if (ctrl && !alt && ((shift && letter == 'p') || (!shift && letter == 'k'))) {
        u->palette = 1;
        u->filter_len = 0;
        u->filter[0] = 0;
        open_level(u, 0, ((v->width - (4 + VDS_FILTER_MAX) * 8) / 2) & ~7, 40);
        return 1;
    }
    if (accelerator(u, ascii, modifiers, vk)) return 1;
    if (vk == VDS_VK_F6) return cycle(u, shift);
    if (u->focus == VDS_NONE) return 0;
    if (vk == VDS_VK_F5 && ctrl) {
        u->wm_saved = geometry_of(v, u->focus);
        u->wm_mode = 1;
        return 1;
    }
    if (vk == VDS_VK_F5) { zoom(u); return 1; }
    if ((vk == VDS_VK_F3 && alt) || (ctrl && letter == 'w')) {
        win_event(u, VDS_WIN_CLOSE, u->focus);
        return 1;
    }
    return vds_ui_widget_key(u, ascii, modifiers, vk);
}

int vds_ui_key(vds_ui *u, uint8_t ascii, uint8_t modifiers, uint8_t vk, uint8_t down) {
    if (!down) {                                            /* a consumed press keeps its release */
        if (u->held && vk == u->held_vk && (vk || ascii == u->held_ascii)) { u->held = 0; return 1; }
        return u->wm_mode || u->depth || u->pointer_mode;
    }
    int used = route(u, ascii, modifiers, vk);
    u->held = (uint8_t)used;
    u->held_vk = vk;
    u->held_ascii = ascii;
    return used;
}

/* ------------------------------------------------------------ protocol hooks */

static int focus_hook(void *ui, uint8_t id) { return vds_ui_focus((vds_ui *)ui, id); }
static void forget_hook(void *ui, uint8_t id) { vds_ui_forget((vds_ui *)ui, id); }
static void after_hook(void *ui) {                     /* only a style change re-decorates */
    vds_ui *u = (vds_ui *)ui;
    if (u->style_seen != core(u)->style.kind) {
        vds_ui_refresh(u);
        for (uint8_t id = 0; id < VDS_UI_FIRST_WINDOW; ++id)
            if (core(u)->windows[id].used && has_widgets(u, id)) vds_ui_widgets_refresh(u, id);
    }
    widgets_check(u);
}

/* The protocol reset already closed every window: drop menus and commands too. */
static void reset_hook(void *ui) {
    vds_ui *u = (vds_ui *)ui;
    vds_proto *p = u->p;
    vds_ui_init(u, p);
}

static int op_hook(void *ui, const uint8_t *b) {
    vds_ui *u = (vds_ui *)ui;
    switch (b[0]) {
    case 0x59: return vds_ui_window_menu(u, b[1], b[2]);
    case 0x60: {
        char label[VDS_LABEL_MAX];
        size_t n = b[7] < VDS_LABEL_MAX - 1 ? b[7] : VDS_LABEL_MAX - 1;
        memcpy(label, b + 8, n);
        label[n] = 0;
        return vds_ui_command(u, (uint16_t)(b[1] | b[2] << 8), b[3], b[4], b[5], b[6], label);
    }
    case 0x61: {
        char title[16];
        size_t tn = b[2], n = tn < sizeof title - 1 ? tn : sizeof title - 1;
        uint16_t items[VDS_MENU_ITEMS];
        uint8_t count = b[3 + tn];
        if (count > VDS_MENU_ITEMS) return 0;
        memcpy(title, b + 3, n);
        title[n] = 0;
        for (unsigned i = 0; i < count; ++i) items[i] = (uint16_t)(b[4 + tn + 2 * i] | b[5 + tn + 2 * i] << 8);
        return vds_ui_menu(u, b[1], title, items, count);
    }
    case 0x62: return vds_ui_command_state(u, (uint16_t)(b[1] | b[2] << 8), b[3]);
    case 0x70:
        return vds_ui_animate(u, b[1], b[2], b[3], (int16_t)(uint16_t)(b[4] | b[5] << 8), (int16_t)(uint16_t)(b[6] | b[7] << 8),
                              (uint16_t)(b[8] | b[9] << 8), b[10]);
    case 0x71: return vds_ui_stop(u, b[1], b[2], b[3]);
    case 0x7B: vds_ui_pointer(u, (int16_t)(uint16_t)(b[1] | b[2] << 8), (int16_t)(uint16_t)(b[3] | b[4] << 8), b[5]); return 1;
    case 0x80: return vds_ui_widget(u, b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9]);
    case 0x81: return vds_ui_widget_text(u, b[1], (const char *)b + 3, b[2]);
    case 0x82: return vds_ui_widget_value(u, b[1], (int16_t)(uint16_t)(b[2] | b[3] << 8));
    case 0x83: return vds_ui_widget_layout(u, b[1], b[2], b[3]);
    case 0x84: return vds_ui_widget_size(u, b[1], b[2], b[3]);
    case 0x85: return vds_ui_widget_item(u, b[1], b[2], (const char *)b + 4, b[3]);
    case 0x86: return vds_ui_widget_count(u, b[1], b[2]);
    case 0x87: return vds_ui_widget_delete(u, b[1]);
    case 0x88: return vds_ui_widget_focus(u, b[1]);
    case 0x89: {
        if (b[1] >= VDS_MAX_WIDGETS || !u->widgets[b[1]].used) return 0;
        const vds_widget *w = &u->widgets[b[1]];
        uint8_t r[VDS_REPLY_MAX] = {0x89, b[1], (uint8_t)w->value, (uint8_t)((uint16_t)w->value >> 8)};
        size_t n = strlen(w->text);
        if (n > VDS_REPLY_MAX - 5) n = VDS_REPLY_MAX - 5;
        r[4] = (uint8_t)n;
        memcpy(r + 5, w->text, n);
        vds_proto_reply(u->p, r, (uint8_t)(5 + n));
        return 1;
    }
    }
    return 0;
}

static void tick_hook(void *ui, uint32_t ms) { vds_ui_tick((vds_ui *)ui, ms); }

void vds_ui_init(vds_ui *u, vds_proto *p) {
    memset(u, 0, sizeof *u);
    u->p = p;
    u->focus = VDS_NONE;
    u->bar_index = VDS_NONE;
    memset(u->window_menu, VDS_NONE, sizeof u->window_menu);
    memset(u->widget_focus, VDS_NONE, sizeof u->widget_focus);
    u->caret_widget = VDS_NONE;
    p->ui = u;
    p->ui_focus = focus_hook;
    p->ui_forget = forget_hook;
    p->ui_op = op_hook;
    p->ui_after = after_hook;
    p->ui_reset = reset_hook;
    p->ui_tick = tick_hook;
}

/* VIOS Display Server widgets (G4): see vds_ui.h. Widgets render into their
 * window's surface (coordinates are surface cells), take keys and clicks
 * locally, and tell the CPU what happened with ACTIVATE / SELECT / CHANGE. */
#include "vds_ui.h"
#include <stddef.h>
#include <string.h>

static vds *core(vds_ui *u) { return u->p->v; }
static uint8_t attr(uint8_t fg, uint8_t bg) { return (uint8_t)(fg | bg << 4); }
static uint8_t lower(uint8_t c) { return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c; }

static vds_widget *widget(vds_ui *u, uint8_t id) {
    return id < VDS_MAX_WIDGETS && u->widgets[id].used ? &u->widgets[id] : 0;
}

static int shown(vds_ui *u, const vds_widget *w) {             /* it and every ancestor visible */
    for (unsigned depth = 0; w && depth < VDS_MAX_WIDGETS; ++depth) {
        if (w->flags & VDS_WF_HIDDEN) return 0;
        w = w->parent == VDS_NONE ? 0 : widget(u, w->parent);
    }
    return 1;
}

static int interactive(uint8_t kind) {
    return kind == VDS_WK_BUTTON || kind == VDS_WK_CHECK || kind == VDS_WK_RADIO || kind == VDS_WK_INPUT || kind == VDS_WK_LIST ||
           kind == VDS_WK_TEXT || kind == VDS_WK_TABS;
}

/* Tabs: label k of the '|'-separated widget text; returns its length. */
static size_t tab_label(const vds_widget *w, unsigned k, const char **at) {
    const char *t = w->text;
    for (; k && *t; ++t) if (*t == '|') --k;
    if (k) { *at = t; return 0; }
    size_t n = 0;
    while (t[n] && t[n] != '|') ++n;
    *at = t;
    return n;
}
static unsigned tab_count(const vds_widget *w) {
    unsigned n = 1;
    for (const char *t = w->text; *t; ++t) n += *t == '|';
    return w->text[0] ? n : 0;
}

static int focusable(vds_ui *u, const vds_widget *w) {
    return w->used && interactive(w->kind) && !(w->flags & VDS_WF_DISABLED) && shown(u, w) && w->w && w->h;
}

static void event(vds_ui *u, uint8_t kind, uint8_t id, int16_t value, int with_value) {
    uint8_t args[3] = {(uint8_t)value, (uint8_t)((uint16_t)value >> 8), u->widgets[id].window};
    if (with_value) vds_proto_event(u->p, kind, id, args, 3);
    else vds_proto_event(u->p, kind, id, args + 2, 1);
}

/* ------------------------------------------------------------ order */

/* The next used widget after `after` in creation order (VDS_NONE: the first)
 * that belongs to window `window` and, unless parent is 0xFE, has that parent. */
#define ANY_PARENT 0xFE
static uint8_t next_widget(vds_ui *u, uint8_t window, uint8_t parent, uint8_t after) {
    uint16_t floor = after == VDS_NONE ? 0 : (uint16_t)(u->widgets[after].seq + 1);
    uint8_t best = VDS_NONE;
    for (unsigned i = 0; i < VDS_MAX_WIDGETS; ++i) {
        const vds_widget *c = &u->widgets[i];
        if (!c->used || c->window != window || (parent != ANY_PARENT && c->parent != parent) || c->seq < floor) continue;
        if (best == VDS_NONE || c->seq < u->widgets[best].seq) best = (uint8_t)i;
    }
    return best;
}

/* ------------------------------------------------------------ layout */

static void layout_box(vds_ui *u, uint8_t box, unsigned depth) {
    vds_widget *b = &u->widgets[box];
    if (b->layout == VDS_LAYOUT_NONE || depth > 8) return;
    unsigned n = 0, fixed = 0, weights = 0, main = b->layout == VDS_LAYOUT_COLUMN ? b->h : b->w;
    for (uint8_t i = next_widget(u, b->window, box, VDS_NONE); i != VDS_NONE; i = next_widget(u, b->window, box, i)) {
        const vds_widget *c = &u->widgets[i];
        if (c->flags & VDS_WF_HIDDEN) continue;
        ++n;
        if (c->fixed) fixed += c->fixed; else weights += c->weight ? c->weight : 1u;
    }
    if (!n) return;
    unsigned gaps = b->gap * (n - 1) <= main ? b->gap * (n - 1) : 0;
    unsigned room = main > gaps + fixed ? main - gaps - fixed : 0, given = 0, seen = 0, weighted = 0, pos = 0;
    for (uint8_t i = next_widget(u, b->window, box, VDS_NONE); i != VDS_NONE; i = next_widget(u, b->window, box, i)) {
        vds_widget *c = &u->widgets[i];
        if (c->flags & VDS_WF_HIDDEN) continue;
        ++seen;
        unsigned size;
        if (c->fixed) size = c->fixed;
        else {
            unsigned wt = c->weight ? c->weight : 1u;
            weighted += wt;
            size = weighted == weights ? room - given : room * wt / weights;   /* remainder to the last one */
            given += size;
        }
        if (pos + size > main) size = pos < main ? main - pos : 0;
        if (b->layout == VDS_LAYOUT_COLUMN) { c->col = b->col; c->w = b->w; c->row = (uint8_t)(b->row + pos); c->h = (uint8_t)size; }
        else { c->row = b->row; c->h = b->h; c->col = (uint8_t)(b->col + pos); c->w = (uint8_t)size; }
        pos += size + (seen < n ? (gaps ? b->gap : 0) : 0);
        if (c->kind == VDS_WK_BOX) layout_box(u, i, depth + 1);
    }
}

static void layout_window(vds_ui *u, uint8_t window) {
    vds *v = core(u);
    for (unsigned i = 0; i < VDS_MAX_WIDGETS; ++i) {
        vds_widget *w = &u->widgets[i];
        if (!w->used || w->window != window || w->parent != VDS_NONE || w->kind != VDS_WK_BOX) continue;
        if (w->flags & VDS_WF_FILL) { w->col = w->row = 0; w->w = v->windows[window].cols; w->h = v->windows[window].rows; }
        layout_box(u, (uint8_t)i, 0);
    }
    u->widget_cols[window] = v->windows[window].cols;
    u->widget_rows[window] = v->windows[window].rows;
}

/* ------------------------------------------------------------ rendering */

static uint8_t surface_of(vds_ui *u, uint8_t window) {
    vds *v = core(u);
    if (window >= VDS_MAX_WINDOWS || !v->windows[window].used) return VDS_NONE;
    uint8_t s = v->windows[window].surface;
    return s < VDS_MAX_SURFACES && v->surfaces[s].cells ? s : VDS_NONE;
}

/* Text without '&', mnemonic position in *m (or -1). */
static size_t plain(const char *text, char *out, size_t max, int *m) {
    size_t n = 0;
    *m = -1;
    for (; *text && n < max; ++text) {
        if (*text == '&' && text[1] && text[1] != '&') { *m = (int)n; continue; }
        if (*text == '&' && text[1] == '&') ++text;        /* "&&" is a literal '&'; a lone trailing '&' too */
        out[n++] = *text;
    }
    return n;
}

static void put(vds *v, uint8_t s, int col, int row, int width, uint8_t a, const char *text, size_t n) {
    if (col < 0 || row < 0 || width <= 0) return;
    if (n > (size_t)width) n = (size_t)width;
    vds_fill(v, s, (uint8_t)col, (uint8_t)row, (uint8_t)width, 1, ' ', a);
    if (n) vds_put(v, s, (uint8_t)col, (uint8_t)row, a, text, n);
}

static void caret_off(vds_ui *u);

/* One list row: plain text, or table cells split at '\t' into column widths. */
static void row_text(vds *v, uint8_t s, const vds_widget *w, int row, uint8_t a, const char *t) {
    char line[VDS_MAX_WIDTH / 8];
    int width = w->w, k = 0;
    if (width > (int)sizeof line) width = (int)sizeof line;
    line[k++] = ' ';
    if (!w->ncols) {
        while (*t && k < width) line[k++] = *t++;
    } else {
        int start = 1;
        for (unsigned c = 0; c < w->ncols && k < width; ++c) {
            int end = start + w->cols[c];
            while (*t && *t != '\t' && k < end - 1 && k < width) line[k++] = *t++;   /* one blank between cells */
            while (*t && *t != '\t') ++t;              /* clipped */
            if (*t == '\t') ++t;
            while (k < end && k < width) line[k++] = ' ';
            start = end;
        }
    }
    vds_fill(v, s, w->col, (uint8_t)row, w->w, 1, ' ', a);
    vds_put(v, s, w->col, (uint8_t)row, a, line, (size_t)k);
}

static void render(vds_ui *u, uint8_t id) {
    vds *v = core(u);
    vds_widget *w = &u->widgets[id];
    uint8_t s = surface_of(u, w->window);
    if (s == VDS_NONE || w->kind == VDS_WK_BOX || !w->w || !w->h || !shown(u, w)) return;
    const vds_style *st = &v->style;
    int focused = u->widget_focus[w->window] == id && u->focus == w->window, off = (w->flags & VDS_WF_DISABLED) != 0;
    uint8_t base = attr(st->title_fg, st->body), sel = attr(st->menu_sel_fg, st->menu_sel_bg);
    uint8_t button = off ? attr(st->menu_disabled, st->menu_bg) : focused ? sel : attr(st->menu_fg, st->menu_bg);
    char text[VDS_WIDGET_TEXT + 8], line[VDS_WIDGET_TEXT + 8];
    int m;
    size_t n = plain(w->text, text, VDS_WIDGET_TEXT, &m);
    if (w->kind == VDS_WK_TEXT) { vds_ui_text_render(u, id); return; }
    if (w->h > 1 && w->kind != VDS_WK_LIST) vds_fill(v, s, w->col, (uint8_t)(w->row + 1), w->w, (uint8_t)(w->h - 1), ' ', base);
    switch (w->kind) {
    case VDS_WK_LABEL:
        put(v, s, w->col, w->row, w->w, off ? attr(st->menu_disabled, st->body) : base, text, n);
        break;
    case VDS_WK_BUTTON: {
        size_t k = 0;
        line[k++] = '['; line[k++] = ' ';
        memcpy(line + k, text, n); k += n;
        line[k++] = ' '; line[k++] = ']';
        int pad = w->w > (int)k ? (w->w - (int)k) / 2 : 0;               /* centred */
        vds_fill(v, s, w->col, w->row, w->w, 1, ' ', button);
        put(v, s, w->col + pad, w->row, w->w - pad, button, line, k);
        if (m >= 0 && !off && !focused && 2 + m + pad < w->w)
            vds_put(v, s, (uint8_t)(w->col + pad + 2 + m), w->row, attr(st->menu_key, st->menu_bg), text + m, 1);
        break;
    }
    case VDS_WK_CHECK: case VDS_WK_RADIO: {
        int check = w->kind == VDS_WK_CHECK;
        line[0] = check ? '[' : '('; line[1] = w->value ? (check ? 'x' : '*') : ' '; line[2] = check ? ']' : ')'; line[3] = ' ';
        memcpy(line + 4, text, n);
        put(v, s, w->col, w->row, w->w, off ? attr(st->menu_disabled, st->body) : focused ? sel : base, line, n + 4);
        break;
    }
    case VDS_WK_INPUT: {
        size_t len = strlen(w->text);
        int caret = w->value < 0 ? 0 : w->value > (int)len ? (int)len : w->value;
        if (caret < w->top) w->top = (uint8_t)caret;
        if (caret >= w->top + w->w) w->top = (uint8_t)(caret - w->w + 1);
        put(v, s, w->col, w->row, w->w, attr(0, off ? st->menu_bg : 3), w->text + w->top, len > w->top ? len - w->top : 0);
        if (focused) { vds_cursor(v, s, (uint8_t)(w->col + caret - w->top), w->row, VDS_CURSOR_UNDERLINE, 1); u->caret_widget = id; }
        else if (u->caret_widget == id) caret_off(u);
        break;
    }
    case VDS_WK_LIST: {
        int header = w->ncols && w->text[0], rows = w->h - header;
        if (w->value >= (int)w->count) w->value = w->count ? (int16_t)(w->count - 1) : 0;
        if (w->value < 0) w->value = 0;
        if (rows > 0 && w->value < w->top) w->top = (uint16_t)w->value;
        if (rows > 0 && w->value >= w->top + rows) w->top = (uint16_t)(w->value - rows + 1);
        if (w->top && w->top + rows > w->count) w->top = (uint16_t)(w->count > rows ? w->count - rows : 0);
        if (header) row_text(v, s, w, w->row, attr(st->menu_fg, st->menu_bg), w->text);
        for (int r = 0; r < rows; ++r) {
            int item = w->top + r;
            int chosen = item == w->value && item < w->count;
            uint8_t a = chosen ? (focused ? sel : attr(st->menu_fg, st->desktop_a)) : base;   /* unfocused: a grey bar */
            row_text(v, s, w, w->row + header + r, a, item < w->count ? u->items[w->first + item] : "");
        }
        break;
    }
    case VDS_WK_TABS: {
        unsigned n = tab_count(w), at = w->col;
        vds_fill(v, s, w->col, w->row, w->w, 1, ' ', base);
        for (unsigned k = 0; k < n && at < (unsigned)(w->col + w->w); ++k) {
            const char *label;
            size_t len = tab_label(w, k, &label);
            uint8_t a = (int)k == w->value ? (focused ? sel : attr(st->menu_fg, st->desktop_a)) : attr(st->menu_fg, st->menu_bg);
            line[0] = ' ';
            if (len > VDS_WIDGET_TEXT) len = VDS_WIDGET_TEXT;
            memcpy(line + 1, label, len);
            line[len + 1] = ' ';
            size_t room = (unsigned)(w->col + w->w) - at;
            vds_put(v, s, (uint8_t)at, w->row, a, line, len + 2 < room ? len + 2 : room);
            at += (unsigned)len + 3;
        }
        break;
    }
    case VDS_WK_PROGRESS: {
        int value = w->value < 0 ? 0 : w->value > 100 ? 100 : w->value, filled = w->w * value / 100;
        char pct[6];
        size_t k = 0;
        if (value >= 100) pct[k++] = '1';
        if (value >= 10) pct[k++] = (char)('0' + value / 10 % 10);
        pct[k++] = (char)('0' + value % 10);
        pct[k++] = '%';
        vds_fill(v, s, w->col, w->row, w->w, 1, ' ', base);
        if (filled) vds_fill(v, s, w->col, w->row, (uint8_t)filled, 1, ' ', sel);
        int at = (w->w - (int)k) / 2;
        for (size_t i = 0; i < k && at + (int)i < w->w; ++i)
            vds_put(v, s, (uint8_t)(w->col + at + i), w->row, at + (int)i < filled ? sel : base, pct + i, 1);
        break;
    }
    }
}

static void caret_off(vds_ui *u) {
    if (u->caret_widget == VDS_NONE) return;
    vds_widget *w = widget(u, u->caret_widget);
    uint8_t s = w ? surface_of(u, w->window) : VDS_NONE;
    if (s != VDS_NONE) vds_cursor(core(u), s, 0, 0, VDS_CURSOR_OFF, 0);
    u->caret_widget = VDS_NONE;
}

void vds_ui_widgets_refresh(vds_ui *u, uint8_t window) {
    if (window >= VDS_MAX_WINDOWS) return;
    layout_window(u, window);
    uint8_t f = u->widget_focus[window];
    if (f == VDS_NONE || !u->widgets[f].used || u->widgets[f].window != window || !focusable(u, &u->widgets[f])) {
        u->widget_focus[window] = VDS_NONE;                      /* first focusable in creation order */
        for (uint8_t i = next_widget(u, window, ANY_PARENT, VDS_NONE); i != VDS_NONE; i = next_widget(u, window, ANY_PARENT, i))
            if (focusable(u, &u->widgets[i])) { u->widget_focus[window] = i; break; }
    }
    /* A laid-out box owns its area: clear it so gaps between children never
     * keep text from an earlier layout, then draw the children. */
    uint8_t s = surface_of(u, window);
    for (unsigned i = 0; s != VDS_NONE && i < VDS_MAX_WIDGETS; ++i) {
        const vds_widget *b = &u->widgets[i];
        if (b->used && b->window == window && b->kind == VDS_WK_BOX && b->layout && b->w && b->h && shown(u, b))
            vds_fill(core(u), s, b->col, b->row, b->w, b->h, ' ', attr(core(u)->style.title_fg, core(u)->style.body));
    }
    for (unsigned i = 0; i < VDS_MAX_WIDGETS; ++i)
        if (u->widgets[i].used && u->widgets[i].window == window) render(u, (uint8_t)i);
}

/* ------------------------------------------------------------ definition */

static void redraw_window_of(vds_ui *u, uint8_t id) { vds_ui_widgets_refresh(u, u->widgets[id].window); }

int vds_ui_widget(vds_ui *u, uint8_t id, uint8_t window, uint8_t kind, uint8_t parent,
                  uint8_t col, uint8_t row, uint8_t w, uint8_t h, uint8_t flags) {
    vds *v = core(u);
    if (id >= VDS_MAX_WIDGETS || window >= VDS_UI_FIRST_WINDOW || !v->windows[window].used ||
        kind < VDS_WK_BOX || kind > VDS_WK_TABS) return 0;
    /* Replace first: the new widget then has no children, so whatever parent
     * survives the deletion cannot be one of its descendants (no cycles). */
    if (u->widgets[id].used) vds_ui_widget_delete(u, id);
    if (parent != VDS_NONE && (parent == id || !widget(u, parent) || u->widgets[parent].window != window ||
                               u->widgets[parent].kind != VDS_WK_BOX)) return 0;
    vds_widget *x = &u->widgets[id];
    memset(x, 0, sizeof *x);
    x->used = 1; x->window = window; x->kind = kind; x->parent = parent; x->flags = flags;
    x->col = col; x->row = row; x->w = w; x->h = h; x->weight = 1; x->slot = VDS_NONE;
    if (kind == VDS_WK_TEXT) {                                  /* a free text buffer, else refused */
        for (uint8_t i = 0; i < VDS_TEXT_SLOTS && x->slot == VDS_NONE; ++i)
            if (!u->texts[i].used) { memset(&u->texts[i], 0, offsetof(vds_text, buf)); u->texts[i].used = 1; u->texts[i].widget = id; x->slot = i; }
        if (x->slot == VDS_NONE) { x->used = 0; return 0; }
    }
    x->seq = u->widget_seq++;
    if (!u->widget_seq) {                                       /* wrapped: renumber in order */
        uint16_t n = 0;
        for (unsigned pass = 0; pass < VDS_MAX_WIDGETS; ++pass) {
            int low = -1;
            for (unsigned i = 0; i < VDS_MAX_WIDGETS; ++i)
                if (u->widgets[i].used && u->widgets[i].seq >= n && (low < 0 || u->widgets[i].seq < u->widgets[low].seq)) low = (int)i;
            if (low < 0) break;
            u->widgets[low].seq = n++;
        }
        u->widget_seq = n;
    }
    redraw_window_of(u, id);
    return 1;
}

int vds_ui_widget_text(vds_ui *u, uint8_t id, const char *text, size_t n) {
    vds_widget *w = widget(u, id);
    if (!w) return 0;
    if (n >= VDS_WIDGET_TEXT) n = VDS_WIDGET_TEXT - 1;
    memcpy(w->text, text, n);
    w->text[n] = 0;
    if (w->kind == VDS_WK_INPUT && w->value > (int16_t)n) w->value = (int16_t)n;
    render(u, id);
    return 1;
}

static void select_radio(vds_ui *u, uint8_t id) {
    vds_widget *w = &u->widgets[id];
    for (unsigned i = 0; i < VDS_MAX_WIDGETS; ++i) {               /* one per group (same parent) */
        vds_widget *o = &u->widgets[i];
        if (o->used && i != id && o->kind == VDS_WK_RADIO && o->window == w->window && o->parent == w->parent && o->value) {
            o->value = 0;
            render(u, (uint8_t)i);
        }
    }
    w->value = 1;
    render(u, id);
}

int vds_ui_widget_value(vds_ui *u, uint8_t id, int16_t value) {
    vds_widget *w = widget(u, id);
    if (!w) return 0;
    if (w->kind == VDS_WK_RADIO && value) { select_radio(u, id); return 1; }
    if (w->kind == VDS_WK_LIST) value = value < 0 ? 0 : value >= (int)w->count ? (int16_t)(w->count ? w->count - 1 : 0) : value;
    if (w->kind == VDS_WK_TABS) value = value < 0 ? 0 : value >= (int)tab_count(w) ? (int16_t)(tab_count(w) ? tab_count(w) - 1 : 0) : value;
    if (w->kind == VDS_WK_TEXT) { u->texts[w->slot].dirty = value != 0; return 1; }
    w->value = value;
    render(u, id);
    return 1;
}

int vds_ui_widget_layout(vds_ui *u, uint8_t id, uint8_t layout, uint8_t gap) {
    vds_widget *w = widget(u, id);
    if (!w || w->kind != VDS_WK_BOX || layout > VDS_LAYOUT_ROW) return 0;
    w->layout = layout;
    w->gap = gap;
    redraw_window_of(u, id);
    return 1;
}

int vds_ui_widget_size(vds_ui *u, uint8_t id, uint8_t fixed, uint8_t weight) {
    vds_widget *w = widget(u, id);
    if (!w) return 0;
    w->fixed = fixed;
    w->weight = weight;
    redraw_window_of(u, id);
    return 1;
}

static void compact(vds_ui *u) {                                   /* close gaps left by recounts */
    uint16_t used = 0;
    for (;;) {
        int next = -1;
        for (unsigned i = 0; i < VDS_MAX_WIDGETS; ++i) {
            const vds_widget *w = &u->widgets[i];
            if (w->used && w->kind == VDS_WK_LIST && w->count && w->first >= used && (next < 0 || w->first < u->widgets[next].first))
                next = (int)i;
        }
        if (next < 0) break;
        vds_widget *w = &u->widgets[next];
        if (w->first != used) memmove(u->items[used], u->items[w->first], (size_t)w->count * VDS_ITEM_TEXT);
        w->first = used;
        used = (uint16_t)(used + w->count);
    }
    u->items_used = used;
}

static void reverse_rows(vds_ui *u, unsigned a, unsigned b) {   /* rows [a, b) */
    char tmp[VDS_ITEM_TEXT];
    while (b > a + 1) {
        --b;
        memcpy(tmp, u->items[a], VDS_ITEM_TEXT);
        memcpy(u->items[a], u->items[b], VDS_ITEM_TEXT);
        memcpy(u->items[b], tmp, VDS_ITEM_TEXT);
        ++a;
    }
}

int vds_ui_widget_count(vds_ui *u, uint8_t id, uint16_t count) {
    vds_widget *w = widget(u, id);
    if (!w || w->kind != VDS_WK_LIST || count > VDS_LIST_POOL) return 0;
    if (count > w->count) {
        compact(u);                                                 /* packed, this list included */
        if (u->items_used - w->count + count > VDS_LIST_POOL) return 0;
        unsigned start = w->first, len = w->count, end = u->items_used;
        if (len && start + len < end) {                             /* rotate this list's rows to the end */
            reverse_rows(u, start, start + len);
            reverse_rows(u, start + len, end);
            reverse_rows(u, start, end);
            for (unsigned i = 0; i < VDS_MAX_WIDGETS; ++i) {
                vds_widget *o = &u->widgets[i];
                if (o->used && o->kind == VDS_WK_LIST && o != w && o->count && o->first > start) o->first = (uint16_t)(o->first - len);
            }
        }
        w->first = (uint16_t)(end - len);
        for (unsigned i = len; i < count; ++i) u->items[w->first + i][0] = 0;
        u->items_used = (uint16_t)(w->first + count);
    }
    w->count = count;
    render(u, id);
    return 1;
}

int vds_ui_widget_item(vds_ui *u, uint8_t id, uint16_t index, const char *text, size_t n) {
    vds_widget *w = widget(u, id);
    if (!w || w->kind != VDS_WK_LIST || index >= w->count) return 0;
    if (n >= VDS_ITEM_TEXT) n = VDS_ITEM_TEXT - 1;
    memcpy(u->items[w->first + index], text, n);
    u->items[w->first + index][n] = 0;
    render(u, id);
    return 1;
}

int vds_ui_widget_append(vds_ui *u, uint8_t id, const char *text, size_t n) {
    vds_widget *w = widget(u, id);
    if (!w || w->kind != VDS_WK_LIST || w->count >= VDS_LIST_POOL) return 0;
    if (!(w->count && w->first + w->count == u->items_used && u->items_used < VDS_LIST_POOL)) {   /* not at the pool end */
        uint16_t count = w->count;
        if (!vds_ui_widget_count(u, id, (uint16_t)(count + 1))) return 0;
        return vds_ui_widget_item(u, id, count, text, n);
    }
    if (n >= VDS_ITEM_TEXT) n = VDS_ITEM_TEXT - 1;               /* fast path: grow in place */
    memcpy(u->items[u->items_used], text, n);
    u->items[u->items_used][n] = 0;
    ++u->items_used;
    ++w->count;
    render(u, id);
    return 1;
}

int vds_ui_widget_columns(vds_ui *u, uint8_t id, const uint8_t *widths, uint8_t n) {
    vds_widget *w = widget(u, id);
    if (!w || w->kind != VDS_WK_LIST || n > VDS_TABLE_COLS) return 0;
    w->ncols = n;
    memcpy(w->cols, widths, n);
    redraw_window_of(u, id);
    return 1;
}

int vds_ui_widget_delete(vds_ui *u, uint8_t id) {
    vds_widget *w = widget(u, id);
    if (!w) return 0;
    uint8_t window = w->window;
    w->used = 0;                                                  /* before the children: no re-entry */
    for (unsigned i = 0; i < VDS_MAX_WIDGETS; ++i)                /* children go with it */
        if (u->widgets[i].used && u->widgets[i].parent == id) vds_ui_widget_delete(u, (uint8_t)i);
    if (u->caret_widget == id) caret_off(u);
    uint8_t s = surface_of(u, window);
    if (s != VDS_NONE && w->kind != VDS_WK_BOX && w->w && w->h)
        vds_fill(core(u), s, w->col, w->row, w->w, w->h, ' ', attr(core(u)->style.title_fg, core(u)->style.body));
    if (w->kind == VDS_WK_LIST) { w->count = 0; compact(u); }
    if (w->kind == VDS_WK_TEXT) vds_ui_text_release(u, id);
    if (u->widget_focus[window] == id) {
        u->widget_focus[window] = VDS_NONE;
        for (uint8_t i = next_widget(u, window, ANY_PARENT, VDS_NONE); i != VDS_NONE; i = next_widget(u, window, ANY_PARENT, i))
            if (focusable(u, &u->widgets[i])) { u->widget_focus[window] = i; break; }
    }
    vds_ui_widgets_refresh(u, window);
    return 1;
}

void vds_ui_widgets_forget(vds_ui *u, uint8_t window) {
    if (u->caret_widget != VDS_NONE && u->widgets[u->caret_widget].window == window) u->caret_widget = VDS_NONE;
    for (unsigned i = 0; i < VDS_MAX_WIDGETS; ++i)
        if (u->widgets[i].used && u->widgets[i].window == window) {
            if (u->widgets[i].kind == VDS_WK_TEXT) vds_ui_text_release(u, (uint8_t)i);
            u->widgets[i].used = 0;
        }
    if (window < VDS_MAX_WINDOWS) u->widget_focus[window] = VDS_NONE;
    compact(u);
}

int vds_ui_widget_focus(vds_ui *u, uint8_t id) {
    vds_widget *w = widget(u, id);
    if (!w || !focusable(u, w)) return 0;
    uint8_t old = u->widget_focus[w->window];
    u->widget_focus[w->window] = id;
    if (old != id) caret_off(u);
    if (old != VDS_NONE && old != id && widget(u, old)) render(u, old);
    render(u, id);
    return 1;
}

/* ------------------------------------------------------------ input */

static int tab(vds_ui *u, uint8_t window, int backwards) {
    uint8_t ring[VDS_MAX_WIDGETS], n = 0, at = VDS_NONE;     /* focusable widgets in creation order */
    for (uint8_t i = next_widget(u, window, ANY_PARENT, VDS_NONE); i != VDS_NONE; i = next_widget(u, window, ANY_PARENT, i))
        if (focusable(u, &u->widgets[i])) { if (i == u->widget_focus[window]) at = n; ring[n++] = i; }
    if (!n) return 0;
    uint8_t to = at == VDS_NONE ? (backwards ? (uint8_t)(n - 1) : 0) : (uint8_t)((at + (backwards ? n - 1 : 1)) % n);
    return vds_ui_widget_focus(u, ring[to]);
}

static void list_select(vds_ui *u, uint8_t id, int index) {
    vds_widget *w = &u->widgets[id];
    if (!w->count) return;
    if (index < 0) index = 0;
    if (index >= (int)w->count) index = w->count - 1;
    if (index == w->value) return;
    w->value = (int16_t)index;
    render(u, id);
    event(u, VDS_EVENT_SELECT, id, (int16_t)index, 1);
}

static int input_key(vds_ui *u, uint8_t id, uint8_t ascii, uint8_t vk) {
    vds_widget *w = &u->widgets[id];
    size_t len = strlen(w->text);
    int caret = w->value < 0 ? 0 : w->value > (int)len ? (int)len : w->value, changed = 0;
    switch (vk) {
    case VDS_VK_LEFT: case VDS_VK_KP_LEFT: if (caret) --caret; break;
    case VDS_VK_RIGHT: case VDS_VK_KP_RIGHT: if (caret < (int)len) ++caret; break;
    case VDS_VK_HOME: case VDS_VK_KP_HOME: caret = 0; break;
    case VDS_VK_END: case VDS_VK_KP_END: caret = (int)len; break;
    case VDS_VK_BACKSPACE:
        if (caret) { memmove(w->text + caret - 1, w->text + caret, len - (size_t)caret + 1); --caret; changed = 1; }
        break;
    case VDS_VK_DELETE:
        if (caret < (int)len) { memmove(w->text + caret, w->text + caret + 1, len - (size_t)caret); changed = 1; }
        break;
    case VDS_VK_RETURN: case VDS_VK_KP_ENTER: event(u, VDS_EVENT_ACTIVATE, id, 0, 0); return 1;
    default:
        if (ascii < 32 || ascii > 126) return 0;
        if (len + 1 >= VDS_WIDGET_TEXT) return 1;
        memmove(w->text + caret + 1, w->text + caret, len - (size_t)caret + 1);
        w->text[caret++] = (char)ascii;
        changed = 1;
    }
    w->value = (int16_t)caret;
    render(u, id);
    if (changed) event(u, VDS_EVENT_CHANGE, id, (int16_t)strlen(w->text), 1);
    return 1;
}

static int press(vds_ui *u, uint8_t id) {
    vds_widget *w = &u->widgets[id];
    switch (w->kind) {
    case VDS_WK_BUTTON: event(u, VDS_EVENT_ACTIVATE, id, 0, 0); return 1;
    case VDS_WK_CHECK: w->value = !w->value; render(u, id); event(u, VDS_EVENT_CHANGE, id, w->value, 1); return 1;
    case VDS_WK_RADIO: if (!w->value) { select_radio(u, id); event(u, VDS_EVENT_CHANGE, id, 1, 1); } return 1;
    }
    return 0;
}

/* Ctrl+Tab / Ctrl+Shift+Tab: next/previous tab of the window's first tabs widget. */
static int switch_tab(vds_ui *u, uint8_t window, int backwards) {
    uint8_t id = VDS_NONE;
    for (uint8_t i = next_widget(u, window, ANY_PARENT, VDS_NONE); i != VDS_NONE && id == VDS_NONE; i = next_widget(u, window, ANY_PARENT, i))
        if (u->widgets[i].kind == VDS_WK_TABS && focusable(u, &u->widgets[i])) id = i;
    if (id == VDS_NONE) return 0;
    vds_widget *w = &u->widgets[id];
    unsigned n = tab_count(w);
    if (n < 2) return 1;
    w->value = (int16_t)((w->value + (backwards ? n - 1 : 1)) % n);
    render(u, id);
    event(u, VDS_EVENT_SELECT, id, w->value, 1);
    return 1;
}

int vds_ui_widget_key(vds_ui *u, uint8_t ascii, uint8_t modifiers, uint8_t vk) {
    uint8_t window = u->focus;
    int ctrl = (modifiers & VDS_MOD_CTRL) != 0;
    if (window == VDS_NONE || (modifiers & (VDS_MOD_LALT | VDS_MOD_RALT))) return 0;
    if (u->widget_cols[window] != core(u)->windows[window].cols || u->widget_rows[window] != core(u)->windows[window].rows)
        vds_ui_widgets_refresh(u, window);
    if (vk == VDS_VK_TAB || (ascii == 9 && !vk)) return ctrl ? switch_tab(u, window, (modifiers & VDS_MOD_SHIFT) != 0)
                                                             : tab(u, window, (modifiers & VDS_MOD_SHIFT) != 0);
    uint8_t id = u->widget_focus[window];
    vds_widget *w = widget(u, id);
    if (!w || !focusable(u, w)) return 0;
    if (w->kind == VDS_WK_TEXT) return vds_ui_text_key(u, id, ascii, modifiers, vk);
    if (ctrl) return 0;
    int space = ascii == ' ' || vk == VDS_VK_SPACE, enter = vk == VDS_VK_RETURN || vk == VDS_VK_KP_ENTER;
    switch (w->kind) {
    case VDS_WK_INPUT: return input_key(u, id, ascii, vk);
    case VDS_WK_BUTTON: return (space || enter) && press(u, id);
    case VDS_WK_CHECK: return space && press(u, id);
    case VDS_WK_RADIO:
        if (space) return press(u, id);
        if (vk == VDS_VK_UP || vk == VDS_VK_DOWN) {                  /* move within the group */
            uint8_t ring[VDS_MAX_WIDGETS], n = 0, at = 0;
            for (uint8_t i = next_widget(u, window, w->parent, VDS_NONE); i != VDS_NONE; i = next_widget(u, window, w->parent, i))
                if (u->widgets[i].kind == VDS_WK_RADIO && focusable(u, &u->widgets[i])) { if (i == id) at = n; ring[n++] = i; }
            int to = vk == VDS_VK_DOWN ? at + 1 : at - 1;
            if (to >= 0 && to < n) { vds_ui_widget_focus(u, ring[to]); press(u, ring[to]); }
            return 1;
        }
        return 0;
    case VDS_WK_TABS: {
        int n = (int)tab_count(w), to = w->value;
        if (vk == VDS_VK_LEFT || vk == VDS_VK_KP_LEFT) to = w->value - 1;
        else if (vk == VDS_VK_RIGHT || vk == VDS_VK_KP_RIGHT) to = w->value + 1;
        else return 0;
        if (to >= 0 && to < n) { w->value = (int16_t)to; render(u, id); event(u, VDS_EVENT_SELECT, id, w->value, 1); }
        return 1;
    }
    case VDS_WK_LIST: {
        int page = w->h - (w->ncols && w->text[0]), last = w->count ? w->count - 1 : 0;
        if (page < 1) page = 1;
        if (w->flags & VDS_WF_PAGER) {                              /* past an edge: ask the CPU for rows */
            int dir = 0;
            if ((vk == VDS_VK_UP || vk == VDS_VK_KP_UP) && w->value <= 0) dir = -1;
            else if ((vk == VDS_VK_DOWN || vk == VDS_VK_KP_DOWN) && w->value >= last) dir = 1;
            else if ((vk == VDS_VK_PAGEUP || vk == VDS_VK_KP_PAGEUP) && w->value <= 0) dir = -2;
            else if ((vk == VDS_VK_PAGEDOWN || vk == VDS_VK_KP_PAGEDOWN) && w->value >= last) dir = 2;
            if (dir) { uint8_t a[2] = {(uint8_t)(int8_t)dir, w->window}; vds_proto_event(u->p, VDS_EVENT_PAGE, id, a, 2); return 1; }
        }
        if ((w->flags & VDS_WF_TREE) && w->count &&
            (vk == VDS_VK_LEFT || vk == VDS_VK_KP_LEFT || vk == VDS_VK_RIGHT || vk == VDS_VK_KP_RIGHT)) {
            uint8_t a[4] = {(uint8_t)w->value, (uint8_t)((uint16_t)w->value >> 8), w->window,
                            (uint8_t)(vk == VDS_VK_RIGHT || vk == VDS_VK_KP_RIGHT)};
            vds_proto_event(u->p, VDS_EVENT_EXPAND, id, a, 4);
            return 1;
        }
        switch (vk) {
        case VDS_VK_UP: case VDS_VK_KP_UP: list_select(u, id, w->value - 1); return 1;
        case VDS_VK_DOWN: case VDS_VK_KP_DOWN: list_select(u, id, w->value + 1); return 1;
        case VDS_VK_PAGEUP: case VDS_VK_KP_PAGEUP: list_select(u, id, w->value - page); return 1;
        case VDS_VK_PAGEDOWN: case VDS_VK_KP_PAGEDOWN: list_select(u, id, w->value + page); return 1;
        case VDS_VK_HOME: case VDS_VK_KP_HOME: list_select(u, id, 0); return 1;
        case VDS_VK_END: case VDS_VK_KP_END: list_select(u, id, w->count - 1); return 1;
        }
        if (enter) { event(u, VDS_EVENT_ACTIVATE, id, 0, 0); return 1; }
        if (ascii > 32 && ascii < 127)                               /* type-ahead */
            for (int k = 1; k <= (int)w->count; ++k) {
                int item = (w->value + k) % w->count;
                const char *t = u->items[w->first + item];
                while (*t == ' ' || *t == '+' || *t == '-') ++t;          /* tree rows: past the indent */
                if (lower((uint8_t)*t) == lower(ascii)) { list_select(u, id, item); return 1; }
            }
        return 0;
    }
    }
    return 0;
}

int vds_ui_widget_click(vds_ui *u, uint8_t window, int col, int row) {
    for (int i = VDS_MAX_WIDGETS - 1; i >= 0; --i) {
        vds_widget *w = &u->widgets[i];
        if (!w->used || w->window != window || !focusable(u, w) || col < w->col || col >= w->col + w->w || row < w->row || row >= w->row + w->h)
            continue;
        vds_ui_widget_focus(u, (uint8_t)i);
        if (w->kind == VDS_WK_INPUT) { w->value = (int16_t)(w->top + col - w->col); render(u, (uint8_t)i); return 1; }
        if (w->kind == VDS_WK_LIST) {
            int r = row - w->row - (w->ncols && w->text[0]);
            if (r >= 0 && w->top + r < w->count) list_select(u, (uint8_t)i, w->top + r);
            return 1;
        }
        if (w->kind == VDS_WK_TEXT || w->kind == VDS_WK_TABS) return 1;
        return press(u, (uint8_t)i) || 1;
    }
    return 0;
}

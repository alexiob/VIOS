/* VIOS Display Server files: /dev/win and /dev/ui/tree as text, rendered and
 * parsed here where the window and widget state lives (ops 9C-9E, see
 * vds_proto.h), so the CPU only moves bytes between handles and this server.
 *
 * Nodes are widgets (0..63, written as the id) and windows (VDS_NODE_WINDOW |
 * id, written w0..w26). Files (VDS_FILE_*):
 *   LIST   one line per node: id kind parent name rect. Windows first (rect
 *          x y cols rows, parent -), then widgets in creation order (rect col
 *          row w h; a top-level widget's parent is its window). Names have
 *          spaces as '_', '-' when empty; a window's name is its title.
 *   NEW    write "kind parent name" (window - Title, button w3 ok; a widget's
 *          parent is wN or a box): makes a node with the lowest free id (a
 *          window also gets the lowest free surface, 32x12, freed again when
 *          ctl closes the window). Reads give the id.
 *   CTL    read: properties, one per line; write: commands, one a line:
 *          rect a b c d, text T, name T, show 0|1, flags F, focus, raise (a
 *          window), value V, layout L G, size F W, style S (widgets: 0
 *          normal, 1 strong, 2 fault), theme T (the whole server: 0
 *          Smalltalk, 1 Turbo Vision), close (a window), delete.
 *   TEXT   the content: a label's text, a list's rows one per line, a
 *          TextView's text, a window's title. A write replaces it, or appends
 *          with the append flag (a handle's later writes).
 *   TREE   Forth source that rebuilds the windows and widgets (UI-SNAPSHOT).
 *          TextView text comes as UI-TEXT+ runs, with UI-CH ( char id -- )
 *          for the newlines and quotes S" cannot hold.
 * Events of a routed node (op 9E) become STREAM events whose arguments are a
 * text line (vds_files_event_line), in pieces of VDS_STREAM_PIECE bytes. */
#include "vds_ui.h"
#include <string.h>

enum { BOX = VDS_WK_BOX, LABEL, BUTTON, CHECK, RADIO, INPUT, LIST, PROGRESS, TEXTVIEW, TABS };
static const char *const KINDS[] = {"window", "box", "label", "button", "check", "radio", "input", "list", "progress", "text", "tabs"};
#define WINDOWS VDS_PROTO_WINDOWS
#define SURFACE_ATTR 0x30                       /* black on white, as the tools' windows */

static vds *core(vds_ui *u) { return u->p->v; }

/* ------------------------------------------------------------ text out */

typedef struct { char *s; unsigned n; } out;
static void put(out *o, char c) { if (o->n < VDS_FILE_LINE - 1) o->s[o->n++] = c; }
static void text(out *o, const char *t) { while (*t) put(o, *t++); }
static void textn(out *o, const char *t, size_t n) { for (size_t i = 0; i < n && t[i]; ++i) put(o, t[i]); }
static void number(out *o, int32_t v) {
    char b[8];
    unsigned i = 0;
    uint32_t x = v < 0 ? (uint32_t)-v : (uint32_t)v;
    if (v < 0) put(o, '-');
    do { b[i++] = (char)('0' + x % 10); x /= 10; } while (x);
    while (i) put(o, b[--i]);
}
static void numbers(out *o, const int32_t *v, unsigned n) { for (unsigned i = 0; i < n; ++i) { put(o, ' '); number(o, v[i]); } }
static void node_id(out *o, unsigned node) {
    if (node & VDS_NODE_WINDOW) put(o, 'w');
    number(o, (int32_t)(node & ~VDS_NODE_WINDOW));
}
static void name(out *o, const char *t, size_t n) {
    if (!n || !*t) put(o, '-');
    for (size_t i = 0; i < n && t[i]; ++i) put(o, t[i] == ' ' ? '_' : t[i]);
}
static void quoted(out *o, const char *t, size_t n) {       /* S" ..." with '"' as ', then a space */
    text(o, "S\" ");
    for (size_t i = 0; i < n && t[i]; ++i) put(o, t[i] == '"' ? '\'' : t[i]);
    text(o, "\" ");
}
static int message(out *o, const char *m) { o->n = 0; text(o, m); o->s[o->n] = 0; return -1; }

/* ------------------------------------------------------------ nodes */

static vds_widget *widget(vds_ui *u, unsigned id) { return id < VDS_MAX_WIDGETS && u->widgets[id].used ? &u->widgets[id] : 0; }
static vds_window *window(vds_ui *u, unsigned id) { return id < WINDOWS && core(u)->windows[id].used ? &core(u)->windows[id] : 0; }
static int exists(vds_ui *u, unsigned node) { return node & VDS_NODE_WINDOW ? window(u, node & 0x7F) != 0 : widget(u, node) != 0; }
static unsigned parent_node(const vds_widget *w) { return w->parent == VDS_NONE ? VDS_NODE_WINDOW | w->window : w->parent; }
static size_t text_len(const char *t, size_t max) { size_t n = 0; while (n < max && t[n]) ++n; return n; }

/* The widget at creation position `position`, or VDS_NONE. */
static unsigned widget_at(vds_ui *u, unsigned position) {
    int last = -1;
    for (unsigned k = 0;; ++k) {
        int next = -1;
        for (unsigned i = 0; i < VDS_MAX_WIDGETS; ++i)
            if (u->widgets[i].used && (last < 0 || u->widgets[i].seq > u->widgets[last].seq) &&
                (next < 0 || u->widgets[i].seq < u->widgets[next].seq)) next = (int)i;
        if (next < 0) return VDS_NONE;
        if (k == position) return (unsigned)next;
        last = next;
    }
}
/* The node at or after position step (windows by id, then widgets in
 * creation order); step is moved to its position. VDS_NONE: none left. */
static unsigned node_at(vds_ui *u, unsigned *step) {
    for (; *step < WINDOWS; ++*step) if (window(u, *step)) return VDS_NODE_WINDOW | *step;
    return widget_at(u, *step - WINDOWS);
}

/* ------------------------------------------------------------ reading */

#define DONE (-2)                               /* this node has no more lines */
static int list_line(vds_ui *u, unsigned node, unsigned sub, out *o) {
    if (sub) return DONE;
    node_id(o, node);
    if (node & VDS_NODE_WINDOW) {
        const vds_window *w = window(u, node & 0x7F);
        int32_t rect[4] = {w->x, w->y, w->cols, w->rows};
        text(o, " window - ");
        name(o, w->title, sizeof w->title);
        numbers(o, rect, 4);
    } else {
        const vds_widget *w = widget(u, node);
        int32_t rect[4] = {w->col, w->row, w->w, w->h};
        put(o, ' '); text(o, KINDS[w->kind]); put(o, ' ');
        node_id(o, parent_node(w));
        put(o, ' ');
        name(o, w->name, sizeof w->name);
        numbers(o, rect, 4);
    }
    return 1;
}

/* A TextView's text from offset at: a UI-TEXT+ run, or UI-CH for a character
 * S" cannot hold; *next is the offset after it. */
static void text_run(const vds_text *t, unsigned node, unsigned at, out *o, unsigned *next) {
    unsigned char c = (unsigned char)t->buf[at];
    if (c < ' ' || c == '"') {
        int32_t a[2] = {c, (int32_t)node};
        numbers(o, a, 2);
        text(o, " UI-CH");
        *next = at + 1;
        return;
    }
    unsigned n = 0;
    while (at + n < t->len && n < 60 && (unsigned char)t->buf[at + n] >= ' ' && t->buf[at + n] != '"') ++n;
    quoted(o, t->buf + at, n);
    number(o, (int32_t)node);
    text(o, " UI-TEXT+");
    *next = at + n;
}

/* Lines of a node: *next is the sub after this line (sub + 1 unless a
 * TextView run covers several bytes). */
static int tree_line(vds_ui *u, unsigned node, unsigned sub, out *o, unsigned *next) {
    if (node & VDS_NODE_WINDOW) {
        unsigned id = node & 0x7F;
        const vds_window *w = window(u, id);
        if (sub) return DONE;
        if (w->surface < VDS_PROTO_SURFACES && core(u)->surfaces[w->surface].cells) {
            int32_t s[4] = {w->surface, core(u)->surfaces[w->surface].cols, core(u)->surfaces[w->surface].rows, SURFACE_ATTR};
            numbers(o, s, 4);
            text(o, " UI-SURFACE ");
        }
        int32_t a[7] = {(int32_t)id, w->surface, w->x, w->y, w->cols, w->rows, w->flags & ~VDS_W_ACTIVE};   /* focus decides ACTIVE */
        numbers(o, a, 7);
        text(o, " UI-WINDOW ");
        quoted(o, w->title, sizeof w->title);
        number(o, (int32_t)id);
        text(o, " UI-TITLE");
        return 1;
    }
    const vds_widget *w = widget(u, node);
    const vds_text *t = w->kind == TEXTVIEW ? vds_ui_text(u, (uint8_t)node) : 0;
    unsigned rows = w->kind == LIST ? w->count : t ? t->len : 0;     /* a sub per row, or per text byte */
    if (sub == 0) {
        int32_t a[9] = {(int32_t)node, w->window, w->kind, w->parent, w->col, w->row, w->w, w->h, w->flags};
        numbers(o, a, 9);
        text(o, " UI-WIDGET");
    } else if (sub == 1 || sub == 2) {
        const char *t = sub == 1 ? w->text : w->name;
        if (!*t) return 0;
        quoted(o, t, sub == 1 ? sizeof w->text : sizeof w->name);
        number(o, (int32_t)node);
        text(o, sub == 1 ? " UI-WTEXT" : " UI-WNAME");
    } else if (sub == 3 && t) {                             /* text: emptied, then in runs */
        if (!rows) return 0;
        quoted(o, "", 0);
        number(o, (int32_t)node);
        text(o, " UI-TEXT");
    } else if (sub < 4 + rows && t) {
        unsigned after;
        text_run(t, node, sub - 4, o, &after);
        *next = 4 + after;
    } else if (sub == 3) {                                  /* rows: UI-ITEMS, then UI-ITEM each */
        if (!rows) return 0;
        int32_t a[2] = {(int32_t)node, (int32_t)rows};
        numbers(o, a, 2);
        text(o, " UI-ITEMS");
    } else if (sub < 4 + rows) {
        quoted(o, u->items[w->first + sub - 4], VDS_ITEM_TEXT);
        number(o, (int32_t)node);
        put(o, ' ');
        number(o, (int32_t)(sub - 4));
        text(o, " UI-ITEM");
    } else if (sub == 4 + rows) {
        unsigned k = w->kind;
        if (w->value && (k == CHECK || k == RADIO || k == PROGRESS || k == TABS || k == LIST)) {
            int32_t a[2] = {(int32_t)node, w->value};
            numbers(o, a, 2);
            text(o, " UI-WVALUE");
        }
        if (w->layout) { int32_t a[3] = {(int32_t)node, w->layout, w->gap}; numbers(o, a, 3); text(o, " UI-LAYOUT"); }
        if (w->fixed || w->weight != 1) { int32_t a[3] = {(int32_t)node, w->fixed, w->weight}; numbers(o, a, 3); text(o, " UI-WSIZE"); }
        if (w->style) { int32_t a[2] = {(int32_t)node, w->style}; numbers(o, a, 2); text(o, " UI-WSTYLE"); }
        if (!o->n) return 0;
    } else return DONE;
    return 1;
}

static int ctl_line(vds_ui *u, unsigned node, unsigned step, out *o) {
    static const char *const WINDOW_KEYS[] = {"kind window", "name ", "rect", "show ", "flags ", "surface", "focus "};
    static const char *const WIDGET_KEYS[] = {"kind ", "parent ", "name ", "rect", "show ", "flags ", "value ", "count ",
                                              "layout", "size", "focus ", "style ", "text "};
    if (node & VDS_NODE_WINDOW) {
        const vds_window *w = window(u, node & 0x7F);
        uint8_t focus = u->widget_focus[node & 0x7F];
        if (step >= 7) return 0;
        text(o, WINDOW_KEYS[step]);
        switch (step) {
        case 1: textn(o, w->title, sizeof w->title); break;
        case 2: { int32_t a[4] = {w->x, w->y, w->cols, w->rows}; numbers(o, a, 4); break; }
        case 3: number(o, w->flags & VDS_W_VISIBLE); break;
        case 4: number(o, w->flags); break;
        case 5: {
            const vds_surface *s = w->surface < VDS_MAX_SURFACES ? &core(u)->surfaces[w->surface] : 0;
            int32_t a[3] = {w->surface, s ? s->cols : 0, s ? s->rows : 0};
            numbers(o, a, 3);
            break;
        }
        case 6: if (focus == VDS_NONE) put(o, '-'); else number(o, focus); break;
        }
        return 1;
    }
    const vds_widget *w = widget(u, node);
    if (step >= 13) return 0;
    text(o, WIDGET_KEYS[step]);
    switch (step) {
    case 0: text(o, KINDS[w->kind]); break;
    case 1: node_id(o, parent_node(w)); break;
    case 2: textn(o, w->name, sizeof w->name); break;
    case 3: { int32_t a[4] = {w->col, w->row, w->w, w->h}; numbers(o, a, 4); break; }
    case 4: number(o, !(w->flags & VDS_WF_HIDDEN)); break;
    case 5: number(o, w->flags); break;
    case 6: number(o, w->value); break;
    case 7: number(o, w->count); break;
    case 8: { int32_t a[2] = {w->layout, w->gap}; numbers(o, a, 2); break; }
    case 9: { int32_t a[2] = {w->fixed, w->weight}; numbers(o, a, 2); break; }
    case 10: number(o, u->widget_focus[w->window] == node); break;
    case 11: number(o, w->style); break;
    case 12: textn(o, w->text, sizeof w->text); break;
    }
    return 1;
}

/* Text: one line (label, title), a row per line (list), or the TextView text
 * in runs that carry their own newlines (*raw set). */
static int text_line(vds_ui *u, unsigned node, unsigned step, out *o, int *raw, uint16_t *next) {
    const vds_widget *w = node & VDS_NODE_WINDOW ? 0 : widget(u, node);
    *next = (uint16_t)(step + 1);
    if (!w) {
        if (step) return 0;
        textn(o, window(u, node & 0x7F)->title, VDS_TITLE_MAX);
    } else if (w->kind == LIST) {
        if (step >= w->count) return 0;
        textn(o, u->items[w->first + step], VDS_ITEM_TEXT);
    } else if (w->kind == TEXTVIEW) {
        const vds_text *t = vds_ui_text(u, (uint8_t)node);
        if (!t || step >= t->len) return 0;
        unsigned n = t->len - step > VDS_FILE_LINE - 1 ? VDS_FILE_LINE - 1 : t->len - step;
        memcpy(o->s, t->buf + step, n);
        o->n = n;
        *raw = 1;
        *next = (uint16_t)(step + n);
    } else {
        if (step) return 0;
        textn(o, w->text, sizeof w->text);
    }
    return 1;
}

int vds_files_line(vds_ui *u, uint8_t file, uint8_t node, uint16_t *step, uint16_t *sub, char *s) {
    out o = {s, 0};
    int raw = 0;
    if (file == VDS_FILE_LIST || file == VDS_FILE_TREE) {
        unsigned at = *step, part = *sub;
        for (;;) {
            unsigned n = node_at(u, &at);
            if (n == VDS_NONE) return 0;
            o.n = 0;
            unsigned next = part + 1;
            int got = file == VDS_FILE_LIST ? list_line(u, n, part, &o) : tree_line(u, n, part, &o, &next);
            if (got == DONE) { ++at; part = 0; continue; }
            if (!got) { ++part; continue; }
            *step = (uint16_t)at;
            *sub = (uint16_t)next;
            break;
        }
    } else if (file == VDS_FILE_NEW) {
        if (*step) return 0;
        if (node == VDS_NONE) return message(&o, "nothing made yet");
        node_id(&o, node);
        *step = 1;
    } else if (file == VDS_FILE_CTL || file == VDS_FILE_TEXT || file == VDS_FILE_EVENT) {
        if (node == VDS_NONE || !exists(u, node)) return message(&o, node & VDS_NODE_WINDOW ? "no such window" : "no such widget");
        if (file == VDS_FILE_EVENT) return 0;              /* opening it checks the node; events come as STREAM */
        if (file == VDS_FILE_CTL) { if (!ctl_line(u, node, *step, &o)) return 0; ++*step; }
        else if (!text_line(u, node, *step, &o, &raw, step)) return 0;
    } else return message(&o, "no such file");
    if (!raw) s[o.n++] = '\n';
    return (int)o.n;
}

/* ------------------------------------------------------------ writing */

static void skip(const char **p, const char *end) { while (*p < end && (**p == ' ' || **p == '\t')) ++*p; }
static int word(const char **p, const char *end, char *w, unsigned capacity) {
    unsigned n = 0;
    skip(p, end);
    for (; *p < end && **p != ' ' && **p != '\t'; ++*p) if (n + 1 < capacity) w[n++] = **p;
    w[n] = 0;
    return n > 0;
}
static int integer(const char **p, const char *end, int32_t *v) {
    int32_t n = 0;
    skip(p, end);
    int negative = *p < end && **p == '-';
    if (negative) ++*p;
    if (*p >= end || **p < '0' || **p > '9') return 0;
    for (; *p < end && **p >= '0' && **p <= '9'; ++*p) if ((n = n * 10 + (**p - '0')) > 99999) return 0;
    *v = negative ? -n : n;
    return 1;
}
/* Exactly n integers in min..max, nothing after them. */
static int integers(const char **p, const char *end, int32_t *v, unsigned n, int32_t min, int32_t max) {
    for (unsigned i = 0; i < n; ++i) if (!integer(p, end, &v[i]) || v[i] < min || v[i] > max) return 0;
    skip(p, end);
    return *p == end;
}
static int node_of(const char *s, unsigned *node) {
    unsigned n = 0, k = 0, win = *s == 'w' || *s == 'W';
    for (s += win; s[k] >= '0' && s[k] <= '9' && k < 3; ++k) n = n * 10 + (unsigned)(s[k] - '0');
    if (!k || s[k] || n >= (win ? WINDOWS : VDS_MAX_WIDGETS)) return 0;
    *node = win ? VDS_NODE_WINDOW | n : n;
    return 1;
}
static int is(const char *a, const char *b) { return !strcmp(a, b); }

/* Text of a node: the title, a widget's text (TextView: its text). */
static int set_text(vds_ui *u, unsigned node, const char *t, size_t n) {
    char buf[VDS_TITLE_MAX];
    if (node & VDS_NODE_WINDOW) {
        if (n > VDS_TITLE_MAX - 1) n = VDS_TITLE_MAX - 1;
        memcpy(buf, t, n);
        buf[n] = 0;
        return vds_window_title(core(u), (uint8_t)(node & 0x7F), buf);
    }
    if (widget(u, node)->kind == TEXTVIEW) return vds_ui_text_set(u, (uint8_t)node, 0, t, n);
    return vds_ui_widget_text(u, (uint8_t)node, t, n);
}

static int make(vds_ui *u, const char *p, const char *end, uint8_t *made, out *o) {
    char kind[12], parent[8];
    unsigned k, node;
    if (!word(&p, end, kind, sizeof kind)) return 1;
    if (!word(&p, end, parent, sizeof parent)) return message(&o[0], "new: kind parent name");
    skip(&p, end);
    const char *label = p;
    size_t n = (size_t)(end - p);
    for (k = 0; k <= TABS && !is(kind, KINDS[k]); ++k) {}
    if (k > TABS) return message(o, "new: unknown kind");
    if (!k) {
        unsigned w = 0, s = 0;
        vds_cell *cells;
        if (!is(parent, "-")) return message(o, "new: a window's parent is -");
        while (w < WINDOWS && window(u, w)) ++w;
        while (s < VDS_PROTO_SURFACES && core(u)->surfaces[s].cells) ++s;
        if (w == WINDOWS || s == VDS_PROTO_SURFACES) return message(o, "new: no free window or surface");
        if (!u->p->cells || !(cells = u->p->cells(u->p->ctx, (uint8_t)s, 32 * 12)) ||
            !vds_surface_init(core(u), (uint8_t)s, 32, 12, cells, SURFACE_ATTR) ||
            !vds_window_open(core(u), (uint8_t)w, (uint8_t)s, 64, 64, 32, 12, VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW, ""))
            return message(o, "new: the display server refused it");
        u->file_surfaces |= (uint16_t)(1u << s);
        node = VDS_NODE_WINDOW | w;
    } else {
        unsigned pn, win, box = VDS_NONE, id = 0;
        if (!node_of(parent, &pn)) return message(o, "new: parent wN or a box id");
        if (pn & VDS_NODE_WINDOW) {
            if (!window(u, pn & 0x7F)) return message(o, "no such window");
            win = pn & 0x7F;
        } else {
            if (!widget(u, pn)) return message(o, "no such widget");
            if (u->widgets[pn].kind != BOX) return message(o, "new: the parent must be a box");
            win = u->widgets[pn].window;
            box = pn;
        }
        while (id < VDS_MAX_WIDGETS && u->widgets[id].used) ++id;
        if (id == VDS_MAX_WIDGETS) return message(o, "new: no free widget id");
        int pane = k == BOX || k == LIST || k == TEXTVIEW;
        unsigned w = pane || k == PROGRESS ? 20 : n + 4 < 8 ? 8 : n + 4 > 40 ? 40 : (unsigned)n + 4;
        if (!vds_ui_widget(u, (uint8_t)id, (uint8_t)win, (uint8_t)k, (uint8_t)box, 0, 0, (uint8_t)w, pane ? 4 : 1, 0))
            return message(o, "new: the display server refused it");
        size_t nn = n > VDS_WIDGET_NAME ? VDS_WIDGET_NAME : n;
        memcpy(u->widgets[id].name, label, nn);
        u->widgets[id].name[nn] = 0;
        node = id;
    }
    if (!k || k == LABEL || k == BUTTON || k == CHECK || k == RADIO || k == TABS) set_text(u, node, label, n);
    *made = (uint8_t)node;
    return 1;
}

static int command(vds_ui *u, unsigned node, const char *p, const char *end, out *o) {
    char cmd[12];
    int32_t v[4];
    unsigned id = node & 0x7F;
    int win = (node & VDS_NODE_WINDOW) != 0;
    vds *c = core(u);
    if (!word(&p, end, cmd, sizeof cmd)) return 1;
    if (!exists(u, node)) return message(o, win ? "no such window" : "no such widget");
    vds_widget *w = win ? 0 : &u->widgets[id];
    if (is(cmd, "text") || (win && is(cmd, "name"))) {
        if (p < end && *p == ' ') ++p;
        return set_text(u, node, p, (size_t)(end - p)) ? 1 : message(o, "text refused");
    }
    if (is(cmd, "rect")) {
        if (!integers(&p, end, v, 4, win ? -32768 : 0, win ? 32767 : 255)) return message(o, "rect: x y cols rows (widget: col row w h)");
        if (!win) return vds_ui_widget_place(u, (uint8_t)id, (uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3]);
        if (v[2] < 1 || v[2] > 255 || v[3] < 1 || v[3] > 255) return message(o, "rect: 1..255 columns and rows");
        vds_window *x = &c->windows[id];
        vds_surface *s = x->surface < VDS_PROTO_SURFACES ? &c->surfaces[x->surface] : 0;
        if (s && s->cells && (s->cols < v[2] || s->rows < v[3]) && u->p->cells) {        /* grow the surface it shows */
            uint8_t cols = (uint8_t)(v[2] > 80 ? 80 : v[2] > s->cols ? v[2] : s->cols), rows = (uint8_t)(v[3] > 60 ? 60 : v[3] > s->rows ? v[3] : s->rows);
            vds_cell *cells = u->p->cells(u->p->ctx, x->surface, (uint16_t)(cols * rows));
            if (cells) vds_surface_init(c, x->surface, cols, rows, cells, SURFACE_ATTR);
        }
        if (!vds_window_move(c, (uint8_t)id, (int16_t)v[0], (int16_t)v[1]) || !vds_window_resize(c, (uint8_t)id, (uint8_t)v[2], (uint8_t)v[3]))
            return message(o, "rect refused");
        vds_ui_widgets_refresh(u, (uint8_t)id);
        return 1;
    }
    if (is(cmd, "show") || is(cmd, "flags")) {
        int show = cmd[0] == 's';
        if (!integers(&p, end, v, 1, 0, show ? 1 : 255)) return message(o, show ? "show 0 or 1" : "flags 0..255");
        unsigned flags = win ? c->windows[id].flags : w->flags;
        if (!show) flags = (unsigned)v[0];
        else if (win) flags = v[0] ? flags | VDS_W_VISIBLE : flags & ~(unsigned)VDS_W_VISIBLE;
        else flags = v[0] ? flags & ~(unsigned)VDS_WF_HIDDEN : flags | VDS_WF_HIDDEN;
        return win ? vds_window_flags(c, (uint8_t)id, (uint8_t)flags) : vds_ui_widget_flags(u, (uint8_t)id, (uint8_t)flags);
    }
    if (is(cmd, "focus")) {
        if (win ? vds_ui_focus(u, (uint8_t)id) : vds_ui_widget_focus(u, (uint8_t)id)) return 1;
        return message(o, win ? "focus refused (a modal window?)" : "focus: not a focusable widget");
    }
    if (is(cmd, "theme")) {
        if (!integers(&p, end, v, 1, 0, 1)) return message(o, "theme 0 Smalltalk or 1 Turbo Vision");
        return vds_set_style(c, (uint8_t)v[0]);
    }
    if (is(cmd, "close") || is(cmd, "delete")) {
        if (!win) return vds_ui_widget_delete(u, (uint8_t)id);
        uint8_t s = c->windows[id].surface;
        if (!vds_window_close(c, (uint8_t)id)) return message(o, "close refused");
        vds_ui_forget(u, (uint8_t)id);
        unsigned shown = 0;
        for (unsigned k = 0; k < WINDOWS; ++k) shown |= window(u, k) && c->windows[k].surface == s;
        if (s < VDS_PROTO_SURFACES && u->file_surfaces >> s & 1 && !shown) {   /* ours and unused: free the id */
            memset(&c->surfaces[s], 0, sizeof c->surfaces[s]);
            u->file_surfaces &= (uint16_t)~(1u << s);
        }
        return 1;
    }
    if (win) {
        if (is(cmd, "raise")) return vds_window_raise(c, (uint8_t)id);
        return message(o, "window ctl: rect text name show flags focus raise close theme");
    }
    if (is(cmd, "name")) {
        skip(&p, end);
        size_t n = (size_t)(end - p) > VDS_WIDGET_NAME ? VDS_WIDGET_NAME : (size_t)(end - p);
        memcpy(w->name, p, n);
        w->name[n] = 0;
        return 1;
    }
    if (is(cmd, "style")) {
        if (!integers(&p, end, v, 1, 0, VDS_WSTYLE_FAULT)) return message(o, "style 0 normal, 1 strong, 2 fault");
        return vds_ui_widget_style(u, (uint8_t)id, (uint8_t)v[0]);
    }
    if (is(cmd, "value")) {
        if (!integers(&p, end, v, 1, -32768, 32767)) return message(o, "value -32768..32767");
        return vds_ui_widget_value(u, (uint8_t)id, (int16_t)v[0]);
    }
    if (is(cmd, "layout") || is(cmd, "size")) {
        int layout = cmd[0] == 'l';
        if (!integers(&p, end, v, 2, 0, 255) || (layout && v[0] > 2)) return message(o, layout ? "layout 0..2 gap" : "size fixed weight");
        if (layout && w->kind != BOX) return message(o, "layout: boxes only");
        return layout ? vds_ui_widget_layout(u, (uint8_t)id, (uint8_t)v[0], (uint8_t)v[1])
                      : vds_ui_widget_size(u, (uint8_t)id, (uint8_t)v[0], (uint8_t)v[1]);
    }
    return message(o, "widget ctl: rect text name show flags focus value layout size style delete theme");
}

static int write_text(vds_ui *u, unsigned node, int append, const char *t, size_t n, out *o) {
    vds_widget *w = node & VDS_NODE_WINDOW ? 0 : widget(u, node);
    if (!exists(u, node)) return message(o, node & VDS_NODE_WINDOW ? "no such window" : "no such widget");
    if (w && w->kind == LIST) {                         /* a row per line */
        if (!append && !vds_ui_widget_count(u, (uint8_t)node, 0)) return message(o, "text refused");
        for (const char *end = t + n; t < end;) {
            const char *nl = t;
            while (nl < end && *nl != '\n') ++nl;
            size_t k = (size_t)(nl - t);
            if (k && t[k - 1] == '\r') --k;
            if (!vds_ui_widget_append(u, (uint8_t)node, t, k)) return message(o, "list full");
            t = nl + 1;
        }
        return 1;
    }
    if (w && w->kind == TEXTVIEW) return vds_ui_text_set(u, (uint8_t)node, append, t, n) ? 1 : message(o, "text refused");
    char buf[VDS_WIDGET_TEXT];                          /* one line: newlines become spaces, the last one goes */
    size_t len = 0, max = w ? VDS_WIDGET_TEXT - 1 : VDS_TITLE_MAX - 1;
    if (append) {
        const char *now = w ? w->text : window(u, node & 0x7F)->title;
        len = text_len(now, max);
        memcpy(buf, now, len);
    }
    for (size_t i = 0; i < n && len < max; ++i)
        if (t[i] != '\r' && !(t[i] == '\n' && i + 1 == n)) buf[len++] = t[i] == '\n' ? ' ' : t[i];
    return set_text(u, node, buf, len) ? 1 : message(o, "text refused");
}

int vds_files_write(vds_ui *u, uint8_t file, uint8_t *node, int append, const char *t, size_t n, char *msg) {
    out o = {msg, 0};
    msg[0] = 0;
    if (file == VDS_FILE_TEXT) { int ok = write_text(u, *node, append, t, n, &o) > 0; msg[o.n] = 0; return ok; }
    if (file != VDS_FILE_NEW && file != VDS_FILE_CTL) { message(&o, "not writable"); msg[o.n] = 0; return 0; }
    for (const char *end = t + n; t < end;) {           /* one command or node a line */
        const char *nl = t;
        while (nl < end && *nl != '\n') ++nl;
        const char *stop = nl > t && nl[-1] == '\r' ? nl - 1 : nl;
        int ok = file == VDS_FILE_NEW ? make(u, t, stop, node, &o) : command(u, *node, t, stop, &o);
        if (ok <= 0) {
            if (!o.n) text(&o, "refused");
            msg[o.n] = 0;
            return 0;
        }
        t = nl + 1;
    }
    return 1;
}

/* ------------------------------------------------------------ events */

static unsigned event_node(uint8_t kind, uint8_t id) {
    switch (kind) {
    case VDS_EVENT_ACTIVATE: case VDS_EVENT_SELECT: case VDS_EVENT_CHANGE: case VDS_EVENT_TEXT: case VDS_EVENT_EXPAND: case VDS_EVENT_PAGE:
        return id < VDS_MAX_WIDGETS ? id : VDS_NONE;
    case VDS_EVENT_FOCUS: case VDS_EVENT_POINTER: case VDS_EVENT_WIN: case VDS_EVENT_COMMAND:
        return id < WINDOWS ? VDS_NODE_WINDOW | id : VDS_NONE;
    }
    return VDS_NONE;
}

size_t vds_files_event_line(uint8_t kind, const uint8_t *args, uint8_t n, char *s) {
    static const char *const TEXT_WHAT[] = {"text", "accept", "do", "print", "inspect"};
    static const char *const WIN_WHAT[] = {"win", "moved", "close", "zoomed", "restored"};
    uint8_t a[32] = {0};                                /* zero past the end */
    out o = {s, 0};
    memcpy(a, args, n < sizeof a ? n : sizeof a);
    switch (kind) {
    case VDS_EVENT_ACTIVATE: text(&o, "activate"); break;
    case VDS_EVENT_SELECT: text(&o, "select "); number(&o, a[0] | a[1] << 8); break;
    case VDS_EVENT_CHANGE: text(&o, "change "); number(&o, (int16_t)(a[0] | a[1] << 8)); break;
    case VDS_EVENT_TEXT: { int32_t v[2] = {a[1] | a[2] << 8, a[3] | a[4] << 8}; text(&o, TEXT_WHAT[a[0] <= 4 ? a[0] : 0]); numbers(&o, v, 2); break; }
    case VDS_EVENT_EXPAND: { int32_t v[2] = {a[0] | a[1] << 8, a[3]}; text(&o, "expand"); numbers(&o, v, 2); break; }
    case VDS_EVENT_PAGE: text(&o, "page "); number(&o, (int8_t)a[0]); break;
    case VDS_EVENT_FOCUS: text(&o, "focus"); break;
    case VDS_EVENT_POINTER: {
        int32_t v[6] = {(int16_t)(a[0] | a[1] << 8), (int16_t)(a[2] | a[3] << 8), a[4], a[5], a[6], a[7]};
        text(&o, "pointer");
        numbers(&o, v, 6);
        break;
    }
    case VDS_EVENT_WIN: {
        int32_t v[4] = {(int16_t)(a[1] | a[2] << 8), (int16_t)(a[3] | a[4] << 8), a[5], a[6]};
        text(&o, WIN_WHAT[a[0] <= 4 ? a[0] : 0]);
        if (a[0] != VDS_WIN_CLOSE) numbers(&o, v, 4);
        break;
    }
    case VDS_EVENT_COMMAND: { int32_t v[2] = {a[0] | a[1] << 8, a[2]}; text(&o, "command"); numbers(&o, v, 2); break; }
    default: text(&o, "event "); number(&o, kind); break;
    }
    s[o.n++] = '\n';
    return o.n;
}

int vds_files_route(vds_ui *u, uint8_t node, uint8_t on) {
    if (node & VDS_NODE_WINDOW ? (node & 0x7F) >= WINDOWS : node >= VDS_MAX_WIDGETS) return 0;
    uint64_t *bits = node & VDS_NODE_WINDOW ? &u->routed_windows : &u->routed_widgets, bit = (uint64_t)1 << (node & 0x3F);
    *bits = on ? *bits | bit : *bits & ~bit;
    return 1;
}

int vds_files_event(vds_ui *u, uint8_t kind, uint8_t id, const uint8_t *args, uint8_t n) {
    unsigned node = event_node(kind, id);
    char line[VDS_FILE_LINE];
    if (node == VDS_NONE) return 0;
    uint64_t bits = node & VDS_NODE_WINDOW ? u->routed_windows : u->routed_widgets;
    if (!(bits >> (node & 0x3F) & 1)) return 0;
    size_t len = vds_files_event_line(kind, args, n, line);
    for (size_t at = 0; at < len; at += VDS_STREAM_PIECE) {      /* pieces; the CPU joins them in its pipe */
        size_t k = len - at < VDS_STREAM_PIECE ? len - at : VDS_STREAM_PIECE;
        vds_proto_event(u->p, VDS_EVENT_STREAM, (uint8_t)node, (const uint8_t *)line + at, (uint8_t)k);
    }
    return 1;
}

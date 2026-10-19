/* VIOS Display Server core: see vds.h. */
#include "vds.h"
#include <string.h>

/* ------------------------------------------------------------ styles */

static const vds_style STYLES[VDS_STYLE_COUNT] = {
    {   /* Smalltalk: grey desktop, white windows, black borders, title tabs. */
        VDS_STYLE_SMALLTALK,
        {0x00, 0x30, 0x3C, 0x3F, 0x2A, 0x15, 0x3E, 0x2B, 0x17, 0x3A, 0x2E, 0x1B, 0x05, 0x29, 0x39, 0x01},
        {0, 14, 13, 4, 5, 0, 13, 11, 15, 14, 13, 15, 0, 5, 1, 0},
        4, 5, 3, 0, 0, 0, 0, 6, 4,
        0, 3, 8, 3, 0, 5, 0
    },
    {   /* Turbo Vision: blue desktop, blue windows, double frames, drop shadows. */
        VDS_STYLE_TURBO,
        {0x00, 0x30, 0x3C, 0x3F, 0x02, 0x06, 0x2A, 0x15, 0x08, 0x0F, 0x0A, 0x28, 0x01, 0x2F, 0x20, 0x17},
        {0, 14, 11, 7, 12, 12, 7, 0, 0, 10, 12, 0, 0, 10, 0, 12},
        5, 12, 4, 3, 6, 3, 6, 4, 4,
        0, 6, 1, 3, 8, 7, 0
    },
};

const vds_style *vds_builtin_style(uint8_t kind) { return kind < VDS_STYLE_COUNT ? &STYLES[kind] : 0; }

/* ------------------------------------------------------------ geometry */

static int empty(vds_rect r) { return r.w <= 0 || r.h <= 0; }
static int has_row(vds_rect r, int y) { return y >= r.y && y < r.y + r.h; }
static vds_rect rect(int x, int y, int w, int h) { vds_rect r; r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h; return r; }
static vds_rect unite(vds_rect a, vds_rect b) {
    if (empty(a)) return b;
    if (empty(b)) return a;
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return rect(x0, y0, x1 - x0, y1 - y0);
}
static vds_rect intersect(vds_rect a, vds_rect b) {
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    return x1 > x0 && y1 > y0 ? rect(x0, y0, x1 - x0, y1 - y0) : rect(0, 0, 0, 0);
}
static long area(vds_rect r) { return empty(r) ? 0 : (long)r.w * r.h; }

typedef struct { vds_rect client, body, tab, shadow_right, shadow_bottom; } geometry;

static geometry geom(const vds *v, const vds_window *w) {
    geometry g;
    int cw = w->cols * 8, ch = w->rows * 8;
    int turbo = v->style.kind == VDS_STYLE_TURBO;
    g.client = rect(w->x, w->y, cw, ch);
    g.body = g.client;
    g.tab = rect(0, 0, 0, 0);
    if ((w->flags & VDS_W_MENU) && !(w->flags & VDS_W_DECOR))
        g.body = rect(w->x - 1, w->y - 1, cw + 2, ch + 2);
    else if (w->flags & VDS_W_DECOR) {
        int b = turbo ? 8 : 2;
        g.body = rect(w->x - b, w->y - b, cw + 2 * b, ch + 2 * b);
        if (!turbo && w->title[0])
            g.tab = rect(g.body.x, g.body.y - 10, (int)(strlen(w->title) + 2) * 8, 11);
    }
    g.shadow_right = g.shadow_bottom = rect(0, 0, 0, 0);
    if (w->flags & VDS_W_SHADOW) {
        int sx = turbo ? 16 : 4, sy = turbo ? 8 : 4;
        g.shadow_right = rect(g.body.x + g.body.w, g.body.y + sy, sx, g.body.h);
        g.shadow_bottom = rect(g.body.x + sx, g.body.y + g.body.h, g.body.w, sy);
    }
    return g;
}

vds_rect vds_window_outer(const vds *v, uint8_t id) {
    if (id >= VDS_MAX_WINDOWS || !v->windows[id].used) return rect(0, 0, 0, 0);
    geometry g = geom(v, &v->windows[id]);
    return unite(unite(g.body, g.tab), unite(g.shadow_right, g.shadow_bottom));
}

/* ------------------------------------------------------------ damage */

void vds_damage(vds *v, vds_rect r) {
    r = intersect(r, rect(0, 0, v->width, v->height));
    if (empty(r)) return;
    for (unsigned i = 0; i < v->ndamage; ++i)
        if (area(intersect(v->damage[i], r)) == area(r)) return;      /* already covered */
    if (v->ndamage < VDS_MAX_DAMAGE) { v->damage[v->ndamage++] = r; return; }
    /* Bounded list: grow the rect whose union with r adds the least area. */
    unsigned best = 0; long cost = -1;
    for (unsigned i = 0; i < v->ndamage; ++i) {
        long c = area(unite(v->damage[i], r)) - area(v->damage[i]);
        if (cost < 0 || c < cost) { cost = c; best = i; }
    }
    v->damage[best] = unite(v->damage[best], r);
}

void vds_damage_all(vds *v) { v->ndamage = 0; vds_damage(v, rect(0, 0, v->width, v->height)); }

static void damage_window(vds *v, uint8_t id) {
    uint8_t f = v->windows[id].flags;
    if ((f & (VDS_W_VISIBLE | VDS_W_MODAL)) == (VDS_W_VISIBLE | VDS_W_MODAL)) vds_damage_all(v);   /* dim changes */
    else if (f & VDS_W_VISIBLE) vds_damage(v, vds_window_outer(v, id));
}

static void damage_surface(vds *v, uint8_t sid, int col, int row, int w, int h) {
    for (unsigned i = 0; i < v->count; ++i) {
        const vds_window *win = &v->windows[v->order[i]];
        if (win->surface != sid || !(win->flags & VDS_W_VISIBLE)) continue;
        vds_rect cells = rect(win->x + (col - win->scroll_col) * 8, win->y + (row - win->scroll_row) * 8, w * 8, h * 8);
        vds_damage(v, intersect(cells, rect(win->x, win->y, win->cols * 8, win->rows * 8)));
    }
}

/* ------------------------------------------------------------ pixels */

static uint8_t *line(vds *v) { return (uint8_t *)v->line; }

static void set_px(uint8_t *ln, int x, uint8_t c) {
    uint8_t *b = ln + (x >> 1);
    *b = (x & 1) ? (uint8_t)((*b & 0xF0) | c) : (uint8_t)((*b & 0x0F) | (c << 4));
}
static uint8_t get_px(const uint8_t *ln, int x) { return (x & 1) ? ln[x >> 1] & 15 : ln[x >> 1] >> 4; }

/* Fill pixels [a, b) clipped to the composed span [x0, x1). */
static void fill(uint8_t *ln, int a, int b, uint8_t c, int x0, int x1) {
    if (a < x0) a = x0;
    if (b > x1) b = x1;
    if (a >= b) return;
    if (a & 1) set_px(ln, a++, c);
    if (b & 1) set_px(ln, --b, c);
    if (a < b) memset(ln + (a >> 1), (c << 4) | c, (size_t)(b - a) >> 1);
}

static void shade(const vds *v, uint8_t *ln, vds_rect r, int y, int x0, int x1) {
    if (!has_row(r, y)) return;
    int a = r.x > x0 ? r.x : x0, b = r.x + r.w < x1 ? r.x + r.w : x1;
    for (int x = a; x < b; ++x) set_px(ln, x, v->style.shade[get_px(ln, x)]);
}

static void hline_or_sides(uint8_t *ln, vds_rect r, int y, uint8_t c, int x0, int x1) {
    if (!has_row(r, y)) return;
    if (y == r.y || y == r.y + r.h - 1) { fill(ln, r.x, r.x + r.w, c, x0, x1); return; }
    fill(ln, r.x, r.x + 1, c, x0, x1);
    fill(ln, r.x + r.w - 1, r.x + r.w, c, x0, x1);
}

/* 8 pixels of one glyph row for colour pair attr, MSB first. */
static const uint32_t *lut(vds *v, uint8_t attr) {
    for (unsigned i = 0; i < VDS_LUT_SLOTS; ++i)
        if (v->lut[i].valid && v->lut[i].attr == attr) return v->lut[i].px;
    unsigned s = v->lut_next++ % VDS_LUT_SLOTS;
    uint8_t fg = attr & 15, bg = attr >> 4;
    for (unsigned bits = 0; bits < 256; ++bits) {
        uint8_t bytes[4];
        for (unsigned k = 0; k < 4; ++k) {
            uint8_t l = (bits & (0x80 >> (2 * k))) ? fg : bg, r = (bits & (0x40 >> (2 * k))) ? fg : bg;
            bytes[k] = (uint8_t)((l << 4) | r);
        }
        memcpy(&v->lut[s].px[bits], bytes, 4);      /* memory order, endianness-agnostic */
    }
    v->lut[s].attr = attr;
    v->lut[s].valid = 1;
    ++v->stats.lut_misses;
    return v->lut[s].px;
}

static void glyph(vds *v, int px, uint8_t bits, uint8_t attr, int x0, int x1) {
    if (!(px & 7) && px >= x0 && px + 8 <= x1) {   /* snapped: one aligned word */
        v->line[px >> 3] = lut(v, attr)[bits];
        return;
    }
    uint8_t fg = attr & 15, bg = attr >> 4;          /* free x or clipped */
    for (int i = 0; i < 8; ++i)
        if (px + i >= x0 && px + i < x1) set_px(line(v), px + i, (bits & (0x80 >> i)) ? fg : bg);
}

static void text(vds *v, int px, int gy, const char *s, uint8_t fg, int bg, int x0, int x1) {
    for (; *s && px < x1; ++s, px += 8) {
        if (px + 8 <= x0) continue;
        uint8_t bits = v->font[(uint8_t)*s * 8 + gy];
        if (bg >= 0) { glyph(v, px, bits, (uint8_t)(fg | bg << 4), x0, x1); continue; }
        for (int i = 0; i < 8; ++i)
            if ((bits & (0x80 >> i)) && px + i >= x0 && px + i < x1) set_px(line(v), px + i, fg);
    }
}

/* ------------------------------------------------------------ composition */

static void client_row(vds *v, const vds_window *w, int y, int x0, int x1) {
    int gy = y - w->y;
    if (gy < 0 || gy >= w->rows * 8) return;
    const vds_surface *s = w->surface < VDS_MAX_SURFACES ? &v->surfaces[w->surface] : 0;
    int srow = gy / 8 + w->scroll_row, grow = gy & 7;
    int first = x0 <= w->x ? 0 : (x0 - w->x) / 8;
    int last = (x1 - w->x + 7) / 8;
    if (last > w->cols) last = w->cols;
    uint8_t blank = (uint8_t)(v->style.body << 4 | v->style.body);
    for (int c = first; c < last; ++c) {
        int sc = c + w->scroll_col;
        vds_cell cell = {' ', blank};
        uint8_t bits;
        if (s && s->cells && srow < s->rows && sc < s->cols) {
            cell = s->cells[srow * s->cols + sc];
            bits = v->font[cell.ch * 8 + grow];
            if (s->cur_shape && sc == s->cur_col && srow == s->cur_row && (!s->cur_blink || v->blink_on)) {
                if (s->cur_shape == VDS_CURSOR_BLOCK) cell.attr = (uint8_t)(cell.attr >> 4 | cell.attr << 4);
                else if (s->cur_shape == VDS_CURSOR_UNDERLINE && grow >= 6) bits = 0xFF;
                else if (s->cur_shape == VDS_CURSOR_BAR) bits |= 0xC0;
            }
        } else
            bits = v->font[cell.ch * 8 + grow];
        glyph(v, w->x + c * 8, bits, cell.attr, x0, x1);
    }
}

static void window_row(vds *v, const vds_window *w, int y, int x0, int x1) {
    geometry g = geom(v, w);
    const vds_style *st = &v->style;
    uint8_t *ln = line(v);
    int active = (w->flags & VDS_W_ACTIVE) != 0;
    shade(v, ln, g.shadow_right, y, x0, x1);
    shade(v, ln, g.shadow_bottom, y, x0, x1);
    if ((w->flags & VDS_W_DECOR) && st->kind == VDS_STYLE_SMALLTALK) {
        if (!empty(g.tab) && has_row(g.tab, y) && y < g.body.y) {
            int ty = y - g.tab.y;
            if (ty == 0) fill(ln, g.tab.x, g.tab.x + g.tab.w, st->frame, x0, x1);
            else {
                fill(ln, g.tab.x, g.tab.x + g.tab.w, active ? st->tab : st->tab_inactive, x0, x1);
                hline_or_sides(ln, rect(g.tab.x, g.tab.y - 1, g.tab.w, g.tab.h + 2), y, st->frame, x0, x1);
                if (ty >= 2 && ty < 10) text(v, g.tab.x + 8, ty - 2, w->title, active ? st->title_fg : st->title_fg_inactive, -1, x0, x1);
            }
        }
        if (has_row(g.body, y)) {
            int t = active ? 2 : 1, bx = g.body.x, bw = g.body.w;
            if (y < g.body.y + t || y >= g.body.y + g.body.h - t) fill(ln, bx, bx + bw, st->frame, x0, x1);
            else {
                fill(ln, bx, bx + t, st->frame, x0, x1);
                fill(ln, bx + bw - t, bx + bw, st->frame, x0, x1);
                fill(ln, bx + t, bx + bw - t, st->body, x0, x1);
            }
        }
    } else if ((w->flags & VDS_W_MENU) && !(w->flags & VDS_W_DECOR) && has_row(g.body, y)) {
        hline_or_sides(ln, g.body, y, st->menu_frame, x0, x1);
    } else if ((w->flags & VDS_W_DECOR) && has_row(g.body, y)) {
        fill(ln, g.body.x, g.body.x + g.body.w, st->body, x0, x1);
        vds_rect b = g.body;
        if (active) {
            hline_or_sides(ln, rect(b.x + 2, b.y + 2, b.w - 4, b.h - 4), y, st->frame, x0, x1);
            hline_or_sides(ln, rect(b.x + 5, b.y + 5, b.w - 10, b.h - 10), y, st->frame, x0, x1);
        } else
            hline_or_sides(ln, rect(b.x + 3, b.y + 3, b.w - 6, b.h - 6), y, st->frame_inactive, x0, x1);
        if (y < b.y + 8 && w->title[0]) {
            char label[VDS_TITLE_MAX + 2];
            size_t n = strlen(w->title);
            label[0] = ' ';
            memcpy(label + 1, w->title, n);
            label[n + 1] = ' ';
            label[n + 2] = 0;
            /* Centred; a title wider than the window is clipped to the frame
             * (never drawn outside the window's damage rect). */
            int room = b.w - (int)(n + 2) * 8, tx = room > 0 ? b.x + ((room / 2) & ~7) : b.x + 8;
            int cx0 = x0 > b.x + 8 ? x0 : b.x + 8, cx1 = x1 < b.x + b.w - 8 ? x1 : b.x + b.w - 8;
            if (cx0 < cx1) text(v, tx, y - b.y, label, active ? st->title_fg : st->title_fg_inactive, st->body, cx0, cx1);
        }
    }
    client_row(v, w, y, x0, x1);
}

static void sprite_row(vds *v, unsigned id, int y, int x0, int x1) {
    const vds_sprite *s = &v->sprites[id];
    if (!s->used || !s->visible) return;
    vds_rect r = vds_sprite_rect(v, (uint8_t)id);
    if (!has_row(r, y)) return;
    const uint8_t *row = s->pixels + (size_t)(y - r.y) * ((s->w + 1u) / 2);
    int a = r.x > x0 ? r.x : x0, b = r.x + r.w < x1 ? r.x + r.w : x1;
    for (int x = a; x < b; ++x) {
        uint8_t c = get_px(row, x - r.x);
        if (c != s->transparent) set_px(line(v), x, c);
    }
}

static void compose(vds *v, int y, int x0, int x1) {
    int start = -1;
    for (int i = v->count - 1; i >= 0 && start < 0; --i) {   /* topmost opaque cover */
        const vds_window *w = &v->windows[v->order[i]];
        if (!(w->flags & VDS_W_VISIBLE)) continue;
        vds_rect b = geom(v, w).body;
        if (has_row(b, y) && b.x <= x0 && b.x + b.w >= x1) start = i;
    }
    int modal = -1;                                          /* topmost visible modal window */
    for (int i = v->count - 1; i >= 0 && modal < 0; --i) {
        const vds_window *w = &v->windows[v->order[i]];
        if ((w->flags & (VDS_W_VISIBLE | VDS_W_MODAL)) == (VDS_W_VISIBLE | VDS_W_MODAL)) modal = i;
    }
    if (start < 0) {
        uint8_t a = v->style.desktop_a, b = v->style.desktop_b;
        uint8_t pattern = (y & 1) ? (uint8_t)(a << 4 | b) : (uint8_t)(b << 4 | a);
        memset(line(v) + (x0 >> 1), pattern, (size_t)(x1 - x0) >> 1);
        start = 0;
    }
    for (int i = start; i < v->count; ++i) {
        const vds_window *w = &v->windows[v->order[i]];
        if (i == modal) shade(v, line(v), rect(x0, y, x1 - x0, 1), y, x0, x1);
        if (w->flags & VDS_W_VISIBLE) window_row(v, w, y, x0, x1);
    }
    for (unsigned i = 0; i < VDS_MAX_SPRITES; ++i) sprite_row(v, i, y, x0, x1);
    memcpy(v->rows[y] + (x0 >> 1), line(v) + (x0 >> 1), (size_t)(x1 - x0) >> 1);
}

int vds_frame(vds *v, uint32_t budget) {
    if (!v->ndamage) return 1;
    ++v->stats.frames;
    int ymin = v->height, ymax = 0;
    for (unsigned i = 0; i < v->ndamage; ++i) {
        if (v->damage[i].y < ymin) ymin = v->damage[i].y;
        if (v->damage[i].y + v->damage[i].h > ymax) ymax = v->damage[i].y + v->damage[i].h;
    }
    uint32_t done = 0;
    for (int y = ymin; y < ymax; ++y) {
        int16_t a[VDS_MAX_DAMAGE], b[VDS_MAX_DAMAGE];
        unsigned n = 0;
        for (unsigned i = 0; i < v->ndamage; ++i) {
            vds_rect d = v->damage[i];
            if (!has_row(d, y)) continue;
            int s = d.x & ~7, e = (d.x + d.w + 7) & ~7;
            if (e > v->width) e = v->width;
            unsigned k = n++;
            while (k && a[k - 1] > s) { a[k] = a[k - 1]; b[k] = b[k - 1]; --k; }
            a[k] = (int16_t)s; b[k] = (int16_t)e;
        }
        if (!n) continue;
        unsigned m = 0;                                      /* merge overlapping spans */
        for (unsigned i = 1; i < n; ++i) {
            if (a[i] <= b[m]) { if (b[i] > b[m]) b[m] = b[i]; }
            else { ++m; a[m] = a[i]; b[m] = b[i]; }
        }
        for (unsigned i = 0; i <= m; ++i) {
            compose(v, y, a[i], b[i]);
            done += (uint32_t)(b[i] - a[i]);
            ++v->stats.spans;
        }
        ++v->stats.rows;
        if (v->row_written) v->row_written(v->row_ctx, (int16_t)y);
        if (done >= budget && y + 1 < ymax) {                /* carry the rest over */
            unsigned keep = 0;
            for (unsigned i = 0; i < v->ndamage; ++i) {
                vds_rect d = v->damage[i];
                if (d.y + d.h <= y + 1) continue;
                if (d.y < y + 1) { d.h = (int16_t)(d.h - (y + 1 - d.y)); d.y = (int16_t)(y + 1); }
                v->damage[keep++] = d;
            }
            v->ndamage = (uint8_t)keep;
            v->stats.pixels += done;
            ++v->stats.carried;
            return 0;
        }
    }
    v->stats.pixels += done;
    v->ndamage = 0;
    return 1;
}

/* ------------------------------------------------------------ setup */

int vds_init(vds *v, uint8_t **rows, int16_t width, int16_t height, const uint8_t *font) {
    if (!v || !rows || !font || width <= 0 || width > VDS_MAX_WIDTH || (width & 7) || height <= 0) return 0;
    memset(v, 0, sizeof *v);
    v->rows = rows;
    v->width = width;
    v->height = height;
    v->font = font;
    v->snap_x = 8;
    v->blink_on = 1;
    v->style = STYLES[VDS_STYLE_SMALLTALK];
    vds_damage_all(v);
    return 1;
}

int vds_set_style(vds *v, uint8_t kind) {
    if (kind >= VDS_STYLE_COUNT) return 0;
    v->style = STYLES[kind];
    vds_damage_all(v);
    return 1;
}

int vds_set_snap(vds *v, uint8_t snap_x) {
    if (snap_x != 8 && snap_x != 1) return 0;
    v->snap_x = snap_x;
    return 1;
}

/* ------------------------------------------------------------ surfaces */

int vds_surface_init(vds *v, uint8_t id, uint8_t cols, uint8_t rows, vds_cell *cells, uint8_t attr) {
    if (id >= VDS_MAX_SURFACES || !cells || !cols || !rows) return 0;
    vds_surface *s = &v->surfaces[id];
    s->cols = cols;
    s->rows = rows;
    s->cells = cells;
    s->cur_shape = VDS_CURSOR_OFF;
    s->cur_col = s->cur_row = 0;
    for (unsigned i = 0; i < (unsigned)cols * rows; ++i) { cells[i].ch = ' '; cells[i].attr = attr; }
    damage_surface(v, id, 0, 0, cols, rows);
    return 1;
}

static vds_surface *surface(vds *v, uint8_t id) {
    return id < VDS_MAX_SURFACES && v->surfaces[id].cells ? &v->surfaces[id] : 0;
}

int vds_put(vds *v, uint8_t id, uint8_t col, uint8_t row, uint8_t attr, const char *s, size_t n) {
    vds_surface *sf = surface(v, id);
    if (!sf || row >= sf->rows || col >= sf->cols) return 0;
    if (n > (size_t)(sf->cols - col)) n = sf->cols - col;
    int first = -1, last = -1;                          /* damage only cells that change */
    for (size_t i = 0; i < n; ++i) {
        vds_cell *c = &sf->cells[row * sf->cols + col + i];
        if (c->ch == (uint8_t)s[i] && c->attr == attr) continue;
        c->ch = (uint8_t)s[i];
        c->attr = attr;
        if (first < 0) first = (int)i;
        last = (int)i;
    }
    if (first >= 0) damage_surface(v, id, col + first, row, last - first + 1, 1);
    return 1;
}

int vds_fill(vds *v, uint8_t id, uint8_t col, uint8_t row, uint8_t w, uint8_t h, uint8_t ch, uint8_t attr) {
    vds_surface *sf = surface(v, id);
    if (!sf || row >= sf->rows || col >= sf->cols) return 0;
    if (w > sf->cols - col) w = (uint8_t)(sf->cols - col);
    if (h > sf->rows - row) h = (uint8_t)(sf->rows - row);
    int changed = 0;
    for (unsigned y = row; y < (unsigned)row + h; ++y)
        for (unsigned x = col; x < (unsigned)col + w; ++x) {
            vds_cell *c = &sf->cells[y * sf->cols + x];
            changed |= c->ch != ch || c->attr != attr;
            c->ch = ch;
            c->attr = attr;
        }
    if (changed) damage_surface(v, id, col, row, w, h);
    return 1;
}

int vds_scroll(vds *v, uint8_t id, int dy, uint8_t attr) {
    vds_surface *sf = surface(v, id);
    if (!sf) return 0;
    int rows = sf->rows, cols = sf->cols;
    if (dy >= rows || -dy >= rows) return vds_fill(v, id, 0, 0, sf->cols, sf->rows, ' ', attr);
    if (dy > 0) memmove(sf->cells, sf->cells + dy * cols, sizeof(vds_cell) * (size_t)(rows - dy) * cols);
    else if (dy < 0) memmove(sf->cells - dy * cols, sf->cells, sizeof(vds_cell) * (size_t)(rows + dy) * cols);
    int from = dy > 0 ? rows - dy : 0, count = dy > 0 ? dy : -dy;
    for (int y = from; y < from + count; ++y)
        for (int x = 0; x < cols; ++x) { sf->cells[y * cols + x].ch = ' '; sf->cells[y * cols + x].attr = attr; }
    damage_surface(v, id, 0, 0, cols, rows);
    return 1;
}

int vds_cursor(vds *v, uint8_t id, uint8_t col, uint8_t row, uint8_t shape, uint8_t blink) {
    vds_surface *sf = surface(v, id);
    if (!sf || shape > VDS_CURSOR_BAR || col >= sf->cols || row >= sf->rows) return 0;
    if (sf->cur_shape) damage_surface(v, id, sf->cur_col, sf->cur_row, 1, 1);
    sf->cur_col = col;
    sf->cur_row = row;
    sf->cur_shape = shape;
    sf->cur_blink = blink != 0;        /* the blink phase is shared; never reset it here */
    if (shape) damage_surface(v, id, col, row, 1, 1);
    return 1;
}

void vds_tick(vds *v, uint32_t ms) {
    v->blink_ms += ms;
    if (v->blink_ms < VDS_BLINK_MS) return;
    v->blink_ms %= VDS_BLINK_MS;
    v->blink_on ^= 1;
    for (uint8_t id = 0; id < VDS_MAX_SURFACES; ++id) {
        const vds_surface *sf = &v->surfaces[id];
        if (sf->cells && sf->cur_shape && sf->cur_blink) damage_surface(v, id, sf->cur_col, sf->cur_row, 1, 1);
    }
}

/* ------------------------------------------------------------ windows */

static vds_window *window(vds *v, uint8_t id) { return id < VDS_MAX_WINDOWS && v->windows[id].used ? &v->windows[id] : 0; }

static int16_t snapped(const vds *v, uint8_t flags, int x) {
    return (int16_t)((v->snap_x == 8 && !(flags & VDS_W_FREE_X)) ? (x & ~7) : x);
}

/* Index just above the last window of the band a window with `flags` joins. */
static unsigned band_top(const vds *v, uint8_t flags) {
    if (flags & VDS_W_OVERLAY) return v->count;
    unsigned at = 0;
    while (at < v->count && !(v->windows[v->order[at]].flags & VDS_W_OVERLAY)) ++at;
    return at;
}

static void set_title(vds_window *w, const char *title) {
    size_t n = title ? strlen(title) : 0;
    if (n >= VDS_TITLE_MAX) n = VDS_TITLE_MAX - 1;
    if (n) memcpy(w->title, title, n);
    w->title[n] = 0;
}

int vds_window_open(vds *v, uint8_t id, uint8_t surface_id, int16_t x, int16_t y,
                    uint8_t cols, uint8_t rows, uint8_t flags, const char *title) {
    if (id >= VDS_MAX_WINDOWS || v->windows[id].used || !cols || !rows) return 0;
    if (surface_id != VDS_NONE && surface_id >= VDS_MAX_SURFACES) return 0;
    vds_window *w = &v->windows[id];
    memset(w, 0, sizeof *w);
    w->used = 1;
    w->surface = surface_id;
    w->flags = flags;
    w->cols = cols;
    w->rows = rows;
    w->x = snapped(v, flags, x);
    w->y = y;
    set_title(w, title);
    unsigned at = band_top(v, flags);
    for (unsigned i = v->count; i > at; --i) v->order[i] = v->order[i - 1];
    v->order[at] = id;
    ++v->count;
    damage_window(v, id);
    return 1;
}

int vds_window_move(vds *v, uint8_t id, int16_t x, int16_t y) {
    vds_window *w = window(v, id);
    if (!w) return 0;
    damage_window(v, id);
    w->x = snapped(v, w->flags, x);
    w->y = y;
    damage_window(v, id);
    return 1;
}

int vds_window_resize(vds *v, uint8_t id, uint8_t cols, uint8_t rows) {
    vds_window *w = window(v, id);
    if (!w || !cols || !rows) return 0;
    damage_window(v, id);
    w->cols = cols;
    w->rows = rows;
    damage_window(v, id);
    return 1;
}

int vds_window_raise(vds *v, uint8_t id) {
    if (!window(v, id)) return 0;
    unsigned i = 0;
    while (v->order[i] != id) ++i;
    for (; i + 1 < v->count; ++i) v->order[i] = v->order[i + 1];
    --v->count;
    unsigned at = band_top(v, v->windows[id].flags);       /* ordinary windows stay below overlays */
    for (unsigned k = v->count; k > at; --k) v->order[k] = v->order[k - 1];
    v->order[at] = id;
    ++v->count;
    damage_window(v, id);
    return 1;
}

int vds_window_lower(vds *v, uint8_t id) {
    if (!window(v, id)) return 0;
    unsigned i = 0;
    while (v->order[i] != id) ++i;
    for (; i > 0; --i) v->order[i] = v->order[i - 1];
    v->order[0] = id;
    damage_window(v, id);
    return 1;
}

int vds_window_flags(vds *v, uint8_t id, uint8_t flags) {
    vds_window *w = window(v, id);
    if (!w) return 0;
    damage_window(v, id);
    w->flags = flags;
    damage_window(v, id);
    return 1;
}

int vds_window_scroll(vds *v, uint8_t id, uint8_t col, uint8_t row) {
    vds_window *w = window(v, id);
    if (!w) return 0;
    w->scroll_col = col;
    w->scroll_row = row;
    if (w->flags & VDS_W_VISIBLE) vds_damage(v, rect(w->x, w->y, w->cols * 8, w->rows * 8));
    return 1;
}

int vds_window_surface(vds *v, uint8_t id, uint8_t surface_id) {
    vds_window *w = window(v, id);
    if (!w || (surface_id != VDS_NONE && surface_id >= VDS_MAX_SURFACES)) return 0;
    w->surface = surface_id;
    if (w->flags & VDS_W_VISIBLE) vds_damage(v, rect(w->x, w->y, w->cols * 8, w->rows * 8));
    return 1;
}

int vds_window_title(vds *v, uint8_t id, const char *title) {
    vds_window *w = window(v, id);
    if (!w) return 0;
    damage_window(v, id);
    set_title(w, title);
    damage_window(v, id);
    return 1;
}

int vds_window_close(vds *v, uint8_t id) {
    vds_window *w = window(v, id);
    if (!w) return 0;
    damage_window(v, id);
    unsigned i = 0;
    while (v->order[i] != id) ++i;
    for (; i + 1 < v->count; ++i) v->order[i] = v->order[i + 1];
    --v->count;
    w->used = 0;
    return 1;
}

/* ------------------------------------------------------------ sprites */

const uint8_t vds_arrow_cursor[VDS_ARROW_W / 2 * VDS_ARROW_H] = {
    0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x03, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF,
    0x03, 0x30, 0xFF, 0xFF, 0xFF, 0xFF,
    0x03, 0x33, 0x0F, 0xFF, 0xFF, 0xFF,
    0x03, 0x33, 0x30, 0xFF, 0xFF, 0xFF,
    0x03, 0x33, 0x33, 0x0F, 0xFF, 0xFF,
    0x03, 0x33, 0x33, 0x30, 0xFF, 0xFF,
    0x03, 0x33, 0x33, 0x33, 0x0F, 0xFF,
    0x03, 0x33, 0x33, 0x33, 0x30, 0xFF,
    0x03, 0x33, 0x33, 0x00, 0x00, 0x0F,
    0x03, 0x30, 0x33, 0x0F, 0xFF, 0xFF,
    0x03, 0x0F, 0x03, 0x30, 0xFF, 0xFF,
    0x00, 0xFF, 0x03, 0x30, 0xFF, 0xFF,
    0x0F, 0xFF, 0xF0, 0x33, 0x0F, 0xFF,
    0xFF, 0xFF, 0xF0, 0x00, 0x0F, 0xFF,
};

vds_rect vds_sprite_rect(const vds *v, uint8_t id) {
    if (id >= VDS_MAX_SPRITES || !v->sprites[id].used) return rect(0, 0, 0, 0);
    const vds_sprite *s = &v->sprites[id];
    return rect(s->x - s->hot_x, s->y - s->hot_y, s->w, s->h);
}

static void damage_sprite(vds *v, uint8_t id) {
    if (v->sprites[id].used && v->sprites[id].visible) vds_damage(v, vds_sprite_rect(v, id));
}

int vds_sprite_define(vds *v, uint8_t id, uint8_t w, uint8_t h, uint8_t hot_x, uint8_t hot_y,
                      uint8_t transparent, const uint8_t *pixels) {
    if (id >= VDS_MAX_SPRITES || !pixels || !w || !h || w > VDS_SPRITE_MAX || h > VDS_SPRITE_MAX ||
        hot_x >= w || hot_y >= h || (transparent > 15 && transparent != VDS_NONE)) return 0;
    vds_sprite *s = &v->sprites[id];
    int visible = s->used && s->visible;
    damage_sprite(v, id);
    int16_t x = s->used ? s->x : 0, y = s->used ? s->y : 0;
    s->used = 1;
    s->visible = (uint8_t)visible;
    s->w = w;
    s->h = h;
    s->hot_x = hot_x;
    s->hot_y = hot_y;
    s->transparent = transparent;
    s->pixels = pixels;
    s->x = x;
    s->y = y;
    damage_sprite(v, id);
    return 1;
}

int vds_sprite_move(vds *v, uint8_t id, int16_t x, int16_t y) {
    if (id >= VDS_MAX_SPRITES || !v->sprites[id].used) return 0;
    if (v->sprites[id].x == x && v->sprites[id].y == y) return 1;
    damage_sprite(v, id);
    v->sprites[id].x = x;
    v->sprites[id].y = y;
    damage_sprite(v, id);
    return 1;
}

int vds_sprite_show(vds *v, uint8_t id, int visible) {
    if (id >= VDS_MAX_SPRITES || !v->sprites[id].used) return 0;
    damage_sprite(v, id);
    v->sprites[id].visible = visible != 0;
    damage_sprite(v, id);
    return 1;
}

int vds_sprite_undefine(vds *v, uint8_t id) {
    if (id >= VDS_MAX_SPRITES || !v->sprites[id].used) return 0;
    damage_sprite(v, id);
    memset(&v->sprites[id], 0, sizeof v->sprites[id]);
    return 1;
}

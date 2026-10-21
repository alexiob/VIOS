/* VIOS Display Server G5 canvases: see vds_canvas.h. */
#include "vds_canvas.h"
#include <string.h>

/* ------------------------------------------------------------ helpers */

static int empty(vds_rect r) { return r.w <= 0 || r.h <= 0; }
static vds_rect rect(long x, long y, long w, long h) {
    vds_rect r;
    r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
    return r;
}
static vds_rect clip(long x, long y, long w, long h, vds_rect to) {   /* x..x+w in long: no overflow */
    long x0 = x > to.x ? x : to.x, y0 = y > to.y ? y : to.y;
    long x1 = x + w < to.x + to.w ? x + w : to.x + to.w, y1 = y + h < to.y + to.h ? y + h : to.y + to.h;
    return x1 > x0 && y1 > y0 ? rect(x0, y0, x1 - x0, y1 - y0) : rect(0, 0, 0, 0);
}
static vds_rect unite(vds_rect a, vds_rect b) {
    if (empty(a)) return b;
    if (empty(b)) return a;
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return rect(x0, y0, x1 - x0, y1 - y0);
}
static long labs_(long v) { return v < 0 ? -v : v; }
static int contains(vds_rect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static vds_canvas *canvas(vds *v, uint8_t id) { return id < VDS_MAX_CANVASES && v->canvases[id].used ? &v->canvases[id] : 0; }
static const vds_canvas *ccanvas(const vds *v, uint8_t id) { return id < VDS_MAX_CANVASES && v->canvases[id].used ? &v->canvases[id] : 0; }
static vds_item *items(const vds_canvas *c) { return (vds_item *)c->items; }
static vds_rect whole(const vds_canvas *c) { return rect(0, 0, c->w, c->h); }
static uint32_t stride(int w) { return ((uint32_t)w + 1u) / 2; }

#define ITEMS_BYTES ((uint32_t)VDS_CANVAS_ITEMS * sizeof(vds_item))

uint32_t vds_canvas_bytes(int16_t w, int16_t h) {
    if (w <= 0 || h <= 0) return 0;
    return ITEMS_BYTES + stride(w) * (uint32_t)h;
}

/* ------------------------------------------------------------ rasteriser */

typedef struct {
    const vds *v;
    uint8_t *out;
    uint32_t stride;
    vds_rect clip;              /* canvas pixels being drawn */
} pen;

static void plot(const pen *p, int x, int y, uint8_t c) {
    if (!contains(p->clip, x, y)) return;
    uint8_t *b = p->out + (size_t)y * p->stride + (x >> 1);
    *b = (x & 1) ? (uint8_t)((*b & 0xF0) | c) : (uint8_t)((*b & 0x0F) | c << 4);
}

static void span(const pen *p, int y, int x0, int x1, uint8_t c) {   /* [x0, x1] inclusive */
    if (y < p->clip.y || y >= p->clip.y + p->clip.h) return;
    if (x0 < p->clip.x) x0 = p->clip.x;
    if (x1 > p->clip.x + p->clip.w - 1) x1 = p->clip.x + p->clip.w - 1;
    if (x0 > x1) return;
    uint8_t *row = p->out + (size_t)y * p->stride;
    if (x0 & 1) { plot(p, x0, y, c); ++x0; }
    if (!(x1 & 1)) { plot(p, x1, y, c); --x1; }
    if (x0 < x1) memset(row + (x0 >> 1), c << 4 | c, (size_t)(x1 - x0 + 1) >> 1);
}

static long isqrt(long n) {
    long r = 0, bit = 1L << 30;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* Pixels of row j of the ellipse inscribed in a w x h box: those whose
 * centre lies inside it, a contiguous run [*lo, *hi]. In doubled coordinates
 * X = 2i + 1 - w, Y = 2j + 1 - h the test is X^2 h^2 + Y^2 w^2 <= w^2 h^2. */
static int ellipse_row(long w, long h, long j, long *lo, long *hi) {
    if (j < 0 || j >= h) return 0;
    long y = 2 * j + 1 - h;
    long xm = isqrt((long)((long long)w * w * (h * h - y * y) / (h * h)));
    long x0 = -xm, x1 = xm;
    if ((x0 + w - 1) & 1) ++x0;
    if ((x1 + w - 1) & 1) --x1;
    if (x0 > x1) return 0;
    *lo = (x0 + w - 1) / 2;
    *hi = (x1 + w - 1) / 2;
    return 1;
}

static void ellipse(const pen *p, const vds_item *it, int filled) {
    long w = it->c, h = it->d, lo, hi, nlo, nhi;
    if (w <= 0 || h <= 0) return;
    long j0 = p->clip.y - it->b, j1 = p->clip.y + p->clip.h - it->b;   /* only rows inside the clip */
    if (j0 < 0) j0 = 0;
    if (j1 > h) j1 = h;
    for (long j = j0; j < j1; ++j) {
        if (!ellipse_row(w, h, j, &lo, &hi)) continue;
        int y = (int)(it->b + j);
        if (filled) { span(p, y, (int)(it->a + lo), (int)(it->a + hi), it->colour); continue; }
        /* Outline: inside pixels with a 4-neighbour outside. */
        plot(p, (int)(it->a + lo), y, it->colour);
        plot(p, (int)(it->a + hi), y, it->colour);
        for (int k = -1; k <= 1; k += 2) {
            if (!ellipse_row(w, h, j + k, &nlo, &nhi)) { span(p, y, (int)(it->a + lo), (int)(it->a + hi), it->colour); continue; }
            if (nlo > lo) span(p, y, (int)(it->a + lo), (int)(it->a + nlo - 1), it->colour);
            if (nhi < hi) span(p, y, (int)(it->a + nhi + 1), (int)(it->a + hi), it->colour);
        }
    }
}

static long long ceil_div(long long a, long long b) { return a >= 0 ? (a + b - 1) / b : -((-a) / b); }   /* b > 0 */

/* A line as a pure function of the major-axis step t = 0..du: the minor
 * offset is t * dv / du rounded half up. Only the steps whose pixel lies in
 * the clip are visited, so a band costs what it draws, however far away the
 * endpoints are, and every band agrees with a whole replay. */
static void line(const pen *p, const vds_item *it) {
    int steep = labs_(it->d - it->b) > labs_(it->c - it->a);
    long u0 = steep ? it->b : it->a, v0 = steep ? it->a : it->b, u1 = steep ? it->d : it->c, v1 = steep ? it->c : it->d;
    long du = labs_(u1 - u0), dv = labs_(v1 - v0);
    int su = u1 >= u0 ? 1 : -1, sv = v1 >= v0 ? 1 : -1;
    long ulo = steep ? p->clip.y : p->clip.x, uhi = ulo + (steep ? p->clip.h : p->clip.w) - 1;
    long vlo = steep ? p->clip.x : p->clip.y, vhi = vlo + (steep ? p->clip.w : p->clip.h) - 1;
    long long tlo = su > 0 ? ulo - u0 : u0 - uhi, thi = su > 0 ? uhi - u0 : u0 - ulo;   /* steps inside the clip's u range */
    long long klo = sv > 0 ? vlo - v0 : v0 - vhi, khi = sv > 0 ? vhi - v0 : v0 - vlo;   /* minor offsets inside its v range */
    if (tlo < 0) tlo = 0;
    if (thi > du) thi = du;
    if (!dv) {
        if (klo > 0 || khi < 0) return;
    } else {                                    /* offset k at step t lies in [klo, khi] */
        long long a = ceil_div((2 * klo - 1) * du, 2LL * dv), b = ceil_div((2 * khi + 1) * du, 2LL * dv) - 1;
        if (a > tlo) tlo = a;
        if (b < thi) thi = b;
    }
    for (long long t = tlo; t <= thi; ++t) {
        long k = du ? (long)((2 * t * dv + du) / (2LL * du)) : 0;
        long u = u0 + su * (long)t, v = v0 + sv * k;
        plot(p, (int)(steep ? v : u), (int)(steep ? u : v), it->colour);
    }
}

static void draw(const pen *p, const vds_item *it) {
    uint8_t c = it->colour;
    switch (it->kind) {
    case VDS_DRAW_DOT: plot(p, it->a, it->b, c); break;
    case VDS_DRAW_LINE: line(p, it); break;
    case VDS_DRAW_RECT:
        if (it->c <= 0 || it->d <= 0) break;
        span(p, it->b, it->a, it->a + it->c - 1, c);
        span(p, it->b + it->d - 1, it->a, it->a + it->c - 1, c);
        {
            long y0 = it->b + 1L, y1 = (long)it->b + it->d - 1;     /* sides: only rows inside the clip */
            if (y0 < p->clip.y) y0 = p->clip.y;
            if (y1 > p->clip.y + p->clip.h) y1 = p->clip.y + p->clip.h;
            for (long y = y0; y < y1; ++y) { plot(p, it->a, (int)y, c); plot(p, (int)(it->a + it->c - 1L), (int)y, c); }
        }
        break;
    case VDS_DRAW_BAR: {
        vds_rect r = clip(it->a, it->b, it->c, it->d, p->clip);
        for (int y = r.y; y < r.y + r.h; ++y) span(p, y, r.x, r.x + r.w - 1, c);
        break;
    }
    case VDS_DRAW_STIPPLE: {
        vds_rect r = clip(it->a, it->b, it->c, it->d, p->clip);
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x) plot(p, x, y, ((x + y) & 1) ? it->extra : c);
        break;
    }
    case VDS_DRAW_OVAL: ellipse(p, it, 0); break;
    case VDS_DRAW_DISC: ellipse(p, it, 1); break;
    case VDS_DRAW_TEXT:
        for (unsigned k = 0; k < it->n; ++k) {
            int gx = it->a + (int)k * 8;
            if (gx >= p->clip.x + p->clip.w || gx + 8 <= p->clip.x) continue;
            const uint8_t *g = p->v->font + (uint8_t)it->text[k] * 8;
            for (int r = 0; r < 8; ++r)
                for (int b = 0; b < 8; ++b)
                    if (g[r] & (0x80 >> b)) plot(p, gx + b, it->b + r, c);
        }
        break;
    case VDS_DRAW_ICON: {
        const vds_bitmap *bm = it->extra < VDS_MAX_BITMAPS ? &p->v->bitmaps[it->extra] : 0;
        if (!bm || !bm->used) break;
        vds_rect r = clip(it->a, it->b, bm->w, bm->h, p->clip);
        for (int y = r.y; y < r.y + r.h; ++y) {
            const uint8_t *row = bm->pixels + (size_t)(y - it->b) * stride(bm->w);
            for (int x = r.x; x < r.x + r.w; ++x) {
                int i = x - it->a;
                uint8_t px = (i & 1) ? row[i >> 1] & 15 : row[i >> 1] >> 4;
                if (px != bm->transparent) plot(p, x, y, px);
            }
        }
        break;
    }
    }
}

vds_rect vds_canvas_item_box(const vds *v, uint8_t id, const vds_item *it) {
    const vds_canvas *c = ccanvas(v, id);
    if (!c || !it) return rect(0, 0, 0, 0);
    vds_rect all = whole(c);
    switch (it->kind) {
    case VDS_DRAW_DOT: return clip(it->a, it->b, 1, 1, all);
    case VDS_DRAW_LINE: {
        long x0 = it->a < it->c ? it->a : it->c, y0 = it->b < it->d ? it->b : it->d;
        long x1 = it->a < it->c ? it->c : it->a, y1 = it->b < it->d ? it->d : it->b;
        return clip(x0, y0, x1 - x0 + 1, y1 - y0 + 1, all);
    }
    case VDS_DRAW_RECT: case VDS_DRAW_BAR: case VDS_DRAW_OVAL: case VDS_DRAW_DISC: case VDS_DRAW_STIPPLE:
        return clip(it->a, it->b, it->c, it->d, all);
    case VDS_DRAW_TEXT: return clip(it->a, it->b, 8L * it->n, 8, all);
    case VDS_DRAW_ICON: {
        const vds_bitmap *bm = it->extra < VDS_MAX_BITMAPS ? &v->bitmaps[it->extra] : 0;
        return bm && bm->used ? clip(it->a, it->b, bm->w, bm->h, all) : rect(0, 0, 0, 0);
    }
    }
    return rect(0, 0, 0, 0);
}

void vds_canvas_replay(const vds *v, uint8_t id, vds_rect r, uint8_t *out) {
    const vds_canvas *c = ccanvas(v, id);
    if (!c || !out) return;
    pen p;
    p.v = v; p.out = out; p.stride = stride(c->w);
    p.clip = clip(r.x, r.y, r.w, r.h, whole(c));
    if (empty(p.clip)) return;
    for (int y = p.clip.y; y < p.clip.y + p.clip.h; ++y) span(&p, y, p.clip.x, p.clip.x + p.clip.w - 1, c->bg);
    const vds_item *list = items(c);
    for (unsigned i = 0; i < VDS_CANVAS_ITEMS; ++i) {
        if (!list[i].kind) continue;
        vds_rect b = vds_canvas_item_box(v, id, &list[i]);
        if (!empty(clip(b.x, b.y, b.w, b.h, p.clip))) draw(&p, &list[i]);
    }
}

/* ------------------------------------------------------------ canvases */

static void dirty(vds_canvas *c, vds_rect r) {
    r = clip(r.x, r.y, r.w, r.h, whole(c));
    if (!empty(r)) c->dirty = unite(c->dirty, r);
}

static void show_change(vds *v, uint8_t id) {           /* what the screen shows of it changes */
    vds_canvas *c = &v->canvases[id];
    if ((c->flags & (VDS_CV_VISIBLE | VDS_CV_READY)) == (VDS_CV_VISIBLE | VDS_CV_READY)) vds_damage_canvas(v, id, whole(c));
}

int vds_canvas_valid(const vds *v, uint8_t id, uint8_t surface, int16_t x, int16_t w, int16_t h, uint8_t bg, uint8_t flags) {
    if (id >= VDS_MAX_CANVASES || surface >= VDS_MAX_SURFACES || (x & 7) || bg > 15 ||
        w <= 0 || h <= 0 || w > VDS_MAX_WIDTH || h > 480 || (flags & ~(VDS_CV_VISIBLE | VDS_CV_CLEAR)))
        return 0;
    uint32_t raster = stride(w) * (uint32_t)h;
    for (uint8_t k = 0; k < VDS_MAX_CANVASES; ++k)
        if (k != id && v->canvases[k].used) raster += stride(v->canvases[k].w) * (uint32_t)v->canvases[k].h;
    return raster <= VDS_CANVAS_RASTER_MAX;
}

int vds_canvas_create(vds *v, uint8_t id, uint8_t surface, int16_t x, int16_t y, int16_t w, int16_t h,
                      uint8_t bg, uint8_t flags, void *memory) {
    if (!memory || !vds_canvas_valid(v, id, surface, x, w, h, bg, flags)) return 0;
    vds_canvas *c = &v->canvases[id];
    if (c->used) show_change(v, id);
    memset(memory, 0, ITEMS_BYTES);
    c->used = 1;
    c->flags = flags;                                    /* not READY until first rasterised */
    c->surface = surface;
    c->bg = bg;
    c->x = x; c->y = y; c->w = w; c->h = h;
    c->items = memory;
    c->pixels = (uint8_t *)memory + ITEMS_BYTES;
    c->dirty = whole(c);
    return 1;
}

int vds_canvas_move(vds *v, uint8_t id, int16_t x, int16_t y) {
    vds_canvas *c = canvas(v, id);
    if (!c || (x & 7)) return 0;
    show_change(v, id);
    c->x = x; c->y = y;
    show_change(v, id);
    return 1;
}

int vds_canvas_flags(vds *v, uint8_t id, uint8_t flags) {
    vds_canvas *c = canvas(v, id);
    if (!c || (flags & ~(VDS_CV_VISIBLE | VDS_CV_CLEAR))) return 0;
    show_change(v, id);
    c->flags = (uint8_t)((c->flags & VDS_CV_READY) | flags);
    show_change(v, id);
    return 1;
}

int vds_canvas_delete(vds *v, uint8_t id) {
    vds_canvas *c = canvas(v, id);
    if (!c) return 0;
    show_change(v, id);
    memset(c, 0, sizeof *c);
    return 1;
}

int vds_canvas_set(vds *v, uint8_t id, uint8_t item, const vds_item *it) {
    vds_canvas *c = canvas(v, id);
    if (!c || !it || item >= VDS_CANVAS_ITEMS || !it->kind || it->kind >= VDS_DRAW_KINDS || it->colour > 15 ||
        (it->kind == VDS_DRAW_TEXT && it->n > VDS_CANVAS_TEXT_MAX) ||
        (it->kind == VDS_DRAW_ICON && it->extra >= VDS_MAX_BITMAPS) || (it->kind == VDS_DRAW_STIPPLE && it->extra > 15))
        return 0;
    vds_item *slot = &items(c)[item];
    if (slot->kind) dirty(c, vds_canvas_item_box(v, id, slot));
    *slot = *it;
    if (slot->kind != VDS_DRAW_TEXT) slot->n = 0;
    dirty(c, vds_canvas_item_box(v, id, slot));
    return 1;
}

int vds_canvas_erase(vds *v, uint8_t id, uint8_t item) {
    vds_canvas *c = canvas(v, id);
    if (!c) return 0;
    if (item == 255) {
        memset(c->items, 0, ITEMS_BYTES);
        dirty(c, whole(c));
        return 1;
    }
    if (item >= VDS_CANVAS_ITEMS || !items(c)[item].kind) return 0;
    dirty(c, vds_canvas_item_box(v, id, &items(c)[item]));
    memset(&items(c)[item], 0, sizeof(vds_item));
    return 1;
}

int vds_canvas_shift(vds *v, uint8_t id, uint8_t item, int16_t dx, int16_t dy) {
    vds_canvas *c = canvas(v, id);
    if (!c || item >= VDS_CANVAS_ITEMS || !items(c)[item].kind) return 0;
    vds_item *it = &items(c)[item];
    dirty(c, vds_canvas_item_box(v, id, it));
    it->a = (int16_t)(it->a + dx);
    it->b = (int16_t)(it->b + dy);
    if (it->kind == VDS_DRAW_LINE) { it->c = (int16_t)(it->c + dx); it->d = (int16_t)(it->d + dy); }
    dirty(c, vds_canvas_item_box(v, id, it));
    return 1;
}

int vds_canvas_ink(vds *v, uint8_t id, uint8_t item, uint8_t colour) {
    vds_canvas *c = canvas(v, id);
    if (!c || item >= VDS_CANVAS_ITEMS || !items(c)[item].kind || colour > 15) return 0;
    items(c)[item].colour = colour;
    dirty(c, vds_canvas_item_box(v, id, &items(c)[item]));
    return 1;
}

const vds_item *vds_canvas_item(const vds *v, uint8_t id, uint8_t item) {
    const vds_canvas *c = ccanvas(v, id);
    return c && item < VDS_CANVAS_ITEMS && items(c)[item].kind ? &items(c)[item] : 0;
}

uint8_t vds_canvas_hit(const vds *v, uint8_t id, int x, int y) {
    const vds_canvas *c = ccanvas(v, id);
    if (!c) return 255;
    for (int i = VDS_CANVAS_ITEMS - 1; i >= 0; --i)
        if (items(c)[i].kind && contains(vds_canvas_item_box(v, id, &items(c)[i]), x, y)) return (uint8_t)i;
    return 255;
}

/* ------------------------------------------------------------ bitmaps */

static void bitmap_users(vds *v, uint8_t id) {           /* redraw the canvases that show bitmap id */
    for (uint8_t k = 0; k < VDS_MAX_CANVASES; ++k) {
        vds_canvas *c = &v->canvases[k];
        if (!c->used) continue;
        for (unsigned i = 0; i < VDS_CANVAS_ITEMS; ++i)
            if (items(c)[i].kind == VDS_DRAW_ICON && items(c)[i].extra == id) { dirty(c, whole(c)); break; }
    }
}

int vds_bitmap_define(vds *v, uint8_t id, uint8_t w, uint8_t h, uint8_t transparent, const uint8_t *pixels) {
    if (id >= VDS_MAX_BITMAPS || !pixels || !w || !h || w > VDS_BITMAP_MAX || h > VDS_BITMAP_MAX) return 0;
    vds_bitmap *b = &v->bitmaps[id];
    b->used = 1; b->w = w; b->h = h; b->transparent = transparent; b->pixels = pixels;
    bitmap_users(v, id);
    return 1;
}

void vds_bitmap_changed(vds *v, uint8_t id) {
    if (id < VDS_MAX_BITMAPS && v->bitmaps[id].used) bitmap_users(v, id);
}

void vds_bitmap_undefine(vds *v, uint8_t id) {
    if (id >= VDS_MAX_BITMAPS || !v->bitmaps[id].used) return;
    bitmap_users(v, id);                                /* before: the old boxes are dirtied */
    memset(&v->bitmaps[id], 0, sizeof v->bitmaps[id]);
}

void vds_canvas_icons(vds *v) {
    for (uint8_t i = 0; i < 8; ++i)
        vds_bitmap_define(v, (uint8_t)(VDS_ICON_FIRST + i), VDS_ICON_SIZE, VDS_ICON_SIZE, VDS_ICON_TRANSPARENT, vds_icons[i]);
}

/* ------------------------------------------------------------ frames */

int vds_canvas_pending(const vds *v) {
    for (uint8_t k = 0; k < VDS_MAX_CANVASES; ++k)
        if (v->canvases[k].used && !empty(v->canvases[k].dirty)) return 1;
    return 0;
}

#define BAND_MIN 8                                      /* rows per replay, at least */

uint32_t vds_canvas_prepare(vds *v, uint32_t budget) { return vds_canvas_prepare_within(v, budget, UINT32_MAX); }

uint32_t vds_canvas_prepare_within(vds *v, uint32_t budget, uint32_t budget_us) {
    int timed = v->clock_us && budget_us != UINT32_MAX;
    uint32_t started = timed ? v->clock_us(v->clock_ctx) : 0, done = 0;
    for (uint8_t k = 0; k < VDS_MAX_CANVASES; ++k) {
        vds_canvas *c = &v->canvases[k];
        while (c->used && !empty(c->dirty)) {
            if (done && (done >= budget || (timed && (uint32_t)(v->clock_us(v->clock_ctx) - started) >= budget_us))) return done;
            vds_rect d = c->dirty;
            uint32_t left = budget > done ? budget - done : 0;
            if (left > VDS_CANVAS_BAND_PX) left = VDS_CANVAS_BAND_PX;
            long rows = (long)(left / (uint32_t)d.w);
            if (rows < BAND_MIN) rows = BAND_MIN;
            if (rows > d.h) rows = d.h;
            vds_rect band = rect(d.x, d.y, d.w, rows);
            vds_canvas_replay(v, k, band, c->pixels);
            done += (uint32_t)band.w * (uint32_t)band.h;
            c->dirty.y = (int16_t)(d.y + rows);
            c->dirty.h = (int16_t)(d.h - rows);
            if (empty(c->dirty)) c->dirty = rect(0, 0, 0, 0);
            if (c->flags & VDS_CV_READY) {
                if (c->flags & VDS_CV_VISIBLE) vds_damage_canvas(v, k, band);
            } else if (empty(c->dirty)) {
                c->flags |= VDS_CV_READY;                   /* first full raster: show it */
                show_change(v, k);
            }
        }
    }
    return done;
}

/* ------------------------------------------------------------ icons */

/* Built-in 16x16 icons (bitmaps 24..31), colours 0-3 so they look the same in
 * both styles, 15 transparent. Art source: tools/vds_icons.py. */
const uint8_t vds_icons[8][VDS_ICON_SIZE / 2 * VDS_ICON_SIZE] = {
    {   /* document */
        0xFF, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xFF, 0xFF, 0xFF, 0x03, 0x33, 0x33, 0x33, 0x00, 0xFF, 0xFF,
        0xFF, 0x03, 0x33, 0x33, 0x33, 0x03, 0x0F, 0xFF, 0xFF, 0x03, 0x33, 0x33, 0x33, 0x00, 0x00, 0xFF,
        0xFF, 0x03, 0x00, 0x00, 0x03, 0x33, 0x30, 0xFF, 0xFF, 0x03, 0x33, 0x33, 0x33, 0x33, 0x30, 0xFF,
        0xFF, 0x03, 0x00, 0x00, 0x00, 0x00, 0x30, 0xFF, 0xFF, 0x03, 0x33, 0x33, 0x33, 0x33, 0x30, 0xFF,
        0xFF, 0x03, 0x00, 0x00, 0x00, 0x00, 0x30, 0xFF, 0xFF, 0x03, 0x33, 0x33, 0x33, 0x33, 0x30, 0xFF,
        0xFF, 0x03, 0x00, 0x00, 0x00, 0x00, 0x30, 0xFF, 0xFF, 0x03, 0x33, 0x33, 0x33, 0x33, 0x30, 0xFF,
        0xFF, 0x03, 0x00, 0x00, 0x03, 0x33, 0x30, 0xFF, 0xFF, 0x03, 0x33, 0x33, 0x33, 0x33, 0x30, 0xFF,
        0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    },
    {   /* folder */
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xF0, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0x22, 0x22, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF,
        0x02, 0x22, 0x22, 0x20, 0x00, 0x00, 0x00, 0x0F, 0x02, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x0F,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x20,
        0x02, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x20, 0x02, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x20,
        0x02, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x20, 0x02, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x20,
        0x02, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    },
    {   /* word */
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F,
        0xF0, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x0F, 0xF0, 0x30, 0x03, 0x33, 0x33, 0x33, 0x33, 0x0F,
        0xF0, 0x30, 0x03, 0x30, 0x00, 0x33, 0x33, 0x0F, 0xF0, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x0F,
        0xF0, 0x33, 0x00, 0x30, 0x00, 0x00, 0x33, 0x0F, 0xF0, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x0F,
        0xF0, 0x33, 0x00, 0x00, 0x30, 0x03, 0x33, 0x0F, 0xF0, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x0F,
        0xF0, 0x33, 0x00, 0x30, 0x00, 0x30, 0x03, 0x0F, 0xF0, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x0F,
        0xF0, 0x30, 0x03, 0x33, 0x33, 0x33, 0x33, 0x0F, 0xF0, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x0F,
        0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    },
    {   /* module */
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0x00, 0x33, 0x33, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x33, 0x33, 0x33, 0x33, 0x00, 0xFF,
        0xF0, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x0F, 0xF0, 0x00, 0x33, 0x33, 0x33, 0x33, 0x00, 0x0F,
        0xF0, 0x22, 0x00, 0x33, 0x33, 0x00, 0x11, 0x0F, 0xF0, 0x22, 0x22, 0x00, 0x00, 0x11, 0x11, 0x0F,
        0xF0, 0x22, 0x22, 0x22, 0x01, 0x11, 0x11, 0x0F, 0xF0, 0x22, 0x22, 0x22, 0x01, 0x11, 0x11, 0x0F,
        0xF0, 0x22, 0x22, 0x22, 0x01, 0x11, 0x11, 0x0F, 0xF0, 0x22, 0x22, 0x22, 0x01, 0x11, 0x11, 0x0F,
        0xFF, 0x00, 0x22, 0x22, 0x01, 0x11, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x22, 0x01, 0x00, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    },
    {   /* info */
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x0F, 0xFF, 0xFF,
        0xFF, 0xF0, 0x03, 0x33, 0x33, 0x30, 0x0F, 0xFF, 0xFF, 0x03, 0x33, 0x30, 0x03, 0x33, 0x30, 0xFF,
        0xF0, 0x33, 0x33, 0x30, 0x03, 0x33, 0x33, 0x0F, 0xF0, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x0F,
        0x03, 0x33, 0x33, 0x00, 0x03, 0x33, 0x33, 0x30, 0x03, 0x33, 0x33, 0x30, 0x03, 0x33, 0x33, 0x30,
        0x03, 0x33, 0x33, 0x30, 0x03, 0x33, 0x33, 0x30, 0x03, 0x33, 0x33, 0x30, 0x03, 0x33, 0x33, 0x30,
        0xF0, 0x33, 0x33, 0x30, 0x03, 0x33, 0x33, 0x0F, 0xF0, 0x33, 0x33, 0x00, 0x00, 0x33, 0x33, 0x0F,
        0xFF, 0x03, 0x33, 0x33, 0x33, 0x33, 0x30, 0xFF, 0xFF, 0xF0, 0x03, 0x33, 0x33, 0x30, 0x0F, 0xFF,
        0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    },
    {   /* warning */
        0xFF, 0xFF, 0xFF, 0xF0, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0x20, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0x02, 0x20, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0x22, 0x22, 0x0F, 0xFF, 0xFF,
        0xFF, 0xFF, 0xF0, 0x20, 0x02, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0x20, 0x02, 0x20, 0xFF, 0xFF,
        0xFF, 0xFF, 0x02, 0x20, 0x02, 0x20, 0xFF, 0xFF, 0xFF, 0xF0, 0x22, 0x20, 0x02, 0x22, 0x0F, 0xFF,
        0xFF, 0xF0, 0x22, 0x20, 0x02, 0x22, 0x0F, 0xFF, 0xFF, 0x02, 0x22, 0x20, 0x02, 0x22, 0x20, 0xFF,
        0xFF, 0x02, 0x22, 0x22, 0x22, 0x22, 0x20, 0xFF, 0xF0, 0x22, 0x22, 0x20, 0x02, 0x22, 0x22, 0x0F,
        0xF0, 0x22, 0x22, 0x20, 0x02, 0x22, 0x22, 0x0F, 0x02, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x20,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    },
    {   /* error */
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x0F, 0xFF, 0xFF,
        0xFF, 0xF0, 0x01, 0x11, 0x11, 0x10, 0x0F, 0xFF, 0xFF, 0x01, 0x11, 0x11, 0x11, 0x11, 0x10, 0xFF,
        0xF0, 0x11, 0x31, 0x11, 0x11, 0x13, 0x11, 0x0F, 0xF0, 0x13, 0x33, 0x11, 0x11, 0x33, 0x31, 0x0F,
        0x01, 0x11, 0x33, 0x31, 0x13, 0x33, 0x11, 0x10, 0x01, 0x11, 0x13, 0x33, 0x33, 0x31, 0x11, 0x10,
        0x01, 0x11, 0x11, 0x33, 0x33, 0x11, 0x11, 0x10, 0x01, 0x11, 0x13, 0x33, 0x33, 0x31, 0x11, 0x10,
        0x01, 0x11, 0x33, 0x31, 0x13, 0x33, 0x11, 0x10, 0xF0, 0x13, 0x33, 0x11, 0x11, 0x33, 0x31, 0x0F,
        0xF0, 0x11, 0x31, 0x11, 0x11, 0x13, 0x11, 0x0F, 0xFF, 0x01, 0x11, 0x11, 0x11, 0x11, 0x10, 0xFF,
        0xFF, 0xF0, 0x01, 0x11, 0x11, 0x10, 0x0F, 0xFF, 0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x0F, 0xFF, 0xFF,
    },
    {   /* disk */
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F,
        0xF0, 0x00, 0x33, 0x33, 0x33, 0x33, 0x00, 0x0F, 0xF0, 0x00, 0x33, 0x33, 0x33, 0x03, 0x00, 0x0F,
        0xF0, 0x00, 0x33, 0x33, 0x33, 0x03, 0x00, 0x0F, 0xF0, 0x00, 0x33, 0x33, 0x33, 0x33, 0x00, 0x0F,
        0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F,
        0xF0, 0x03, 0x33, 0x33, 0x33, 0x33, 0x30, 0x0F, 0xF0, 0x03, 0x00, 0x00, 0x00, 0x00, 0x30, 0x0F,
        0xF0, 0x03, 0x33, 0x33, 0x33, 0x33, 0x30, 0x0F, 0xF0, 0x03, 0x00, 0x00, 0x00, 0x00, 0x30, 0x0F,
        0xF0, 0x03, 0x33, 0x33, 0x33, 0x33, 0x30, 0x0F, 0xF0, 0x03, 0x33, 0x33, 0x33, 0x33, 0x30, 0x0F,
        0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    },
};

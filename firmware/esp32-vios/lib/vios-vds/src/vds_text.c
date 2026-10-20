/* VIOS Display Server TextView (G4, kind VDS_WK_TEXT): a multi-line text
 * edited on the ESP, as the Input widget is. The CPU sets the text, reads it
 * back (op 8C) and decides what an accept or an evaluation means; the ESP does
 * cursor movement, selection, scrolling, editing and a local clipboard within
 * a frame. Lines end in '\n'; '\r' is never stored. No line table: rendering
 * scans from the cached offset of the first visible line. See vds_ui.h. */
#include "vds_ui.h"
#include <string.h>

static vds *core(vds_ui *u) { return u->p->v; }
static uint8_t attr(uint8_t fg, uint8_t bg) { return (uint8_t)(fg | bg << 4); }

static vds_text *text_of(vds_ui *u, uint8_t id) {
    if (id >= VDS_MAX_WIDGETS || !u->widgets[id].used || u->widgets[id].kind != VDS_WK_TEXT) return 0;
    uint8_t slot = u->widgets[id].slot;
    return slot < VDS_TEXT_SLOTS && u->texts[slot].used ? &u->texts[slot] : 0;
}
const vds_text *vds_ui_text(vds_ui *u, uint8_t id) { return text_of(u, id); }

void vds_ui_text_release(vds_ui *u, uint8_t id) {
    uint8_t slot = u->widgets[id].slot;
    if (slot < VDS_TEXT_SLOTS && u->texts[slot].widget == id) u->texts[slot].used = 0;
    u->widgets[id].slot = VDS_NONE;
}

static uint16_t line_start(const vds_text *t, uint16_t at) {
    while (at && t->buf[at - 1] != '\n') --at;
    return at;
}
static uint16_t line_end(const vds_text *t, uint16_t at) {
    while (at < t->len && t->buf[at] != '\n') ++at;
    return at;
}
static unsigned lines_between(const vds_text *t, uint16_t from, uint16_t to) {   /* from <= to */
    unsigned n = 0;
    for (uint16_t i = from; i < to; ++i) n += t->buf[i] == '\n';
    return n;
}
static uint16_t sel_lo(const vds_text *t) { return t->caret < t->anchor ? t->caret : t->anchor; }
static uint16_t sel_hi(const vds_text *t) { return t->caret < t->anchor ? t->anchor : t->caret; }

/* Keep the caret inside the view: first visible line and left column. */
static void scroll_to_caret(const vds_widget *w, vds_text *t) {
    if (t->top > t->len) t->top = 0;
    t->top = line_start(t, t->top);
    if (t->caret < t->top) t->top = line_start(t, t->caret);
    unsigned rows = w->h ? w->h : 1;
    while (lines_between(t, t->top, t->caret) >= rows) t->top = (uint16_t)(line_end(t, t->top) + 1);
    unsigned col = (unsigned)(t->caret - line_start(t, t->caret)), cols = w->w ? w->w : 1;
    if (col < t->left) t->left = (uint16_t)col;
    if (col >= t->left + cols) t->left = (uint16_t)(col - cols + 1);
}

void vds_ui_text_render(vds_ui *u, uint8_t id) {
    vds *v = core(u);
    vds_widget *w = &u->widgets[id];
    vds_text *t = text_of(u, id);
    uint8_t s = w->window < VDS_MAX_WINDOWS && v->windows[w->window].used ? v->windows[w->window].surface : VDS_NONE;
    if (u->deferring) { w->pending = 1; u->pending_any = 1; return; }
    w->pending = 0;
    if (!t || s >= VDS_MAX_SURFACES || !v->surfaces[s].cells || !w->w || !w->h) return;
    const vds_style *st = &v->style;
    int focused = u->widget_focus[w->window] == id && u->focus == w->window;
    uint8_t base = attr(st->title_fg, st->body), sel = focused ? attr(st->menu_sel_fg, st->menu_sel_bg) : attr(st->menu_fg, st->desktop_a);
    uint16_t lo = sel_lo(t), hi = sel_hi(t);
    scroll_to_caret(w, t);
    char run[VDS_MAX_WIDTH / 8];
    uint16_t at = t->top;
    int caret_row = -1, caret_col = 0;
    for (int r = 0; r < w->h; ++r) {
        int has_line = at <= t->len && (r == 0 || (at > 0 && t->buf[at - 1] == '\n'));
        uint16_t end = has_line ? line_end(t, at) : at;
        if (has_line && t->caret >= at && t->caret <= end) { caret_row = r; caret_col = t->caret - at - t->left; }
        /* Runs of one colour: the selection, and the text around it. */
        int c = 0;
        if (w->row + r >= v->surfaces[s].rows) break;            /* clipped to the surface */
        while (c < w->w && w->col + c < v->surfaces[s].cols) {
            uint16_t i = (uint16_t)(at + t->left + c);
            int chosen = has_line && i >= lo && i < hi && (i < end || (i == end && c == (int)(end - at - t->left)));
            uint8_t a = chosen ? sel : base;
            int n = 0;
            while (c + n < w->w && n < (int)sizeof run) {
                uint16_t j = (uint16_t)(at + t->left + c + n);
                int jt = has_line && j < end, js = has_line && j >= lo && j < hi && (j < end || (j == end && c + n == (int)(end - at - t->left)));
                if ((js ? sel : base) != a) break;
                char ch = jt ? t->buf[j] : ' ';
                run[n++] = ch == '\t' || (unsigned char)ch < 32 ? ' ' : ch;
            }
            vds_put(v, s, (uint8_t)(w->col + c), (uint8_t)(w->row + r), a, run, (size_t)n);
            c += n;
        }
        at = has_line && end < t->len ? (uint16_t)(end + 1) : (uint16_t)(t->len + 1);
    }
    if (focused && caret_row >= 0 && caret_col >= 0 && caret_col < w->w) {
        vds_cursor(v, s, (uint8_t)(w->col + caret_col), (uint8_t)(w->row + caret_row), VDS_CURSOR_BAR, 1);
        u->caret_widget = id;
    } else if (u->caret_widget == id) {
        vds_cursor(v, s, 0, 0, VDS_CURSOR_OFF, 0);
        u->caret_widget = VDS_NONE;
    }
}

/* ------------------------------------------------------------ editing */

static void edited(vds_ui *u, uint8_t id, vds_text *t, int by_user) {
    ++t->rev;
    if (by_user && !t->dirty) {
        t->dirty = 1;
        uint8_t a[3] = {1, 0, u->widgets[id].window};
        vds_proto_event(u->p, VDS_EVENT_CHANGE, id, a, 3);
    }
}

/* Replace the selection with n bytes ('\r' dropped). 0 if it does not fit. */
static int replace(vds_text *t, const char *text, size_t n) {
    uint16_t lo = sel_lo(t), hi = sel_hi(t);
    size_t add = 0;
    for (size_t i = 0; i < n; ++i) add += text[i] != '\r';
    if (t->len - (hi - lo) + add > VDS_TEXT_MAX) return 0;
    memmove(t->buf + lo + add, t->buf + hi, (size_t)(t->len - hi));
    for (size_t i = 0, k = lo; i < n; ++i) if (text[i] != '\r') t->buf[k++] = text[i];
    t->len = (uint16_t)(t->len - (hi - lo) + add);
    t->anchor = lo;
    t->caret = (uint16_t)(lo + add);
    return 1;
}

int vds_ui_text_set(vds_ui *u, uint8_t id, int append, const char *text, size_t n) {
    vds_text *t = text_of(u, id);
    if (!t) return 0;
    if (!append) { t->len = t->caret = t->anchor = t->top = t->left = 0; t->dirty = 0; }
    uint16_t caret = t->caret, anchor = t->caret == t->anchor ? t->caret : t->anchor;
    t->caret = t->anchor = t->len;
    int ok = replace(t, text, n);
    t->caret = append ? caret : 0;
    t->anchor = append ? anchor : 0;
    edited(u, id, t, 0);
    vds_ui_text_render(u, id);
    return ok;
}

int vds_ui_text_select(vds_ui *u, uint8_t id, uint16_t caret, uint16_t anchor) {
    vds_text *t = text_of(u, id);
    if (!t) return 0;
    t->caret = caret > t->len ? t->len : caret;
    t->anchor = anchor > t->len ? t->len : anchor;
    vds_ui_text_render(u, id);
    return 1;
}

int vds_ui_text_insert(vds_ui *u, uint8_t id, const char *text, size_t n) {
    vds_text *t = text_of(u, id);
    if (!t || !replace(t, text, n)) return 0;
    edited(u, id, t, 0);
    vds_ui_text_render(u, id);
    return 1;
}

static void text_event(vds_ui *u, uint8_t id, vds_text *t, uint8_t what) {
    uint16_t lo = sel_lo(t), hi = sel_hi(t);
    uint8_t a[6] = {what, (uint8_t)lo, (uint8_t)(lo >> 8), (uint8_t)hi, (uint8_t)(hi >> 8), u->widgets[id].window};
    vds_proto_event(u->p, VDS_EVENT_TEXT, id, a, 6);
}

/* Up/Down: the same column on the neighbouring line, clamped to its length. */
static uint16_t vertical(const vds_text *t, uint16_t at, int lines) {
    uint16_t start = line_start(t, at), col = (uint16_t)(at - start);
    while (lines < 0 && start) { start = line_start(t, (uint16_t)(start - 1)); ++lines; }
    while (lines > 0) {
        uint16_t end = line_end(t, start);
        if (end >= t->len) break;
        start = (uint16_t)(end + 1);
        --lines;
    }
    uint16_t end = line_end(t, start);
    return (uint16_t)(start + col > end ? end : start + col);
}

/* Ctrl+[ / Ctrl+]: two spaces more / fewer at the start of every line the
 * selection touches (the caret's line without one). */
static int indent(vds_text *t, int add) {
    uint16_t hi = sel_hi(t), lo = sel_lo(t), ls = line_start(t, lo);
    int keep = t->caret != t->anchor && lo == ls;              /* a selection from a line start keeps it */
    if (hi > sel_lo(t) && hi == line_start(t, hi)) --hi;       /* a selection ending at a line start */
    int changed = 0;
    for (;;) {
        unsigned k = 0;
        if (add) { if (t->len + 2 > VDS_TEXT_MAX) break; k = 2; memmove(t->buf + ls + 2, t->buf + ls, (size_t)(t->len - ls)); t->buf[ls] = t->buf[ls + 1] = ' '; t->len = (uint16_t)(t->len + 2); }
        else { while (k < 2 && ls + k < t->len && t->buf[ls + k] == ' ') ++k; memmove(t->buf + ls, t->buf + ls + k, (size_t)(t->len - ls - k)); t->len = (uint16_t)(t->len - k); }
        if (k) {
            changed = 1;
            uint16_t *pos[2] = {&t->caret, &t->anchor};
            for (unsigned i = 0; i < 2; ++i)
                if (add ? *pos[i] > ls || (*pos[i] == ls && !(keep && *pos[i] == lo)) : *pos[i] > ls) *pos[i] = (uint16_t)(add ? *pos[i] + 2 : *pos[i] - (*pos[i] - ls < k ? *pos[i] - ls : k));
            hi = (uint16_t)(add ? hi + 2 : hi - k);
        }
        uint16_t end = line_end(t, ls);
        if (end >= t->len || end + 1 > hi) break;
        ls = (uint16_t)(end + 1);
    }
    return changed;
}

int vds_ui_text_key(vds_ui *u, uint8_t id, uint8_t ascii, uint8_t modifiers, uint8_t vk) {
    vds_text *t = text_of(u, id);
    vds_widget *w = &u->widgets[id];
    if (!t) return 0;
    int ctrl = (modifiers & VDS_MOD_CTRL) != 0, shift = (modifiers & VDS_MOD_SHIFT) != 0;
    int ro = (w->flags & VDS_WF_READONLY) != 0;
    uint8_t letter = ctrl && ascii >= 1 && ascii <= 26 ? (uint8_t)(ascii + 'a' - 1)
                   : ascii >= 'A' && ascii <= 'Z' ? (uint8_t)(ascii + 32) : ascii;
    int moved = 1;
    uint16_t to = t->caret;
    switch (vk) {
    case VDS_VK_LEFT: case VDS_VK_KP_LEFT:
        to = !shift && t->caret != t->anchor ? sel_lo(t) : t->caret ? (uint16_t)(t->caret - 1) : 0; break;
    case VDS_VK_RIGHT: case VDS_VK_KP_RIGHT:
        to = !shift && t->caret != t->anchor ? sel_hi(t) : t->caret < t->len ? (uint16_t)(t->caret + 1) : t->len; break;
    case VDS_VK_UP: case VDS_VK_KP_UP: to = vertical(t, t->caret, -1); break;
    case VDS_VK_DOWN: case VDS_VK_KP_DOWN: to = vertical(t, t->caret, 1); break;
    case VDS_VK_PAGEUP: case VDS_VK_KP_PAGEUP: to = vertical(t, t->caret, -(int)(w->h > 1 ? w->h - 1 : 1)); break;
    case VDS_VK_PAGEDOWN: case VDS_VK_KP_PAGEDOWN: to = vertical(t, t->caret, w->h > 1 ? w->h - 1 : 1); break;
    case VDS_VK_HOME: case VDS_VK_KP_HOME: to = ctrl ? 0 : line_start(t, t->caret); break;
    case VDS_VK_END: case VDS_VK_KP_END: to = ctrl ? t->len : line_end(t, t->caret); break;
    default: moved = 0;
    }
    if (moved) {
        t->caret = to;
        if (!shift) t->anchor = to;
        vds_ui_text_render(u, id);
        return 1;
    }
    if (ctrl && vk != VDS_VK_ESCAPE && (ascii == 27 || ascii == '[' || ascii == 29 || ascii == ']')) {
        if (!ro && indent(t, ascii == 27 || ascii == '[')) edited(u, id, t, 1);
        vds_ui_text_render(u, id);
        return 1;
    }
    if (ctrl) {
        switch (letter) {
        case 'a': t->anchor = 0; t->caret = t->len; vds_ui_text_render(u, id); return 1;
        case 'c': case 'x': {
            uint16_t lo = sel_lo(t), hi = sel_hi(t);
            if (lo == hi) return 1;
            u->clip_len = (uint16_t)(hi - lo > VDS_CLIP_MAX ? VDS_CLIP_MAX : hi - lo);
            memcpy(u->clip, t->buf + lo, u->clip_len);
            if (letter == 'x' && !ro && replace(t, "", 0)) { edited(u, id, t, 1); vds_ui_text_render(u, id); }
            return 1;
        }
        case 'v':
            if (!ro && u->clip_len && replace(t, u->clip, u->clip_len)) { t->anchor = t->caret; edited(u, id, t, 1); vds_ui_text_render(u, id); }
            return 1;
        case 's': text_event(u, id, t, VDS_TEXT_ACCEPT); return 1;
        case 'd': case 'p': case 'i':
            if (t->caret == t->anchor) {                       /* nothing selected: the caret's line */
                t->anchor = line_start(t, t->caret);
                t->caret = line_end(t, t->caret);
                vds_ui_text_render(u, id);
            }
            text_event(u, id, t, letter == 'd' ? VDS_TEXT_DO : letter == 'p' ? VDS_TEXT_PRINT : VDS_TEXT_INSPECT);
            return 1;
        }
        return 0;
    }
    if (ro) return 0;
    int changed = 0;
    if (vk == VDS_VK_BACKSPACE || vk == VDS_VK_DELETE) {
        if (t->caret == t->anchor) {
            if (vk == VDS_VK_BACKSPACE && t->caret) --t->anchor;
            else if (vk == VDS_VK_DELETE && t->caret < t->len) ++t->anchor;
        }
        changed = t->caret != t->anchor && replace(t, "", 0);
    } else if (vk == VDS_VK_RETURN || vk == VDS_VK_KP_ENTER) changed = replace(t, "\n", 1);
    else if (ascii >= 32 && ascii <= 126) { char c = (char)ascii; changed = replace(t, &c, 1); }
    else return 0;
    if (changed) { t->anchor = t->caret; edited(u, id, t, 1); }
    vds_ui_text_render(u, id);
    return 1;
}

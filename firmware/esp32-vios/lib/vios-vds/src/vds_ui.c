/* VIOS Display Server intrinsics: see vds_ui.h. */
#include "vds_ui.h"
#include <string.h>

static vds *core(vds_ui *u) { return u->p->v; }

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

static void apply(vds *v, uint8_t id, vds_geometry g) {
    vds_window_move(v, id, g.x, g.y);
    vds_window_resize(v, id, g.cols, g.rows);
}

static void win_event(vds_ui *u, uint8_t what, uint8_t id) {
    vds *v = core(u);
    const vds_window *w = &v->windows[id];
    uint8_t args[7] = {what, (uint8_t)w->x, (uint8_t)((uint16_t)w->x >> 8), (uint8_t)w->y, (uint8_t)((uint16_t)w->y >> 8),
                       w->cols, w->rows};
    vds_proto_event(u->p, VDS_EVENT_WIN, id, args, sizeof args);
}

static int focus_hook(void *ui, uint8_t id) { return vds_ui_focus((vds_ui *)ui, id); }
static void forget_hook(void *ui, uint8_t id) { vds_ui_forget((vds_ui *)ui, id); }

void vds_ui_init(vds_ui *u, vds_proto *p) {
    memset(u, 0, sizeof *u);
    u->p = p;
    u->focus = VDS_NONE;
    p->ui = u;
    p->ui_focus = focus_hook;
    p->ui_forget = forget_hook;
}

void vds_ui_forget(vds_ui *u, uint8_t id) {
    if (id >= VDS_MAX_WINDOWS) return;
    if (u->focus == id) { u->focus = VDS_NONE; u->wm_mode = 0; }
    u->zoomed &= ~(1ul << id);
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
    if (previous != id) vds_proto_event(u->p, VDS_EVENT_FOCUS, id, &previous, 1);
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
    if (u->zoomed & (1ul << id)) {
        apply(v, id, u->zoom_saved[id]);
        u->zoomed &= ~(1ul << id);
        win_event(u, VDS_WIN_RESTORED, id);
        return;
    }
    u->zoom_saved[id] = geometry_of(v, id);
    int turbo = v->style.kind == VDS_STYLE_TURBO, top = turbo ? 8 : 12, inset = turbo ? 8 : 2;
    vds_geometry g = {8, (int16_t)top, (uint8_t)((v->width - 16) / 8), (uint8_t)((v->height - top - inset) / 8)};
    apply(v, id, g);
    u->zoomed |= 1ul << id;
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
        apply(v, id, u->wm_saved);
        u->wm_mode = 0;
        return 1;
    default:
        return 1;                                           /* the mode swallows other keys */
    }
    apply(v, id, g);
    return 1;
}

static int route(vds_ui *u, uint8_t ascii, uint8_t modifiers, uint8_t vk, int ctrl, int shift, int alt);

int vds_ui_key(vds_ui *u, uint8_t ascii, uint8_t modifiers, uint8_t vk, uint8_t down) {
    int ctrl = (modifiers & VDS_MOD_CTRL) != 0, shift = (modifiers & VDS_MOD_SHIFT) != 0;
    int alt = (modifiers & (VDS_MOD_LALT | VDS_MOD_RALT)) != 0;
    if (!down) {                                            /* a consumed press keeps its release */
        if (u->held && vk == u->held_vk && (vk || ascii == u->held_ascii)) { u->held = 0; return 1; }
        return u->wm_mode;
    }
    int used = route(u, ascii, modifiers, vk, ctrl, shift, alt);
    u->held = (uint8_t)used;
    u->held_vk = vk;
    u->held_ascii = ascii;
    return used;
}

static int route(vds_ui *u, uint8_t ascii, uint8_t modifiers, uint8_t vk, int ctrl, int shift, int alt) {
    vds *v = core(u);
    if (u->focus != VDS_NONE && !focusable(v, u->focus)) vds_ui_forget(u, u->focus);
    if (u->wm_mode) return wm_key(u, modifiers, vk);
    if (vk == VDS_VK_F6) return cycle(u, shift);
    if (u->focus == VDS_NONE) return 0;
    if (vk == VDS_VK_F5 && ctrl) {
        u->wm_saved = geometry_of(v, u->focus);
        u->wm_mode = 1;
        return 1;
    }
    if (vk == VDS_VK_F5) { zoom(u); return 1; }
    if ((vk == VDS_VK_F3 && alt) || (ctrl && (ascii == 'w' || ascii == 'W' || ascii == 0x17))) {
        win_event(u, VDS_WIN_CLOSE, u->focus);
        return 1;
    }
    return 0;
}

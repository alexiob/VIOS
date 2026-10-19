/* VIOS Display Server wire protocol: see vds_proto.h. */
#include "vds_proto.h"
#include <string.h>

void vds_proto_reply(vds_proto *p, const uint8_t *data, uint8_t n) {
    if (p->send) p->send(p->ctx, VDS_PACKET_REPLY, data, n);
}
static void reply(vds_proto *p, const uint8_t *data, uint8_t n) { vds_proto_reply(p, data, n); }

static int16_t s16(const uint8_t *b) { return (int16_t)(uint16_t)(b[0] | b[1] << 8); }
static uint16_t u16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }
static void put16(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *b, uint32_t v) { put16(b, v); put16(b + 2, v >> 16); }

/* Size of the op at b (n bytes left), or 0 if unknown or truncated. */
static size_t op_size(const uint8_t *b, size_t n) {
    size_t s;
    switch (b[0]) {
    case 0x01: s = 5; break;
    case 0x10: s = n < 6 ? 0 : 6u + b[5]; break;
    case 0x11: s = n < 3 ? 0 : 3u + b[2]; break;
    case 0x12: s = n < 5 ? 0 : 5u + 2u * b[4]; break;
    case 0x13: s = 8; break;
    case 0x14: s = 4; break;
    case 0x15: s = 6; break;
    case 0x16: s = 8; break;
    case 0x30: case 0x34: s = 1; break;
    case 0x36: s = 2; break;
    case 0x31: case 0x33: case 0x35: s = 2; break;
    case 0x32: s = 5; break;
    case 0x50: s = 10; break;
    case 0x51: s = 8; break;
    case 0x52: case 0x57: case 0x58: case 0x7A: s = 2; break;
    case 0x53: case 0x55: case 0x79: s = 3; break;
    case 0x54: s = n < 3 ? 0 : 3u + b[2]; break;
    case 0x56: case 0x74: s = 4; break;
    case 0x76: s = 7; break;
    case 0x77: s = n < 5 ? 0 : 5u + b[4]; break;
    case 0x78: s = 6; break;
    case 0x59: s = 3; break;
    case 0x60: s = n < 8 ? 0 : 8u + b[7]; break;
    case 0x61: s = n < 4 || n < 4u + b[2] ? 0 : 4u + b[2] + 2u * b[3 + b[2]]; break;
    case 0x62: s = 4; break;
    case 0x70: s = 11; break;
    case 0x71: s = 4; break;
    case 0x7B: s = 6; break;
    case 0x80: s = 10; break;
    case 0x81: s = n < 3 ? 0 : 3u + b[2]; break;
    case 0x82: case 0x83: case 0x84: s = 4; break;
    case 0x85: s = n < 4 ? 0 : 4u + b[3]; break;
    case 0x86: s = 3; break;
    case 0x87: case 0x88: case 0x89: s = 2; break;
    default: s = 0; break;
    }
    return s && s <= n ? s : 0;
}

void vds_proto_event(vds_proto *p, uint8_t kind, uint8_t id, const uint8_t *args, uint8_t n) {
    uint8_t data[VDS_EVENT_MAX];
    if (n > sizeof data - 3) n = sizeof data - 3;
    data[0] = p->event_seq++;
    data[1] = kind;
    data[2] = id;
    if (n) memcpy(data + 3, args, n);
    if (p->push_events) {
        if (p->send) p->send(p->ctx, VDS_PACKET_EVENT, data, (uint8_t)(n + 3));
        return;
    }
    if (p->event_count == VDS_EVENT_QUEUE) {                /* keep the newest; count the loss */
        p->event_head = (uint8_t)((p->event_head + 1) % VDS_EVENT_QUEUE);
        --p->event_count;
        if (p->event_lost < 255) ++p->event_lost;
    }
    unsigned slot = (p->event_head + p->event_count++) % VDS_EVENT_QUEUE;
    memcpy(p->events[slot], data, n + 3u);
    p->event_len[slot] = (uint8_t)(n + 3);
}

/* Reply 36 left n (len event)*n: as many queued events as fit. A loss is
 * reported first as an OVERFLOW event so the CPU knows to resynchronise. */
static void poll_events(vds_proto *p, uint8_t max) {
    uint8_t r[VDS_REPLY_MAX] = {0x36, 0, 0};
    unsigned at = 3;
    if (p->event_lost && max) {
        uint8_t lost = p->event_lost;
        p->event_lost = 0;
        uint8_t ev[4] = {p->event_seq++, VDS_EVENT_OVERFLOW, 0, lost};
        r[at++] = sizeof ev;
        memcpy(r + at, ev, sizeof ev);
        at += sizeof ev;
        ++r[2];
    }
    while (p->event_count && r[2] < max && at + 1u + p->event_len[p->event_head] <= sizeof r) {
        uint8_t len = p->event_len[p->event_head];
        r[at++] = len;
        memcpy(r + at, p->events[p->event_head], len);
        at += len;
        ++r[2];
        p->event_head = (uint8_t)((p->event_head + 1) % VDS_EVENT_QUEUE);
        --p->event_count;
    }
    r[1] = p->event_count;
    reply(p, r, (uint8_t)at);
}


static void error(vds_proto *p, uint8_t op, size_t offset) {
    uint8_t args[2] = {op, (uint8_t)offset};
    if (p->errors < 255) ++p->errors;
    vds_proto_event(p, VDS_EVENT_ERROR, 0, args, 2);
}

void vds_proto_init(vds_proto *p, vds *v, vds_cell *(*cells)(void *, uint8_t, uint16_t),
                    void (*send)(void *, uint8_t, const uint8_t *, uint8_t), void *ctx) {
    memset(p, 0, sizeof *p);
    p->v = v;
    p->cells = cells;
    p->send = send;
    p->ctx = ctx;
}


static void reset(vds_proto *p) {
    vds *v = p->v;
    while (v->count) {
        uint8_t id = v->order[v->count - 1];
        vds_window_close(v, id);
        if (p->ui_forget) p->ui_forget(p->ui, id);
    }
    for (uint8_t i = 0; i < VDS_MAX_SPRITES; ++i) vds_sprite_undefine(v, i);
    memset(v->surfaces, 0, sizeof v->surfaces);
    p->pool_used = 0;
    memset(p->sprite_off, 0, sizeof p->sprite_off);
    memset(p->sprite_len, 0, sizeof p->sprite_len);
    vds_set_snap(v, 8);
    vds_set_style(v, VDS_STYLE_SMALLTALK);
    if (p->ui_reset) p->ui_reset(p->ui);
    p->errors = p->desync = 0;                              /* a new epoch starts clean */
    ++p->epoch;
}

static int define_sprite(vds_proto *p, const uint8_t *b) {
    uint8_t id = b[1], w = b[2], h = b[3], tr = b[6];
    if (id >= VDS_MAX_SPRITES || !w || !h || w > VDS_SPRITE_MAX || h > VDS_SPRITE_MAX) return 0;
    uint16_t size = (uint16_t)((w + 1u) / 2 * h);
    if (p->sprite_len[id] < size) {                     /* grow: take fresh pool space */
        if (p->pool_used + size > VDS_PROTO_POOL) return 0;
        p->sprite_off[id] = p->pool_used;
        p->pool_used = (uint16_t)(p->pool_used + size);
    }
    p->sprite_len[id] = size > p->sprite_len[id] ? size : p->sprite_len[id];
    memset(p->pool + p->sprite_off[id], tr < 16 ? (tr << 4 | tr) : 0, size);
    return vds_sprite_define(p->v, id, w, h, b[4], b[5], tr, p->pool + p->sprite_off[id]);
}

/* Ops whose window (b[1]) or surface argument the CPU may not name. */
static int reserved(const uint8_t *b) {
    switch (b[0]) {
    case 0x01: case 0x10: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x32:
        return b[1] >= VDS_PROTO_SURFACES;
    case 0x50: case 0x55:
        return b[1] >= VDS_PROTO_WINDOWS || (b[2] != VDS_NONE && b[2] >= VDS_PROTO_SURFACES);
    case 0x51: case 0x52: case 0x53: case 0x54: case 0x56: case 0x57: case 0x58: case 0x59:
        return b[1] >= VDS_PROTO_WINDOWS;
    }
    return 0;
}

static int apply(vds_proto *p, const uint8_t *b) {
    vds *v = p->v;
    uint8_t op = b[0];
    if (op != 0x10 && op != 0x11) p->more_ok = 0;
    if (reserved(b)) return 0;
    if ((op >= 0x59 && op <= 0x62) || op == 0x70 || op == 0x71 || op == 0x7B || (op >= 0x80 && op <= 0x89))
        return p->ui_op ? p->ui_op(p->ui, b) : 0;
    switch (op) {
    case 0x01: {
        if (b[1] >= VDS_MAX_SURFACES || !b[2] || !b[3] || !p->cells) return 0;
        vds_cell *cells = p->cells(p->ctx, b[1], (uint16_t)(b[2] * b[3]));
        return cells && vds_surface_init(v, b[1], b[2], b[3], cells, b[4]);
    }
    case 0x10:
        if (!vds_put(v, b[1], b[2], b[3], b[4], (const char *)b + 6, b[5])) return 0;
        p->more_ok = 1; p->more_id = b[1]; p->more_col = (uint8_t)(b[2] + b[5]); p->more_row = b[3];
        return 1;
    case 0x11:
        if (!p->more_ok || !vds_put(v, p->more_id, p->more_col, p->more_row, b[1], (const char *)b + 3, b[2])) return 0;
        p->more_col = (uint8_t)(p->more_col + b[2]);
        return 1;
    case 0x12: {
        vds_surface *s = b[1] < VDS_MAX_SURFACES ? &v->surfaces[b[1]] : 0;
        if (!s || !s->cells || b[3] >= s->rows || b[2] + b[4] > s->cols) return 0;
        for (unsigned i = 0; i < b[4]; ++i)
            if (!vds_put(v, b[1], (uint8_t)(b[2] + i), b[3], b[6 + 2 * i], (const char *)b + 5 + 2 * i, 1)) return 0;
        return 1;
    }
    case 0x13: return vds_fill(v, b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    case 0x14: return vds_scroll(v, b[1], (int8_t)b[2], b[3]);
    case 0x15: return vds_cursor(v, b[1], b[2], b[3], b[4], b[5]);
    case 0x16: return vds_scroll_rect(v, b[1], b[2], b[3], b[4], b[5], (int8_t)b[6], b[7]);
    case 0x30: p->hold = 0; return 1;
    case 0x31: {
        uint8_t r[9] = {0x31, b[1], 0, 0, 0, 0, p->errors, p->desync, p->raw};
        put16(r + 2, p->epoch);
        put16(r + 4, p->generation);
        p->desync = 0;
        reply(p, r, sizeof r);
        return 1;
    }
    case 0x32: {
        vds_surface *s = b[1] < VDS_MAX_SURFACES ? &v->surfaces[b[1]] : 0;
        uint8_t n = b[4], r[2 + 28];
        if (!s || !s->cells || n > 14 || b[3] >= s->rows || b[2] + n > s->cols) return 0;
        r[0] = 0x32; r[1] = n;
        for (unsigned i = 0; i < n; ++i) {
            r[2 + 2 * i] = s->cells[b[3] * s->cols + b[2] + i].ch;
            r[3 + 2 * i] = s->cells[b[3] * s->cols + b[2] + i].attr;
        }
        reply(p, r, (uint8_t)(2 + 2 * n));
        return 1;
    }
    case 0x33: {
        uint8_t r[10] = {0x33, b[1]};
        uint32_t a, c;
        unsigned sprites = 0;
        for (unsigned i = 0; i < VDS_MAX_SPRITES; ++i) sprites += v->sprites[i].used;
        if (b[1] == 0) { a = v->stats.frames; c = v->stats.rows; }
        else if (b[1] == 1) { a = v->stats.pixels; c = v->stats.carried; }
        else if (b[1] == 2) { a = v->stats.lut_misses; c = v->ndamage | (uint32_t)v->count << 8 | (uint32_t)sprites << 16; }
        else return 0;
        put32(r + 2, a);
        put32(r + 6, c);
        reply(p, r, sizeof r);
        return 1;
    }
    case 0x34: reset(p); return 1;
    case 0x36: poll_events(p, b[1]); return 1;
    case 0x35:
        p->raw = b[1] != 0;
        if (!p->raw) vds_damage_all(v);
        return 1;
    case 0x50: {
        char none = 0;
        return vds_window_open(v, b[1], b[2], s16(b + 3), s16(b + 5), b[7], b[8], b[9], &none);
    }
    case 0x51:
        return vds_window_move(v, b[1], s16(b + 2), s16(b + 4)) && vds_window_resize(v, b[1], b[6], b[7]);
    case 0x52: return vds_window_raise(v, b[1]);
    case 0x53: return vds_window_flags(v, b[1], b[2]);
    case 0x54: {
        char title[VDS_TITLE_MAX];
        size_t n = b[2] < VDS_TITLE_MAX - 1 ? b[2] : VDS_TITLE_MAX - 1;
        memcpy(title, b + 3, n);
        title[n] = 0;
        return vds_window_title(v, b[1], title);
    }
    case 0x55: return vds_window_surface(v, b[1], b[2]);
    case 0x56: return vds_window_scroll(v, b[1], b[2], b[3]);
    case 0x57:
        if (!vds_window_close(v, b[1])) return 0;
        if (p->ui_forget) p->ui_forget(p->ui, b[1]);
        return 1;
    case 0x58: return p->ui_focus ? p->ui_focus(p->ui, b[1]) : 0;
    case 0x74: {
        uint16_t value = u16(b + 2);
        if (b[1] == 1) return value <= 255 && vds_set_snap(v, (uint8_t)value);
        if (b[1] == 2) return value <= 255 && vds_set_style(v, (uint8_t)value);
        return 0;
    }
    case 0x76: return define_sprite(p, b);
    case 0x77: {
        uint8_t id = b[1];
        uint16_t off = u16(b + 2);
        if (id >= VDS_MAX_SPRITES || !v->sprites[id].used || v->sprites[id].pixels != p->pool + p->sprite_off[id] ||
            (uint32_t)off + b[4] > p->sprite_len[id]) return 0;
        memcpy(p->pool + p->sprite_off[id] + off, b + 5, b[4]);
        if (v->sprites[id].visible) vds_damage(v, vds_sprite_rect(v, id));
        return 1;
    }
    case 0x78: return vds_sprite_move(v, b[1], s16(b + 2), s16(b + 4));
    case 0x79: return vds_sprite_show(v, b[1], b[2]);
    case 0x7A:
        return vds_sprite_define(v, b[1], VDS_ARROW_W, VDS_ARROW_H, 0, 0, VDS_ARROW_TRANSPARENT, vds_arrow_cursor);
    }
    return 0;
}

int vds_proto_batch(vds_proto *p, const uint8_t *b, size_t len) {
    for (size_t at = 0; at < len;) {                    /* validate the whole batch first */
        size_t s = op_size(b + at, len - at);
        if (!s) {
            p->desync = 1;
            error(p, b[at], at);
            return 0;
        }
        at += s;
    }
    for (size_t at = 0; at < len;) {
        size_t s = op_size(b + at, len - at);
        uint8_t op = b[at];
        if (op != 0x30 && op != 0x31 && op != 0x32 && op != 0x33 && op != 0x36 && op != 0x89 && !p->hold) { p->hold = 1; p->held_ms = 0; }
        if (!apply(p, b + at)) error(p, op, at);
        at += s;
    }
    ++p->generation;
    if (p->ui_after) p->ui_after(p->ui);
    return 1;
}

int vds_proto_frame(vds_proto *p, uint32_t elapsed_ms, uint32_t budget) { return vds_proto_frame_within(p, elapsed_ms, budget, UINT32_MAX); }

int vds_proto_frame_within(vds_proto *p, uint32_t elapsed_ms, uint32_t budget, uint32_t budget_us) {
    if (p->ui_tick) p->ui_tick(p->ui, elapsed_ms);
    vds_tick(p->v, elapsed_ms);
    if (p->raw) return 1;
    if (p->hold) {
        p->held_ms += elapsed_ms;
        if (p->held_ms < VDS_PROTO_HOLD_MS) return p->v->ndamage == 0;
        p->hold = 0;                                     /* a stalled client cannot hide output */
    }
    return vds_frame_within(p->v, budget, budget_us);
}

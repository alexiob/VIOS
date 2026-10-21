/* CPU side of the VIOS GUI: Forth words that drive the ESP display server
 * (firmware/esp32-vios/lib/vios-vds) through VDU 23,0,&E4 batches, and pull
 * its events (op 36). The ESP owns presentation and frame-time behaviour
 * (compositing, window manager, menus, caret blink); these words only state
 * what exists and receive semantic events. Wire format: vds_proto.h.
 *
 * Every word sends its op at once unless UI-BEGIN started a batch, which
 * UI-COMMIT sends as one E4 payload ending in a COMMIT, so the ESP shows it
 * atomically. Text longer than one op is split into PUT + MORE runs. */
#include "vios/ui.h"
#include "vios/runtime.h"
#include "platform/vdp.h"

#define BATCH_MAX 250
#define REPLY 0xF1                      /* packet 0x71 as seen by the CPU */
#define EVENT_RING 8
static unsigned char batch[BATCH_MAX];
static unsigned batch_used, batching;
static unsigned char events[EVENT_RING][VIOS_UI_EVENT_MAX], event_len[EVENT_RING];
static unsigned event_head, event_count;

static void flush(void) {
    if (!batch_used) return;
    unsigned char header[4] = {23, 0, 0xE4, (unsigned char)batch_used};
    platform_vdp_send(header, sizeof header);
    platform_vdp_send(batch, batch_used);
    batch_used = 0;
}

/* Append one complete op; ops never straddle two E4 payloads. */
static void op(const unsigned char *bytes, unsigned n) {
    if (batch_used + n > BATCH_MAX) flush();
    for (unsigned i = 0; i < n; ++i) batch[batch_used++] = bytes[i];
    if (!batching) flush();
}

/* The batch buffer is empty after the flush and carries the request. */
static int request(const unsigned char *bytes, unsigned n, unsigned char *reply, unsigned capacity, unsigned *length) {
    flush();
    if (n > BATCH_MAX - 4) return 0;
    batch[0] = 23; batch[1] = 0; batch[2] = 0xE4; batch[3] = (unsigned char)n;
    for (unsigned i = 0; i < n; ++i) batch[4 + i] = bytes[i];
    return platform_vdp_request(batch, 4 + n, REPLY, reply, capacity, length) && *length && reply[0] == bytes[0];
}

/* Stack helpers: pop k cells (last pushed first) into out[0..k-1] in push order. */
static int pop(fcell *out, unsigned k) {
    for (unsigned i = k; i--;) out[i] = fvm_pop();
    return !fvm_error;
}
static unsigned char b(fcell v) { return (unsigned char)(v & 0xFF); }
static int32_t s24(fcell v) { v &= FVM_MASK; return v & 0x800000UL ? (int32_t)v - 0x1000000L : (int32_t)v; }
static int range(fcell v, unsigned max, const char *what) { return v <= max ? 1 : forth_error(what); }
static void le16(unsigned char *p, int32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)((uint32_t)v >> 8); }

/* Copy a Forth string (addr len) into op bytes, checking printable ASCII. */
static int text(fcell addr, fcell len, unsigned char *out, unsigned max) {
    if (len > max) return forth_error("UI text too long");
    for (fcell i = 0; i < len; ++i) {
        unsigned char c = fvm_read8(addr + i);
        if (fvm_error) return 0;
        out[i] = c;
    }
    return 1;
}

/* UI-RESET clears everything on the ESP, the tools' commands and menus too:
 * the count lets the tools install them again (vios_tools_idle). */
static unsigned resets;
unsigned vios_ui_resets(void) { return resets; }
static int ui_reset(void) { unsigned char o[] = {0x34}; batch_used = 0; event_count = 0; op(o, 1); ++resets; return 1; }
static int ui_raw(void) { fcell f = fvm_pop(); if (fvm_error) return 0; unsigned char o[] = {0x35, (unsigned char)(f != 0)}; op(o, 2); return 1; }
static int config(unsigned char key, fcell v, unsigned max, const char *what) {
    if (!range(v, max, what)) return 0;
    unsigned char o[] = {0x74, key, b(v), 0};
    op(o, 4);
    return 1;
}
static int ui_style(void) { fcell v = fvm_pop(); return !fvm_error && config(2, v, 1, "UI-STYLE: 0 Smalltalk or 1 Turbo Vision"); }
static int ui_snap(void) { fcell v = fvm_pop(); return !fvm_error && (v == 8 || v == 1 ? config(1, v, 8, "") : forth_error("UI-SNAP: 8 or 1")); }

static int ui_surface(void) {                           /* ( id cols rows attr -- ) */
    fcell a[4];
    if (!pop(a, 4) || !range(a[0], VIOS_UI_SURFACES - 1, "UI surface id 0..10") || !range(a[1], 80, "UI surface: 1..80 columns") ||
        !range(a[2], 60, "UI surface: 1..60 rows") || !a[1] || !a[2]) return fvm_error ? 0 : forth_error("UI surface size");
    unsigned char o[] = {0x01, b(a[0]), b(a[1]), b(a[2]), b(a[3])};
    op(o, sizeof o);
    return 1;
}

static int ui_type(void) {                              /* ( addr len id col row attr -- ) */
    fcell a[6];
    if (!pop(a, 6) || !range(a[2], VIOS_UI_SURFACES - 1, "UI surface id 0..10") || !range(a[3], 255, "UI column") || !range(a[4], 255, "UI row"))
        return 0;
    if (a[1] > 255) return forth_error("UI-TYPE: at most 255 characters");
    unsigned char o[6 + 64];
    fcell done = 0;
    do {
        unsigned n = a[1] - done > 64 ? 64u : (unsigned)(a[1] - done), at;
        if (!done) { o[0] = 0x10; o[1] = b(a[2]); o[2] = b(a[3]); o[3] = b(a[4]); o[4] = b(a[5]); o[5] = (unsigned char)n; at = 6; }
        else { o[0] = 0x11; o[1] = b(a[5]); o[2] = (unsigned char)n; at = 3; }
        if (!text(a[0] + done, n, o + at, 64)) return 0;
        op(o, at + n);
        done += n;
    } while (done < a[1]);
    return 1;
}

static int ui_fill(void) {                              /* ( id col row w h char attr -- ) */
    fcell a[7];
    if (!pop(a, 7) || !range(a[0], VIOS_UI_SURFACES - 1, "UI surface id 0..10")) return 0;
    unsigned char o[] = {0x13, b(a[0]), b(a[1]), b(a[2]), b(a[3]), b(a[4]), b(a[5]), b(a[6])};
    op(o, sizeof o);
    return 1;
}

static int ui_scroll(void) {                            /* ( id dy attr -- ) */
    fcell a[3];
    if (!pop(a, 3) || !range(a[0], VIOS_UI_SURFACES - 1, "UI surface id 0..10")) return 0;
    int32_t dy = s24(a[1]);
    if (dy < -127 || dy > 127) return forth_error("UI-SCROLL: -127..127 rows");
    unsigned char o[] = {0x14, b(a[0]), (unsigned char)(dy & 0xFF), b(a[2])};
    op(o, sizeof o);
    return 1;
}

static int ui_caret(void) {                             /* ( id col row shape blink -- ) */
    fcell a[5];
    if (!pop(a, 5) || !range(a[0], VIOS_UI_SURFACES - 1, "UI surface id 0..10") || !range(a[3], 3, "UI-CARET shape 0..3")) return 0;
    unsigned char o[] = {0x15, b(a[0]), b(a[1]), b(a[2]), b(a[3]), (unsigned char)(a[4] != 0)};
    op(o, sizeof o);
    return 1;
}

static int window_id(fcell id) { return range(id, VIOS_UI_WINDOWS - 1, "UI window id 0..26"); }

static int ui_window(void) {                            /* ( id surface x y cols rows flags -- ) */
    fcell a[7];
    if (!pop(a, 7) || !window_id(a[0]) || (a[1] != 255 && !range(a[1], VIOS_UI_SURFACES - 1, "UI surface id 0..10 or 255")) ||
        !a[4] || !a[5] || !range(a[4], 255, "UI window columns") || !range(a[5], 255, "UI window rows"))
        return fvm_error ? 0 : forth_error("UI window size");
    unsigned char o[10] = {0x50, b(a[0]), b(a[1])};
    le16(o + 3, s24(a[2]));
    le16(o + 5, s24(a[3]));
    o[7] = b(a[4]); o[8] = b(a[5]); o[9] = b(a[6]);
    op(o, sizeof o);
    return 1;
}

static int ui_title(void) {                             /* ( addr len id -- ) */
    fcell a[3];
    if (!pop(a, 3) || !window_id(a[2])) return 0;
    unsigned char o[3 + 31] = {0x54, b(a[2]), b(a[1])};
    if (!text(a[0], a[1], o + 3, 31)) return 0;
    op(o, 3 + (unsigned)a[1]);
    return 1;
}

static int ui_place(void) {                             /* ( id x y cols rows -- ) */
    fcell a[5];
    if (!pop(a, 5) || !window_id(a[0]) || !a[3] || !a[4] || a[3] > 255 || a[4] > 255) return fvm_error ? 0 : forth_error("UI-PLACE size");
    unsigned char o[8] = {0x51, b(a[0])};
    le16(o + 2, s24(a[1]));
    le16(o + 4, s24(a[2]));
    o[6] = b(a[3]); o[7] = b(a[4]);
    op(o, sizeof o);
    return 1;
}

static int window_op(unsigned char code) {              /* ( id -- ) */
    fcell id = fvm_pop();
    if (fvm_error || !window_id(id)) return 0;
    unsigned char o[] = {code, b(id)};
    op(o, 2);
    return 1;
}
static int ui_raise(void) { return window_op(0x52); }
static int ui_close(void) { return window_op(0x57); }
static int ui_focus(void) { return window_op(0x58); }

static int window_pair(unsigned char code, unsigned max, const char *what) {   /* ( id value -- ) */
    fcell a[2];
    if (!pop(a, 2) || !window_id(a[0]) || !range(a[1], max, what)) return 0;
    unsigned char o[] = {code, b(a[0]), b(a[1])};
    op(o, 3);
    return 1;
}
static int ui_flags(void) { return window_pair(0x53, 255, "UI-FLAGS 0..255"); }
static int ui_client(void) { return window_pair(0x55, 255, "UI-CLIENT surface"); }
static int ui_popup(void) { return window_pair(0x59, 255, "UI-POPUP menu 0..15 or 255"); }

static int ui_view(void) {                              /* ( id col row -- ) */
    fcell a[3];
    if (!pop(a, 3) || !window_id(a[0])) return 0;
    unsigned char o[] = {0x56, b(a[0]), b(a[1]), b(a[2])};
    op(o, sizeof o);
    return 1;
}

static int ui_command(void) {                           /* ( addr len id flags vk mods ascii -- ) */
    fcell a[7];
    if (!pop(a, 7) || !range(a[2], 0xFDFF, "UI-COMMAND id 0..65023")) return 0;
    unsigned char o[8 + 23] = {0x60, b(a[2]), (unsigned char)(a[2] >> 8), b(a[3]), b(a[4]), b(a[5]), b(a[6]), b(a[1])};
    if (!text(a[0], a[1], o + 8, 23)) return 0;
    op(o, 8 + (unsigned)a[1]);
    return 1;
}

static int ui_menu(void) {                              /* ( item1 .. itemN n addr len menu -- ) */
    fcell a[4];
    if (!pop(a, 4) || !range(a[3], 15, "UI-MENU id 0..15") || !range(a[0], 16, "UI-MENU: at most 16 items")) return 0;
    unsigned char o[4 + 15 + 32];
    unsigned at = 0;
    o[at++] = 0x61; o[at++] = b(a[3]); o[at++] = b(a[2]);
    if (!text(a[1], a[2], o + at, 15)) return 0;
    at += (unsigned)a[2];
    o[at++] = b(a[0]);
    fcell items[16];
    for (unsigned i = (unsigned)a[0]; i--;) items[i] = fvm_pop();
    if (fvm_error) return 0;
    for (unsigned i = 0; i < a[0]; ++i) { o[at++] = b(items[i]); o[at++] = (unsigned char)(items[i] >> 8); }
    op(o, at);
    return 1;
}

static int ui_state(void) {                             /* ( id flags -- ) */
    fcell a[2];
    if (!pop(a, 2) || !range(a[0], 0xFDFF, "UI-STATE command id")) return 0;
    unsigned char o[] = {0x62, b(a[0]), (unsigned char)(a[0] >> 8), b(a[1])};
    op(o, sizeof o);
    return 1;
}

static int sprite_id(fcell id) { return range(id, 15, "UI sprite id 0..15"); }
static int ui_sprite(void) {                            /* ( id w h hotx hoty transparent -- ) */
    fcell a[6];
    if (!pop(a, 6) || !sprite_id(a[0]) || !a[1] || !a[2] || a[1] > 64 || a[2] > 64 || a[3] >= a[1] || a[4] >= a[2])
        return fvm_error ? 0 : forth_error("UI-SPRITE: 1..64 pixels, hotspot inside");
    unsigned char o[] = {0x76, b(a[0]), b(a[1]), b(a[2]), b(a[3]), b(a[4]), b(a[5])};
    op(o, sizeof o);
    return 1;
}
static int ui_pixels(void) {                            /* ( addr len id offset -- ) 4 bpp bytes */
    fcell a[4];
    if (!pop(a, 4) || !sprite_id(a[2]) || !range(a[3] + a[1], 2048, "UI-PIXELS past the sprite")) return 0;
    unsigned char o[5 + 64];
    for (fcell done = 0; done < a[1];) {
        unsigned n = a[1] - done > 64 ? 64u : (unsigned)(a[1] - done);
        o[0] = 0x77; o[1] = b(a[2]); le16(o + 2, (int32_t)(a[3] + done)); o[4] = (unsigned char)n;
        if (!text(a[0] + done, n, o + 5, 64)) return 0;
        op(o, 5 + n);
        done += n;
    }
    return 1;
}
static int ui_sprite_at(void) {                         /* ( id x y -- ) */
    fcell a[3];
    if (!pop(a, 3) || !sprite_id(a[0])) return 0;
    unsigned char o[6] = {0x78, b(a[0])};
    le16(o + 2, s24(a[1]));
    le16(o + 4, s24(a[2]));
    op(o, sizeof o);
    return 1;
}
static int ui_sprite_show(void) {                       /* ( id flag -- ) */
    fcell a[2];
    if (!pop(a, 2) || !sprite_id(a[0])) return 0;
    unsigned char o[] = {0x79, b(a[0]), (unsigned char)(a[1] != 0)};
    op(o, sizeof o);
    return 1;
}
static int ui_arrow(void) { fcell id = fvm_pop(); if (fvm_error || !sprite_id(id)) return 0; unsigned char o[] = {0x7A, b(id)}; op(o, 2); return 1; }

static int ui_animate(void) {                           /* ( target id prop from to ms easing -- ) */
    fcell a[7];
    if (!pop(a, 7) || !range(a[0], 1, "UI-ANIMATE target: 0 window, 1 sprite") || !range(a[2], 3, "UI-ANIMATE property 0..3") ||
        !range(a[5], 65535, "UI-ANIMATE: at most 65535 ms") || !range(a[6], 3, "UI-ANIMATE easing 0..3")) return 0;
    if (a[0] == 0 ? !window_id(a[1]) : !sprite_id(a[1])) return 0;
    unsigned char o[11] = {0x70, b(a[0]), b(a[1]), b(a[2])};
    le16(o + 4, s24(a[3]));
    le16(o + 6, s24(a[4]));
    le16(o + 8, (int32_t)a[5]);
    o[10] = b(a[6]);
    op(o, sizeof o);
    return 1;
}
static int ui_stop(void) {                              /* ( target id prop -- ) */
    fcell a[3];
    if (!pop(a, 3) || !range(a[0], 1, "UI-STOP target") || !range(a[2], 3, "UI-STOP property")) return 0;
    unsigned char o[] = {0x71, b(a[0]), b(a[1]), b(a[2])};
    op(o, sizeof o);
    return 1;
}
static int ui_pointer(void) {                           /* ( x y buttons -- ) */
    fcell a[3];
    if (!pop(a, 3)) return 0;
    unsigned char o[6] = {0x7B};
    le16(o + 1, s24(a[0]));
    le16(o + 3, s24(a[1]));
    o[5] = b(a[2]);
    op(o, sizeof o);
    return 1;
}

/* Widgets (G4). Widget ids 0..63; text up to 47 characters, list rows 31. */
static int widget_id(fcell id) { return range(id, VIOS_UI_WIDGETS - 1, "UI widget id 0..63"); }
static int ui_widget(void) {                            /* ( id window kind parent col row w h flags -- ) */
    fcell a[9];
    if (!pop(a, 9) || !widget_id(a[0]) || !window_id(a[1]) || !a[2] || !range(a[2], 10, "UI-WIDGET kind 1..10") ||
        (a[3] != 255 && !widget_id(a[3])))
        return fvm_error ? 0 : forth_error("UI-WIDGET kind 1..10");
    unsigned char o[10] = {0x80};
    for (unsigned i = 0; i < 9; ++i) o[1 + i] = b(a[i]);
    op(o, sizeof o);
    return 1;
}
static int ui_wtext(void) {                             /* ( addr len id -- ) */
    fcell a[3];
    if (!pop(a, 3) || !widget_id(a[2])) return 0;
    unsigned char o[3 + 47] = {0x81, b(a[2]), b(a[1])};
    if (!text(a[0], a[1], o + 3, 47)) return 0;
    op(o, 3 + (unsigned)a[1]);
    return 1;
}
static int ui_wvalue(void) {                            /* ( id value -- ) */
    fcell a[2];
    if (!pop(a, 2) || !widget_id(a[0])) return 0;
    unsigned char o[4] = {0x82, b(a[0])};
    le16(o + 2, s24(a[1]));
    op(o, sizeof o);
    return 1;
}
static int widget_triple(unsigned char code, const char *what) {   /* ( id x y -- ) */
    fcell a[3];
    if (!pop(a, 3) || !widget_id(a[0]) || !range(a[1], 255, what) || !range(a[2], 255, what)) return 0;
    unsigned char o[] = {code, b(a[0]), b(a[1]), b(a[2])};
    op(o, sizeof o);
    return 1;
}
static int ui_layout(void) { return widget_triple(0x83, "UI-LAYOUT: layout 0..2, gap"); }
static int ui_wsize(void) { return widget_triple(0x84, "UI-WSIZE: fixed and weight 0..255"); }
static int ui_item(void) {                              /* ( addr len id index -- ) */
    fcell a[4];
    if (!pop(a, 4) || !widget_id(a[2]) || !range(a[3], VIOS_UI_ROWS - 1, "UI-ITEM index 0..1023")) return 0;
    unsigned char o[5 + 31] = {0x90, b(a[2]), b(a[3]), (unsigned char)(a[3] >> 8), b(a[1])};
    if (!text(a[0], a[1], o + 5, 31)) return 0;
    op(o, 5 + (unsigned)a[1]);
    return 1;
}
static int ui_items(void) {                             /* ( id count -- ) */
    fcell a[2];
    if (!pop(a, 2) || !widget_id(a[0]) || !range(a[1], VIOS_UI_ROWS, "UI-ITEMS count 0..1024")) return 0;
    unsigned char o[] = {0x91, b(a[0]), b(a[1]), (unsigned char)(a[1] >> 8)};
    op(o, sizeof o);
    return 1;
}
static int ui_row(void) {                               /* ( addr len id -- ) append a row */
    fcell a[3];
    if (!pop(a, 3) || !widget_id(a[2])) return 0;
    unsigned char o[4 + 31] = {0x92, b(a[2]), 1, b(a[1])};
    if (!text(a[0], a[1], o + 4, 31)) return 0;
    op(o, 4 + (unsigned)a[1]);
    return 1;
}
static int ui_columns(void) {                           /* ( w1 .. wn n id -- ) table columns */
    fcell id = fvm_pop(), n = fvm_pop();
    if (fvm_error || !widget_id(id) || !range(n, 8, "UI-COLUMNS: at most 8")) return 0;
    unsigned char o[3 + 8] = {0x93, b(id), b(n)};
    for (unsigned i = (unsigned)n; i--;) { fcell w = fvm_pop(); if (fvm_error || !range(w, 80, "UI-COLUMNS width 0..80")) return 0; o[3 + i] = b(w); }
    op(o, 3 + (unsigned)n);
    return 1;
}
static int ui_keys(void) { return window_pair(0x5A, 1, "UI-KEYS: 0 forward or 1 drop"); }

/* TextView (kind 9): the text travels in 8A runs; read back 25 bytes a reply. */
void vios_ui_text(unsigned char id, const char *s, size_t n, int append) {
    unsigned char o[4 + 200] = {0x8A, id};
    size_t done = 0;
    do {
        unsigned k = n - done > 200 ? 200u : (unsigned)(n - done);
        o[2] = (unsigned char)(append || done);
        o[3] = (unsigned char)k;
        for (unsigned i = 0; i < k; ++i) o[4 + i] = (unsigned char)s[done + i];
        op(o, 4 + k);
        done += k;
    } while (done < n);
}
static int forth_text(unsigned char code) {             /* ( addr len id -- ): 0 replace, 1 append, 2 insert */
    fcell a[3];
    unsigned char o[3 + 200];
    if (!pop(a, 3) || !widget_id(a[2])) return 0;
    if (a[1] > (code == 2 ? 200u : VIOS_UI_TEXT_MAX)) return forth_error("UI text: at most 4096 bytes (insert 200)");
    fcell done = 0;
    do {
        unsigned k = a[1] - done > 200 ? 200u : (unsigned)(a[1] - done);
        if (!text(a[0] + done, k, o + 3, 200)) return 0;
        if (code == 2) { o[0] = 0x8D; o[1] = b(a[2]); o[2] = (unsigned char)k; op(o, 3 + k); }
        else vios_ui_text(b(a[2]), (const char *)o + 3, k, code || done);
        done += k;
    } while (done < a[1]);
    return 1;
}
static int ui_text(void) { return forth_text(0); }
static int ui_text_more(void) { return forth_text(1); }
static int ui_insert(void) { return forth_text(2); }
static int ui_select(void) {                            /* ( id caret anchor -- ) */
    fcell a[3];
    if (!pop(a, 3) || !widget_id(a[0])) return 0;
    unsigned char o[6] = {0x8B, b(a[0])};
    le16(o + 2, (int32_t)(a[1] > 0xFFFF ? 0xFFFF : a[1]));
    le16(o + 4, (int32_t)(a[2] > 0xFFFF ? 0xFFFF : a[2]));
    op(o, sizeof o);
    return 1;
}
int vios_ui_text_info(unsigned char id, struct vios_ui_text_info *out) {
    unsigned char o[] = {0x8E, id}, r[32];
    unsigned n = 0;
    if (!request(o, sizeof o, r, sizeof r, &n) || n < 11 || r[1] != id) return 0;
    out->length = (unsigned)(r[2] | r[3] << 8); out->caret = (unsigned)(r[4] | r[5] << 8);
    out->anchor = (unsigned)(r[6] | r[7] << 8); out->revision = (unsigned)(r[8] | r[9] << 8); out->dirty = r[10];
    return 1;
}
/* Copy text [offset, offset + n) of TextView id to sink in reply-sized runs. */
int vios_ui_text_read(unsigned char id, unsigned offset, unsigned n, void (*sink)(void *, const char *, unsigned), void *ctx) {
    while (n) {
        unsigned char o[5] = {0x8C, id, (unsigned char)offset, (unsigned char)(offset >> 8), (unsigned char)(n > 25 ? 25 : n)}, r[32];
        unsigned got = 0;
        if (!request(o, sizeof o, r, sizeof r, &got) || got < 5 || r[1] != id || (unsigned)(r[2] | r[3] << 8) != offset || 5u + r[4] > got) return 0;
        if (!r[4]) return 0;                            /* shorter than asked */
        sink(ctx, (const char *)r + 5, r[4]);
        offset += r[4];
        n -= r[4];
    }
    return 1;
}
struct forth_sink { fcell addr; unsigned at; };
static void to_forth(void *ctx, const char *s, unsigned n) {
    struct forth_sink *f = ctx;
    for (unsigned i = 0; i < n && !fvm_error; ++i) fvm_write8(f->addr + f->at++, (unsigned char)s[i]);
}
static int ui_text_get(void) {                          /* ( id addr max -- len ) */
    fcell a[3];
    struct vios_ui_text_info info;
    if (!pop(a, 3) || !widget_id(a[0])) return 0;
    if (!vios_ui_text_info(b(a[0]), &info)) return forth_error("UI-TEXT@: no such TextView");
    unsigned n = info.length < a[2] ? info.length : (unsigned)a[2];
    struct forth_sink f = {a[1], 0};
    if (!vios_ui_text_read(b(a[0]), 0, n, to_forth, &f) || fvm_error) return fvm_error ? 0 : forth_error("UI-TEXT@: read failed");
    return fvm_push(n);
}
static int ui_wname(void) {                             /* ( addr len id -- ) a name for /dev/win */
    fcell a[3];
    if (!pop(a, 3) || !widget_id(a[2])) return 0;
    unsigned char o[3 + 10] = {0x94, b(a[2]), b(a[1])};
    if (!text(a[0], a[1], o + 3, 10)) return 0;
    op(o, 3 + (unsigned)a[1]);
    return 1;
}
static int ui_wstyle(void) {                            /* ( id style -- ) 0 normal, 1 strong, 2 fault */
    fcell a[2];
    if (!pop(a, 2) || !widget_id(a[0]) || !range(a[1], 2, "UI-WSTYLE: 0 normal, 1 strong, 2 fault")) return 0;
    unsigned char o[] = {0x9F, b(a[0]), b(a[1])};
    op(o, sizeof o);
    return 1;
}
static int widget_op(unsigned char code) { fcell id = fvm_pop(); if (fvm_error || !widget_id(id)) return 0; unsigned char o[] = {code, b(id)}; op(o, 2); return 1; }
static int ui_wdelete(void) { return widget_op(0x87); }
static int ui_wfocus(void) { return widget_op(0x88); }
static int ui_wget(void) {                              /* ( id addr -- value len ) text copied to addr */
    fcell a[2];
    if (!pop(a, 2) || !widget_id(a[0])) return 0;
    unsigned char o[] = {0x89, b(a[0])}, r[32];
    unsigned n = 0;
    if (!request(o, sizeof o, r, sizeof r, &n) || n < 5 || r[1] != b(a[0]) || 5u + r[4] > n) return forth_error("UI-WGET: no such widget");
    for (unsigned i = 0; i < r[4]; ++i) { fvm_write8(a[1] + i, r[5 + i]); if (fvm_error) return 0; }
    return fvm_push((fcell)s24((fcell)(int16_t)(r[2] | r[3] << 8) & FVM_MASK)) && fvm_push(r[4]);
}

static int ui_begin(void) { batching = 1; return 1; }
static int ui_commit(void) { unsigned char o[] = {0x30}; batching = 0; op(o, 1); return 1; }

/* C clients (kernel/vios_tools.c) use the same encoder, batches and requests. */
void vios_ui_op(const unsigned char *bytes, unsigned n) { op(bytes, n); }
int vios_ui_request(const unsigned char *bytes, unsigned n, unsigned char *reply, unsigned capacity, unsigned *length) {
    return request(bytes, n, reply, capacity, length);
}
void vios_ui_begin(void) { batching = 1; }
void vios_ui_commit(void) { (void)ui_commit(); }
static int (*claim)(const unsigned char *event, unsigned len), (*route)(const unsigned char *event, unsigned len);
void vios_ui_claim(int (*hook)(const unsigned char *event, unsigned len)) { claim = hook; }
void vios_ui_route(int (*hook)(const unsigned char *event, unsigned len)) { route = hook; }

static int ui_sync(void) {                              /* ( -- epoch generation errors ) */
    static unsigned char seq;
    unsigned char o[] = {0x31, ++seq}, r[32];
    unsigned n = 0;
    if (!request(o, sizeof o, r, sizeof r, &n) || n < 9 || r[1] != seq) return forth_error("UI-SYNC: no display server reply");
    return fvm_push((fcell)(r[2] | r[3] << 8)) && fvm_push((fcell)(r[4] | r[5] << 8)) && fvm_push(r[6]);
}

/* Pull queued events into the CPU ring; returns 0 when the server did not answer. */
static int poll_events(void) {
    unsigned char o[2] = {0x36}, r[32];
    for (unsigned rounds = 0; rounds < 4 && event_count < EVENT_RING; ++rounds) {
        unsigned n = 0;
        o[1] = (unsigned char)(EVENT_RING - event_count);  /* never more than the ring can hold */
        if (!request(o, 2, r, sizeof r, &n) || n < 3) return 0;
        if (r[2] > o[1]) return 0;                      /* the server overran the request */
        for (unsigned at = 3, k = 0; k < r[2] && at < n; ++k) {
            unsigned len = r[at++];
            if (at + len > n || len > VIOS_UI_EVENT_MAX) break;
            if (route && len >= 3 && r[at + 1] == 0x13) { route(r + at, len); at += len; continue; }   /* /dev/win/N/event text */
            if (claim && claim(r + at, len)) { at += len; continue; }   /* a tool's own event */
            unsigned slot = (event_head + event_count++) % EVENT_RING;
            for (unsigned i = 0; i < len; ++i) events[slot][i] = r[at + i];
            event_len[slot] = (unsigned char)len;
            at += len;
        }
        if (!r[1]) break;                               /* nothing left on the ESP */
    }
    return 1;
}

int vios_ui_poll(void) { return poll_events(); }

static int take(unsigned char *out, unsigned *len) {
    if (!event_count && !poll_events()) return -1;
    if (!event_count) return 0;
    *len = event_len[event_head];
    for (unsigned i = 0; i < *len; ++i) out[i] = events[event_head][i];
    event_head = (event_head + 1) % EVENT_RING;
    --event_count;
    return 1;
}

static int ui_event(void) {                             /* ( addr -- len ) raw: seq kind id args... */
    fcell addr = fvm_pop();
    unsigned char e[VIOS_UI_EVENT_MAX];
    unsigned len = 0;
    if (fvm_error) return 0;
    int got = take(e, &len);
    if (got < 0) return forth_error("UI-EVENT: no display server reply");
    for (unsigned i = 0; got && i < len; ++i) { fvm_write8(addr + i, e[i]); if (fvm_error) return 0; }
    return fvm_push(got ? len : 0);
}

static int ui_next(void) {                              /* ( -- kind id arg TRUE | FALSE ) */
    unsigned char e[VIOS_UI_EVENT_MAX];
    unsigned len = 0;
    int got = take(e, &len);
    if (got < 0) return forth_error("UI-NEXT: no display server reply");
    if (!got || len < 3) return fvm_push(0);
    fcell arg = len >= 5 ? (fcell)(e[3] | e[4] << 8) : len == 4 ? e[3] : 0;
    return fvm_push(e[1]) && fvm_push(e[2]) && fvm_push(arg) && fvm_push(FVM_MASK);
}

int vios_ui_init(void) {
    static const struct { const char *name; int (*fn)(void); } words[] = {
        {"UI-RESET", ui_reset}, {"UI-RAW", ui_raw}, {"UI-STYLE", ui_style}, {"UI-SNAP", ui_snap},
        {"UI-SURFACE", ui_surface}, {"UI-TYPE", ui_type}, {"UI-FILL", ui_fill}, {"UI-SCROLL", ui_scroll}, {"UI-CARET", ui_caret},
        {"UI-WINDOW", ui_window}, {"UI-TITLE", ui_title}, {"UI-PLACE", ui_place}, {"UI-RAISE", ui_raise}, {"UI-CLOSE", ui_close},
        {"UI-FOCUS", ui_focus}, {"UI-FLAGS", ui_flags}, {"UI-CLIENT", ui_client}, {"UI-VIEW", ui_view},
        {"UI-COMMAND", ui_command}, {"UI-MENU", ui_menu}, {"UI-STATE", ui_state}, {"UI-POPUP", ui_popup},
        {"UI-SPRITE", ui_sprite}, {"UI-PIXELS", ui_pixels}, {"UI-SPRITE-AT", ui_sprite_at}, {"UI-SPRITE-SHOW", ui_sprite_show},
        {"UI-ARROW", ui_arrow}, {"UI-ANIMATE", ui_animate}, {"UI-STOP", ui_stop}, {"UI-POINTER", ui_pointer},
        {"UI-WIDGET", ui_widget}, {"UI-WTEXT", ui_wtext}, {"UI-WVALUE", ui_wvalue}, {"UI-LAYOUT", ui_layout}, {"UI-WSIZE", ui_wsize},
        {"UI-ITEM", ui_item}, {"UI-ITEMS", ui_items}, {"UI-WDELETE", ui_wdelete}, {"UI-WFOCUS", ui_wfocus}, {"UI-WGET", ui_wget}, {"UI-WNAME", ui_wname}, {"UI-WSTYLE", ui_wstyle},
        {"UI-BEGIN", ui_begin}, {"UI-COMMIT", ui_commit},
        {"UI-ROW+", ui_row},   /* append a list row; UI-ROW is the layout constant */ {"UI-COLUMNS", ui_columns}, {"UI-KEYS", ui_keys}, {"UI-TEXT", ui_text}, {"UI-TEXT+", ui_text_more},
        {"UI-INSERT", ui_insert}, {"UI-SELECT", ui_select}, {"UI-TEXT@", ui_text_get},
        {"UI-SYNC", ui_sync}, {"UI-EVENT", ui_event}, {"UI-NEXT", ui_next}};
    batch_used = batching = event_head = event_count = 0;
    /* Words that only send bytes may run in background tasks; requests wait
     * for a reply and stay in the foreground. */
    for (unsigned i = 0; i < sizeof words / sizeof words[0]; ++i) {
        int foreground = words[i].fn == ui_sync || words[i].fn == ui_event || words[i].fn == ui_next || words[i].fn == ui_wget || words[i].fn == ui_text_get;
        if (!forth_register_native(words[i].name, words[i].fn, foreground ? 0 : FORTH_NATIVE_TASK)) return 0;
    }
    return forth_eval(
        "1 CONSTANT UI-VISIBLE 2 CONSTANT UI-DECOR 4 CONSTANT UI-SHADOW 8 CONSTANT UI-ACTIVE "
        "16 CONSTANT UI-FREE-X 32 CONSTANT UI-MODAL "
        "2 CONSTANT UI-ENABLED 4 CONSTANT UI-CHECKED 8 CONSTANT UI-STATUS "
        "65535 CONSTANT UI-SEPARATOR : UI-SUBMENU ( menu -- item ) 65024 OR ; "
        "1 CONSTANT UI-CTRL 2 CONSTANT UI-SHIFT 4 CONSTANT UI-ALT "
        "6 CONSTANT UI-EV-FOCUS 16 CONSTANT UI-EV-WIN 17 CONSTANT UI-EV-COMMAND 14 CONSTANT UI-EV-OVERFLOW 15 CONSTANT UI-EV-ERROR "
        "15 CONSTANT UI-CURSOR 7 CONSTANT UI-EV-POINTER 18 CONSTANT UI-EV-ANIMDONE "
        "0 CONSTANT UI-TWEEN-WINDOW 1 CONSTANT UI-TWEEN-SPRITE 0 CONSTANT UI-X 1 CONSTANT UI-Y "
        "2 CONSTANT UI-SCROLL-ROW 3 CONSTANT UI-SCROLL-COL 0 CONSTANT UI-LINEAR 1 CONSTANT UI-IN-OUT "
        "2 CONSTANT UI-OUT 3 CONSTANT UI-SPRING "
        "1 CONSTANT UI-BOX 2 CONSTANT UI-LABEL 3 CONSTANT UI-BUTTON 4 CONSTANT UI-CHECK 5 CONSTANT UI-RADIO "
        "6 CONSTANT UI-INPUT 7 CONSTANT UI-LIST 8 CONSTANT UI-PROGRESS 1 CONSTANT UI-HIDDEN 2 CONSTANT UI-DISABLED "
        "4 CONSTANT UI-WF-FILL 1 CONSTANT UI-COLUMN 2 CONSTANT UI-ROW 255 CONSTANT UI-NONE "
        "2 CONSTANT UI-EV-ACTIVATE 3 CONSTANT UI-EV-SELECT 4 CONSTANT UI-EV-CHANGE "
        "9 CONSTANT UI-TEXTVIEW 10 CONSTANT UI-TABS 8 CONSTANT UI-READONLY 16 CONSTANT UI-TREE 32 CONSTANT UI-PAGER "
        "5 CONSTANT UI-EV-TEXT 8 CONSTANT UI-EV-EXPAND 9 CONSTANT UI-EV-PAGE "
        /* ( char id -- ) one character onto a TextView: /dev/ui/tree's newlines and quotes */
        "CREATE UI-CH-BUF 1 ALLOT : UI-CH >R UI-CH-BUF C! UI-CH-BUF 1 R> UI-TEXT+ ;");
}

/* VIOS Display Server wire protocol: CPU -> ESP `VDU 23,0,&E4,len,ops...`
 * batches and ESP -> CPU replies/events (packet codes 0x71 / 0x70, which
 * reach the CPU as 0xF1 / 0xF0). Portable C99, no heap.
 *
 * A batch is length-validated as a whole before any op is applied, so a
 * malformed batch is dropped without side effects and the stream can never
 * desynchronise. An op that is well formed but refers to something absent
 * (an unknown window, say) is skipped and reported as an ERROR event.
 * Multi-byte values are little endian; s16 is two's complement. Window ids
 * from VDS_PROTO_WINDOWS and surface ids from VDS_PROTO_SURFACES are reserved
 * for the intrinsics (menus, menu bar, status line) and refused here.
 *
 *  op  bytes          meaning
 *  01  id cols rows attr                 create/resize surface (cells from the adapter)
 *  10  id col row attr n ch*n            put a single-style run
 *  11  attr n ch*n                       continue after the previous PUT/MORE
 *  12  id col row n (ch attr)*n          mixed-style run
 *  13  id col row w h ch attr            fill a cell rectangle
 *  14  id dy(s8) attr                    scroll a surface (dy > 0 moves text up)
 *  15  id col row shape blink            text caret
 *  16  id col row w h dy(s8) attr       scroll a cell rectangle (dy > 0 moves text up)
 *  30                                    commit: show everything sent so far
 *  31  seq                               reply 31 seq epoch(u16) gen(u16) errors desync raw
 *  32  id col row n                      reply 32 n (ch attr)*n, n <= 14
 *  36  max                               poll events: reply 36 left n (len event)*n with
 *                                        n <= max, as many queued events as fit (events are pulled)
 *  33  sel                               reply 33 sel u32 u32 (0 frames rows, 1 pixels carried,
 *                                        2 lut_misses damage|windows<<8|sprites<<16)
 *  34                                    reset: drop windows, sprites, surfaces, errors; epoch++
 *  35  on                                raw: legacy VDU owns the screen while on
 *  50  id surface x(s16) y(s16) cols rows flags   open a window
 *  51  id x(s16) y(s16) cols rows        move and resize
 *  52  id                                raise
 *  53  id flags                          window flags
 *  54  id n ch*n                         title
 *  55  id surface                        rebind client surface
 *  56  id col row                        client scroll offset
 *  57  id                                close
 *  58  id                                focus (raise, activate; refused under a modal)
 *  59  id menu                           pop-up menu of a window (255: world menu)
 *  60  cmd(u16) flags vk mods ascii n ch*n   define a command ('&' marks the mnemonic)
 *  61  menu n title*n count item(u16)*count  define a menu (items: command, FFFF separator,
 *                                        FE00|menu submenu); menu 0 is the bar / world menu
 *  62  cmd(u16) flags                    command state (enabled 2, checked 4, status line 8)
 *  70  target id prop from(s16) to(s16) ms(u16) easing   tween (target 0 window, 1 sprite, 2 canvas;
 *                                        prop 0 x, 1 y, 2 scroll row, 3 scroll col;
 *                                        easing 0 linear, 1 in-out, 2 out, 3 spring)
 *  71  target id prop                    stop a tween where it is
 *  5A  id policy                         key policy (0 unused keys to the CPU, 1 dropped)
 *  74  key value(u16)                    config: 1 snap_x, 2 style, 3 nudge (1: one 1-byte event
 *                                        packet announces queued events until the next poll)
 *  76  id w h hot_x hot_y transparent    define a sprite (pixels from the pool)
 *  77  id offset(u16) n byte*n           sprite pixel data
 *  78  id x(s16) y(s16)                  move a sprite (hotspot position)
 *  79  id on                             show/hide a sprite
 *  7A  id                                define the built-in arrow cursor
 *  7B  x(s16) y(s16) buttons             pointer position and buttons (host or keyboard
 *                                        pointer; a press focuses the window under it)
 *  80  id window kind parent col row w h flags   widget (kinds: 1 box, 2 label, 3 button,
 *                                        4 check, 5 radio, 6 input, 7 list, 8 progress, 9 TextView,
 *                                        10 tabs; flags 1 hidden, 2 disabled, 4 fill, 8 read-only,
 *                                        16 tree, 32 pager)
 *  81  id n ch*n                         widget text ('&' marks a mnemonic)
 *  82  id value(s16)                     widget value
 *  83  id layout gap                     box layout (0 none, 1 column, 2 row)
 *  84  id fixed weight                   size inside a box: fixed cells, else by weight
 *  85  id index n ch*n                   list item
 *  86  id count                          list length
 *  87  id                                delete a widget and its children
 *  88  id                                focus a widget
 *  89  id                                reply 89 id value(s16) n ch*n (text, at most 25)
 *  8A  id mode n ch*n                    TextView text: mode 0 replace, 1 append
 *  8B  id caret(u16) anchor(u16)         TextView caret and selection
 *  8C  id offset(u16) n                  reply 8C id offset(u16) n ch*n (n <= 25)
 *  8D  id n ch*n                         TextView insert at the caret (replaces and then
 *                                        selects the inserted text)
 *  8E  id                                reply 8E id len(u16) caret(u16) anchor(u16) rev(u16) dirty
 *  90  id index(u16) n ch*n              list row (u16 index)
 *  91  id count(u16)                     list length (u16)
 *  92  id k (n ch*n)*k                   append k rows to a list
 *  93  id n width*n                      table column widths (n <= 8, 0: plain list)
 *  94  id n ch*n                         widget name (at most 10, for /dev/win)
 *  95                                    reply 95 widgets[8] windows[4] surfaces[2]: bit i set
 *                                        when id i is in use (CPU window and surface ids only)
 *  96  id                                reply 96 id kind window parent col row w h flags
 *                                        value(s16) count(u16) layout gap fixed weight style n name*n
 *  97  id                                reply 97 id flags x(s16) y(s16) cols rows surface
 *                                        surface_cols surface_rows focus(widget) n
 *                                        (title length; read the title with 98)
 *  98  what id index(u16) off            reply 98 what id index(u16) off n ch*n, n <= 23: text
 *                                        from off of what 0 widget text, 1 list row index,
 *                                        2 window title (n 0: past the end)
 *  99  id col row w h                    move/resize a widget in place
 *  9A  id flags                          widget flags in place (hide, disable, ...)
 *  9B  from                              reply 9B from n id*n: widget ids in creation order
 *                                        from position `from` (parents first), n <= 27
 *  9C  file node step(u16) sub(u16) off  reply 9C status step(u16) sub(u16) n ch*n, n <= 23: text
 *                                        of /dev/win file `file` (vds_files.c) for node from the
 *                                        line at cursor (step, sub), from byte off of that line.
 *                                        status 0 more of this line, 1 its last bytes (step, sub:
 *                                        the next line), 2 end of file, 3 error (text: why)
 *  9D  file node flags n ch*n            reply 9D ok node n ch*n: write to that file (flags 1:
 *                                        append); node: the one made (new); text: why it failed
 *  9E  node on                           route a node's events to it as STREAM events (13) whose
 *                                        arguments are its text line, in pieces of at most 23
 *                                        bytes (on 0: back to plain events)
 *  9F  id style                          widget style: 0 normal, 1 strong, 2 fault (red)
 *  G5 canvases (vds_canvas.h; coordinates s16, canvas pixels unless noted):
 *  C0  id surface x y w h bg flags       create/replace a canvas at surface pixel (x, y), x a
 *                                        multiple of 8 (flags 1 visible, 2 clear: bg shows the cells)
 *  C1  id x y                            move a canvas on its surface
 *  C2  id flags                          canvas flags
 *  C3  id                                delete a canvas
 *  C4  id item kind colour fields        set an item: 1 dot x y; 2 line x0 y0 x1 y1; 3 rect,
 *                                        4 bar, 5 oval, 6 disc x y w h; 7 text x y n ch*n (n <= 20);
 *                                        8 icon x y bitmap(u8); 9 stipple x y w h colour2(u8)
 *  C5  id item                           delete an item (255: all)
 *  C6  id item dx dy                     move an item
 *  C7  id item colour                    recolour an item
 *  C8  bitmap w h transparent            define bitmap 0..23 (pixels from the bitmap pool)
 *  C9  bitmap offset(u16) n byte*n       bitmap pixel data
 *  CA  id x y                            reply CA id item x y: topmost item there (255 none)
 *  CB  id                                reply CB id used flags surface x y w h items bytes(u32)
 *  CC  id item                           reply CC id item kind colour fields (as C4; kind 0: none)
 */
#ifndef VIOS_VDS_PROTO_H
#define VIOS_VDS_PROTO_H
#include "vds.h"
#include "vds_canvas.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VDS_PROTO_POOL 4096             /* sprite pixel bytes */
#define VDS_PROTO_BITMAP_POOL 16384     /* uploaded bitmap pixel bytes */
#define VDS_PROTO_BITMAPS 24            /* CPU bitmap ids 0..23; 24..31 are the built-in icons */
#define VDS_PROTO_WINDOWS 27            /* CPU window ids 0..26 */
#define VDS_PROTO_SURFACES 11           /* CPU surface ids 0..10 */
#define VDS_PROTO_HOLD_MS 33            /* uncommitted changes show after this */
#define VDS_PACKET_EVENT 0x70
#define VDS_PACKET_REPLY 0x71
#define VDS_EVENT_ERROR 0x0F
#define VDS_EVENT_OVERFLOW 0x0E        /* args: events lost */
#define VDS_EVENT_QUEUE 16
#define VDS_EVENT_MAX 29                /* seq kind id + 26 argument bytes */
#define VDS_REPLY_MAX 30                /* the CPU keeps 32-byte reply slots */

typedef struct vds_proto {
    vds *v;
    /* Cell memory for surface id (cols * rows cells); NULL refuses the op. */
    vds_cell *(*cells)(void *ctx, uint8_t id, uint16_t count);
    /* One ESP -> CPU packet: code VDS_PACKET_EVENT or VDS_PACKET_REPLY. */
    void (*send)(void *ctx, uint8_t code, const uint8_t *data, uint8_t n);
    void *ctx;
    uint8_t pool[VDS_PROTO_POOL];
    uint16_t pool_used, sprite_off[VDS_MAX_SPRITES], sprite_len[VDS_MAX_SPRITES];
    /* Canvas memory for canvas id (vds_canvas_bytes); NULL refuses canvases.
     * Set after vds_proto_init (PSRAM on the device). */
    void *(*canvas_mem)(void *ctx, uint8_t id, uint32_t bytes);
    uint8_t bitmap_pool[VDS_PROTO_BITMAP_POOL];
    uint16_t bitmap_used, bitmap_off[VDS_PROTO_BITMAPS], bitmap_len[VDS_PROTO_BITMAPS];
    uint8_t more_ok, more_id, more_col, more_row;
    uint8_t raw, hold, errors, desync, event_seq;
    uint16_t epoch, generation;
    uint32_t held_ms;
    /* Events are queued for the CPU to poll (op 36) unless push_events is set,
     * in which case each one is sent at once as a VDS_PACKET_EVENT. */
    uint8_t push_events, event_head, event_count, event_lost;
    /* Nudge (config 3): one 1-byte VDS_PACKET_EVENT when an event is queued
     * and none was announced since the last poll, so the CPU polls on demand. */
    uint8_t nudge, nudged;
    uint8_t event_len[VDS_EVENT_QUEUE], events[VDS_EVENT_QUEUE][VDS_EVENT_MAX];
    /* Optional intrinsics (vds_ui sets these): CPU focus requests, closes. */
    void *ui;
    int (*ui_focus)(void *ui, uint8_t id);
    void (*ui_forget)(void *ui, uint8_t id);
    int (*ui_op)(void *ui, const uint8_t *op);   /* ops 59-62, 70, 71, 7B */
    void (*ui_tick)(void *ui, uint32_t ms);      /* once per frame, before composing */
    void (*ui_after)(void *ui);                  /* after every applied batch */
    void (*ui_reset)(void *ui);
    /* Optional: may take an event before it is queued (returns 1 if it did). */
    int (*ui_route)(void *ui, uint8_t kind, uint8_t id, const uint8_t *args, uint8_t n);
    /* Optional: called between the ops of a batch and before ui_after, where
     * the state is consistent (composition is held while a batch runs). The
     * host may let a waiting frame in here instead of after the whole batch. */
    void (*yield)(void *ctx);
} vds_proto;

void vds_proto_init(vds_proto *p, vds *v, vds_cell *(*cells)(void *, uint8_t, uint16_t),
                    void (*send)(void *, uint8_t, const uint8_t *, uint8_t), void *ctx);
/* Apply one E4 payload. Returns 1 if applied, 0 if rejected as malformed. */
int vds_proto_batch(vds_proto *p, const uint8_t *payload, size_t len);
/* Per vertical blank: advance time, rasterise dirty canvases and compose
 * within the budget (canvases take up to half of it first) unless changes are
 * held for a COMMIT or the screen is raw. Returns 1 when nothing is left. */
int vds_proto_frame(vds_proto *p, uint32_t elapsed_ms, uint32_t budget);
/* With a time budget too (vds_frame_within). */
int vds_proto_frame_within(vds_proto *p, uint32_t elapsed_ms, uint32_t budget, uint32_t budget_us);
/* One reply packet (VDS_PACKET_REPLY) to the CPU, at most VDS_REPLY_MAX bytes. */
void vds_proto_reply(vds_proto *p, const uint8_t *data, uint8_t n);
/* Queue (or push) one event for the CPU: kind, window/widget id and up to 26 bytes. */
void vds_proto_event(vds_proto *p, uint8_t kind, uint8_t id, const uint8_t *args, uint8_t n);

#ifdef __cplusplus
}
#endif
#endif

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
 *  70  target id prop from(s16) to(s16) ms(u16) easing   tween (target 0 window, 1 sprite;
 *                                        prop 0 x, 1 y, 2 scroll row, 3 scroll col;
 *                                        easing 0 linear, 1 in-out, 2 out, 3 spring)
 *  71  target id prop                    stop a tween where it is
 *  74  key value(u16)                    config: 1 snap_x, 2 style
 *  76  id w h hot_x hot_y transparent    define a sprite (pixels from the pool)
 *  77  id offset(u16) n byte*n           sprite pixel data
 *  78  id x(s16) y(s16)                  move a sprite (hotspot position)
 *  79  id on                             show/hide a sprite
 *  7A  id                                define the built-in arrow cursor
 *  7B  x(s16) y(s16) buttons             pointer position and buttons (host or keyboard
 *                                        pointer; a press focuses the window under it)
 *  80  id window kind parent col row w h flags   widget (kinds: 1 box, 2 label, 3 button,
 *                                        4 check, 5 radio, 6 input, 7 list, 8 progress)
 *  81  id n ch*n                         widget text ('&' marks a mnemonic)
 *  82  id value(s16)                     widget value
 *  83  id layout gap                     box layout (0 none, 1 column, 2 row)
 *  84  id fixed weight                   size inside a box: fixed cells, else by weight
 *  85  id index n ch*n                   list item
 *  86  id count                          list length
 *  87  id                                delete a widget and its children
 *  88  id                                focus a widget
 *  89  id                                reply 89 id value(s16) n ch*n (text, at most 25)
 */
#ifndef VIOS_VDS_PROTO_H
#define VIOS_VDS_PROTO_H
#include "vds.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VDS_PROTO_POOL 4096             /* sprite pixel bytes */
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
    uint8_t more_ok, more_id, more_col, more_row;
    uint8_t raw, hold, errors, desync, event_seq;
    uint16_t epoch, generation;
    uint32_t held_ms;
    /* Events are queued for the CPU to poll (op 36) unless push_events is set,
     * in which case each one is sent at once as a VDS_PACKET_EVENT. */
    uint8_t push_events, event_head, event_count, event_lost;
    uint8_t event_len[VDS_EVENT_QUEUE], events[VDS_EVENT_QUEUE][VDS_EVENT_MAX];
    /* Optional intrinsics (vds_ui sets these): CPU focus requests, closes. */
    void *ui;
    int (*ui_focus)(void *ui, uint8_t id);
    void (*ui_forget)(void *ui, uint8_t id);
    int (*ui_op)(void *ui, const uint8_t *op);   /* ops 59-62, 70, 71, 7B */
    void (*ui_tick)(void *ui, uint32_t ms);      /* once per frame, before composing */
    void (*ui_after)(void *ui);                  /* after every applied batch */
    void (*ui_reset)(void *ui);
} vds_proto;

void vds_proto_init(vds_proto *p, vds *v, vds_cell *(*cells)(void *, uint8_t, uint16_t),
                    void (*send)(void *, uint8_t, const uint8_t *, uint8_t), void *ctx);
/* Apply one E4 payload. Returns 1 if applied, 0 if rejected as malformed. */
int vds_proto_batch(vds_proto *p, const uint8_t *payload, size_t len);
/* Per vertical blank: advance time and compose within the budget unless
 * changes are held for a COMMIT or the screen is raw. Returns vds_frame's
 * result (1 when nothing is left). */
int vds_proto_frame(vds_proto *p, uint32_t elapsed_ms, uint32_t budget);
/* One reply packet (VDS_PACKET_REPLY) to the CPU, at most VDS_REPLY_MAX bytes. */
void vds_proto_reply(vds_proto *p, const uint8_t *data, uint8_t n);
/* Queue (or push) one event for the CPU: kind, window/widget id and up to 26 bytes. */
void vds_proto_event(vds_proto *p, uint8_t kind, uint8_t id, const uint8_t *args, uint8_t n);

#ifdef __cplusplus
}
#endif
#endif

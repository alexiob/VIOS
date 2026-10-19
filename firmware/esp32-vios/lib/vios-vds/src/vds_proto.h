/* VIOS Display Server wire protocol: CPU -> ESP `VDU 23,0,&E4,len,ops...`
 * batches and ESP -> CPU replies/events (packet codes 0x71 / 0x70, which
 * reach the CPU as 0xF1 / 0xF0). Portable C99, no heap.
 *
 * A batch is length-validated as a whole before any op is applied, so a
 * malformed batch is dropped without side effects and the stream can never
 * desynchronise. An op that is well formed but refers to something absent
 * (an unknown window, say) is skipped and reported as an ERROR event.
 * Multi-byte values are little endian; s16 is two's complement.
 *
 *  op  bytes          meaning
 *  01  id cols rows attr                 create/resize surface (cells from the adapter)
 *  10  id col row attr n ch*n            put a single-style run
 *  11  attr n ch*n                       continue after the previous PUT/MORE
 *  12  id col row n (ch attr)*n          mixed-style run
 *  13  id col row w h ch attr            fill a cell rectangle
 *  14  id dy(s8) attr                    scroll a surface (dy > 0 moves text up)
 *  15  id col row shape blink            text caret
 *  30                                    commit: show everything sent so far
 *  31  seq                               reply 31 seq epoch(u16) gen(u16) errors desync raw
 *  32  id col row n                      reply 32 n (ch attr)*n, n <= 14
 *  33  sel                               reply 33 sel u32 u32 (0 frames rows, 1 pixels carried,
 *                                        2 lut_misses damage|windows<<8|sprites<<16)
 *  34                                    reset: drop windows, sprites, surfaces; epoch++
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
 *  74  key value(u16)                    config: 1 snap_x, 2 style
 *  76  id w h hot_x hot_y transparent    define a sprite (pixels from the pool)
 *  77  id offset(u16) n byte*n           sprite pixel data
 *  78  id x(s16) y(s16)                  move a sprite (hotspot position)
 *  79  id on                             show/hide a sprite
 *  7A  id                                define the built-in arrow cursor
 */
#ifndef VIOS_VDS_PROTO_H
#define VIOS_VDS_PROTO_H
#include "vds.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VDS_PROTO_POOL 4096             /* sprite pixel bytes */
#define VDS_PROTO_HOLD_MS 33            /* uncommitted changes show after this */
#define VDS_PACKET_EVENT 0x70
#define VDS_PACKET_REPLY 0x71
#define VDS_EVENT_ERROR 0x0F

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
    /* Optional intrinsics (vds_ui sets these): CPU focus requests, closes. */
    void *ui;
    int (*ui_focus)(void *ui, uint8_t id);
    void (*ui_forget)(void *ui, uint8_t id);
} vds_proto;

void vds_proto_init(vds_proto *p, vds *v, vds_cell *(*cells)(void *, uint8_t, uint16_t),
                    void (*send)(void *, uint8_t, const uint8_t *, uint8_t), void *ctx);
/* Apply one E4 payload. Returns 1 if applied, 0 if rejected as malformed. */
int vds_proto_batch(vds_proto *p, const uint8_t *payload, size_t len);
/* Per vertical blank: advance time and compose within the budget unless
 * changes are held for a COMMIT or the screen is raw. Returns vds_frame's
 * result (1 when nothing is left). */
int vds_proto_frame(vds_proto *p, uint32_t elapsed_ms, uint32_t budget);
/* Queue one event to the CPU: kind, window/widget id and up to 26 bytes. */
void vds_proto_event(vds_proto *p, uint8_t kind, uint8_t id, const uint8_t *args, uint8_t n);

#ifdef __cplusplus
}
#endif
#endif

#ifndef VIOS_VDP_TRANSPORT_H
#define VIOS_VDP_TRANSPORT_H
#include <stddef.h>
#include "platform/input.h"
#define VDP_HISTORY 4
struct vdp_reply { unsigned char type,length,valid,data[32]; };
struct vdp_transport {
 unsigned char state,type,length,index,scratch[32];
 unsigned char input[512];unsigned short head,tail;
 struct vdp_reply replies[8];unsigned dropped;
 struct platform_key_event keys[32];
 unsigned char key_head,key_count,key_route;unsigned key_dropped;
 /* Cooked mode keeps physical keystrokes out of partial host lines. */
 unsigned char line_mode,host_open,local_pending,local_used,local[235];unsigned local_dropped;
 /* Line editing: cursor within local; local_erase asks the platform echo to
  * clear the shown line once it is handed to the REPL. Up/Down recall the
  * last VDP_HISTORY lines (newest first). */
 unsigned char local_cursor,local_erase;
 unsigned char history[4][235],history_length[4],history_count,history_next,history_pos;
 /* F1 pressed while keys are not routed to the UI: a view-switch request. */
 unsigned char hotkey;
 unsigned char interrupt;   /* Ctrl-C: physical key, or a host line starting with 3 */
 /* Host-service reply (packet FC): whole payload, one in flight. */
 unsigned char host[240],host_length,host_ready;
};
void vdp_transport_init(struct vdp_transport *p);
void vdp_transport_keyboard_route(struct vdp_transport *p,int enabled);
int vdp_transport_keyboard_event(struct vdp_transport *p,struct platform_key_event *out);
/* 1: physical ASCII is edited locally and inserted as one CR-terminated line
 * only between host lines; 0: each key-down byte is console input (KEY). */
void vdp_transport_line_mode(struct vdp_transport *p,int enabled);
void vdp_transport_feed(struct vdp_transport *p,unsigned char byte);
int vdp_transport_getc(struct vdp_transport *p);
/* 1 once per Ctrl-C seen since the last call (tasks design B2: Ctrl-C at preemption points). */
int vdp_transport_take_interrupt(struct vdp_transport *p);
/* Take the latest host-service reply; 0 if none has arrived. */
int vdp_transport_host_reply(struct vdp_transport *p,unsigned char *data,unsigned capacity,unsigned *length);
int vdp_transport_packet(struct vdp_transport *p,unsigned char type,void *data,size_t capacity,unsigned *length);
#endif

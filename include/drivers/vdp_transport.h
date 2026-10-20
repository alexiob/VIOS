#ifndef VIOS_VDP_TRANSPORT_H
#define VIOS_VDP_TRANSPORT_H
#include <stddef.h>
#include "platform/input.h"
struct vdp_reply { unsigned char type,length,valid,data[32]; };
struct vdp_transport {
 unsigned char state,type,length,index,scratch[32];
 unsigned char input[512];unsigned short head,tail;
 struct vdp_reply replies[8];unsigned dropped;
 struct platform_key_event keys[32];
 unsigned char key_head,key_count,key_route;unsigned key_dropped;
 /* Line mode (the REPL awaits a line): physical lines are edited on the ESP
  * (firmware/esp32-vios/include/vios_line.h, GUI toolkit Stage 2a) and arrive
  * whole as FE host lines. A key that still reaches the CPU in line mode (sent
  * before the ESP learned of the mode) is plain input for the REPL's reader;
  * inside a partial host line it would split it, so it is dropped and counted. */
 unsigned char line_mode,host_open;unsigned local_dropped;
 /* F1 pressed while keys are not routed to the UI: a view-switch request. */
 unsigned char hotkey;
 unsigned char interrupt;   /* Ctrl-C: physical key, or a host line starting with 3 */
 /* Host-service reply (packet FC): whole payload, one in flight. */
 unsigned char host[240],host_length,host_ready;
};
void vdp_transport_init(struct vdp_transport *p);
void vdp_transport_keyboard_route(struct vdp_transport *p,int enabled);
int vdp_transport_keyboard_event(struct vdp_transport *p,struct platform_key_event *out);
/* 1: the REPL awaits a line (the ESP edits physical lines); 0: a command runs
 * and each key-down byte is console input (KEY), Ctrl-C interrupts it. */
void vdp_transport_line_mode(struct vdp_transport *p,int enabled);
void vdp_transport_feed(struct vdp_transport *p,unsigned char byte);
int vdp_transport_getc(struct vdp_transport *p);
/* 1 once per Ctrl-C seen since the last call (tasks design B2: Ctrl-C at preemption points). */
int vdp_transport_take_interrupt(struct vdp_transport *p);
/* Take the latest host-service reply; 0 if none has arrived. */
int vdp_transport_host_reply(struct vdp_transport *p,unsigned char *data,unsigned capacity,unsigned *length);
int vdp_transport_packet(struct vdp_transport *p,unsigned char type,void *data,size_t capacity,unsigned *length);
#endif

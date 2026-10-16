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
};
void vdp_transport_init(struct vdp_transport *p);
void vdp_transport_keyboard_route(struct vdp_transport *p,int enabled);
int vdp_transport_keyboard_event(struct vdp_transport *p,struct platform_key_event *out);
void vdp_transport_feed(struct vdp_transport *p,unsigned char byte);
int vdp_transport_getc(struct vdp_transport *p);
int vdp_transport_packet(struct vdp_transport *p,unsigned char type,void *data,size_t capacity,unsigned *length);
#endif

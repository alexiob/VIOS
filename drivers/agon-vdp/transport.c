/* Parse standard VDP replies and the reserved VIOS console input packet. */
#include "drivers/vdp_transport.h"
void vdp_transport_init(struct vdp_transport *p) {
 p->state=p->head=p->tail=0;p->dropped=0;
 p->key_head=p->key_count=p->key_route=0;p->key_dropped=0;
 p->line_mode=p->host_open=0;p->local_dropped=0;p->hotkey=p->interrupt=0;p->host_length=p->host_ready=0;
 for(unsigned i=0;i<8;++i)p->replies[i].valid=p->replies[i].type=0;
}
void vdp_transport_keyboard_route(struct vdp_transport *p,int enabled) {
 unsigned char route=(unsigned char)(enabled!=0);
 if(p->key_route!=route){p->key_head=p->key_count=0;p->key_route=route;}
}
int vdp_transport_keyboard_event(struct vdp_transport *p,struct platform_key_event *out) {
 if(!out||!p->key_count)return 0;
 *out=p->keys[p->key_head];p->key_head=(unsigned char)((p->key_head+1)&31);--p->key_count;return 1;
}
static void keyboard(struct vdp_transport *p) {
 if(p->key_count==32){++p->key_dropped;++p->dropped;return;}
 struct platform_key_event *event=&p->keys[(p->key_head+p->key_count)&31];
 event->ascii=p->scratch[0];event->modifiers=p->scratch[1];
 event->virtual_key=p->scratch[2];event->down=p->scratch[3];++p->key_count;
}
static void input(struct vdp_transport *p,unsigned char byte) {
 unsigned short next=(unsigned short)((p->tail+1)&511);
 if(next==p->head){++p->dropped;return;}
 p->input[p->tail]=byte;p->tail=next;
}
static void host(struct vdp_transport *p,unsigned char byte) {
 if(byte==3&&!p->host_open&&!p->line_mode){p->interrupt=1;return;}   /* while a command runs; never inside a request line */
 input(p,byte);p->host_open=!(byte=='\r'||byte=='\n'||byte==3);
}
/* A key-down the ESP did not consume (see the header). While a command runs
 * it is console input and Ctrl-C interrupts it; in line mode it is input for
 * the REPL's reader (which echoes it and handles Backspace), except Esc and
 * Ctrl-C, and never inside a partial host line. */
static void local(struct vdp_transport *p,unsigned char c) {
 if(!p->line_mode){if(c==3){p->interrupt=1;return;}if(c)input(p,c);return;}   /* Ctrl-C is not also input */
 if(!c||c==3||c==27)return;
 if(p->host_open){++p->local_dropped;return;}
 input(p,c=='\n'?'\r':c);
}
void vdp_transport_line_mode(struct vdp_transport *p,int enabled) {p->line_mode=(unsigned char)(enabled!=0);}
void vdp_transport_feed(struct vdp_transport *p,unsigned char byte) {
 if(!p->state) {if(byte>=0x80){p->type=byte;p->state=1;}return;}
 if(p->state==1) {p->index=0;p->length=byte;p->state=byte?2:0;return;}
 if(p->type==0xFE)host(p,byte);
 else if(p->type==0xFC){if(p->index<sizeof p->host)p->host[p->index]=byte;}
 else if(p->index<sizeof p->scratch)p->scratch[p->index]=byte;
 if(++p->index!=p->length)return;
 p->state=0;
 if(p->type==0xFE)return;
 if(p->type==0xFC){p->host_length=p->length>sizeof p->host?0:p->length;p->host_ready=p->length<=sizeof p->host;return;}
 if(p->length>sizeof p->scratch){++p->dropped;return;}
 if(p->type==0x81&&p->length>=4) {
  if(p->key_route){if(p->length==4)keyboard(p);}
  else if(p->scratch[3]&&p->scratch[2]==PLATFORM_KEY_F1)p->hotkey=1;
  else if(p->scratch[3])local(p,p->scratch[0]);
 }
 unsigned slot=8;
 for(unsigned i=0;i<8;++i)if(p->replies[i].type==p->type){slot=i;break;}
 if(slot==8)for(unsigned i=0;i<8;++i)if(!p->replies[i].valid){slot=i;break;}
 if(slot==8){slot=0;++p->dropped;}
 struct vdp_reply *r=&p->replies[slot];
 r->type=p->type;r->length=p->length;r->valid=1;
 for(unsigned i=0;i<r->length;++i)r->data[i]=p->scratch[i];
}
int vdp_transport_take_interrupt(struct vdp_transport *p){int hit=p->interrupt;p->interrupt=0;return hit;}
int vdp_transport_getc(struct vdp_transport *p) {
 if(p->head==p->tail)return -1;
 int c=p->input[p->head];p->head=(unsigned short)((p->head+1)&511);return c;
}
int vdp_transport_packet(struct vdp_transport *p,unsigned char type,void *data,size_t capacity,unsigned *length) {
 if(length)*length=0;
 for(unsigned i=0;i<8;++i)if(p->replies[i].valid&&p->replies[i].type==type) {
  struct vdp_reply *r=&p->replies[i];
  if(r->length>capacity)return 0;
  unsigned char *out=data;for(unsigned n=0;n<r->length;++n)out[n]=r->data[n];
  if(length)*length=r->length;r->valid=0;return 1;
 }
 return 0;
}
int vdp_transport_host_reply(struct vdp_transport *p,unsigned char *data,unsigned capacity,unsigned *length) {
 if(!p->host_ready||p->host_length>capacity)return 0;
 for(unsigned i=0;i<p->host_length;++i)data[i]=p->host[i];
 *length=p->host_length;p->host_ready=0;return 1;
}

/* Parse standard VDP replies and the reserved VIOS console input packet. */
#include "drivers/vdp_transport.h"
void vdp_transport_init(struct vdp_transport *p) {
 p->state=p->head=p->tail=0;p->dropped=0;
 p->key_head=p->key_count=p->key_route=0;p->key_dropped=0;
 p->line_mode=p->host_open=p->local_pending=p->local_used=0;p->local_dropped=0;p->local_cursor=p->local_erase=p->hotkey=0;p->host_length=p->host_ready=0;p->history_count=p->history_next=p->history_pos=0;
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
static void flush_local(struct vdp_transport *p) {
 if(!p->local_pending||p->host_open||!p->line_mode)return;
 unsigned used=(unsigned)((p->tail-p->head)&511);
 if(511-used<(unsigned)p->local_used+1u)return;
 for(unsigned i=0;i<p->local_used;++i)input(p,p->local[i]);
 input(p,'\r');p->local_used=p->local_cursor=0;p->local_pending=0;p->local_erase=1;
}
static void host(struct vdp_transport *p,unsigned char byte) {
 input(p,byte);p->host_open=!(byte=='\r'||byte=='\n'||byte==3);flush_local(p);
}
static void remember(struct vdp_transport *p) {
 if(!p->local_used)return;
 unsigned last=(p->history_next+VDP_HISTORY-1)%VDP_HISTORY;
 if(p->history_count&&p->history_length[last]==p->local_used){
  unsigned same=1;for(unsigned i=0;i<p->local_used;++i)if(p->history[last][i]!=p->local[i])same=0;if(same)return;
 }
 for(unsigned i=0;i<p->local_used;++i)p->history[p->history_next][i]=p->local[i];
 p->history_length[p->history_next]=p->local_used;p->history_next=(unsigned char)((p->history_next+1)%VDP_HISTORY);
 if(p->history_count<VDP_HISTORY)++p->history_count;
}
static void recall(struct vdp_transport *p,int older) {
 /* history_pos 0 is the line being edited; 1 is the newest entry. */
 unsigned pos=p->history_pos;
 if(older){if(pos>=p->history_count)return;++pos;}else{if(!pos)return;--pos;}
 p->history_pos=(unsigned char)pos;p->local_used=0;
 if(pos){unsigned e=(p->history_next+VDP_HISTORY-pos)%VDP_HISTORY;for(unsigned i=0;i<p->history_length[e];++i)p->local[i]=p->history[e][i];p->local_used=p->history_length[e];}
 p->local_cursor=p->local_used;
}
static void erase_at(struct vdp_transport *p,unsigned at) {
 for(unsigned i=at;i+1<p->local_used;++i)p->local[i]=p->local[i+1];--p->local_used;
}
/* Cooked line editor: insert at cursor, Left/Right/Home/End, Backspace and
 * Delete, Esc/Ctrl-C clear, Up/Down history. Raw mode passes ASCII through. */
static void local(struct vdp_transport *p,unsigned char c,unsigned char key) {
 if(!p->line_mode){if(c)input(p,c);return;}
 if(p->local_pending){++p->local_dropped;return;}
 if(key==PLATFORM_KEY_RETURN||key==PLATFORM_KEY_KP_ENTER||c=='\r'||c=='\n'){remember(p);p->history_pos=0;p->local_pending=1;flush_local(p);}
 else if(key==PLATFORM_KEY_LEFT||key==PLATFORM_KEY_KP_LEFT){if(p->local_cursor)--p->local_cursor;}
 else if(key==PLATFORM_KEY_RIGHT||key==PLATFORM_KEY_KP_RIGHT){if(p->local_cursor<p->local_used)++p->local_cursor;}
 else if(key==PLATFORM_KEY_HOME||key==PLATFORM_KEY_KP_HOME)p->local_cursor=0;
 else if(key==PLATFORM_KEY_END||key==PLATFORM_KEY_KP_END)p->local_cursor=p->local_used;
 else if(key==PLATFORM_KEY_UP||key==PLATFORM_KEY_KP_UP)recall(p,1);
 else if(key==PLATFORM_KEY_DOWN||key==PLATFORM_KEY_KP_DOWN)recall(p,0);
 else if(key==PLATFORM_KEY_DELETE){if(p->local_cursor<p->local_used)erase_at(p,p->local_cursor);}
 else if(key==PLATFORM_KEY_BACKSPACE||c==8||c==127){if(p->local_cursor){--p->local_cursor;erase_at(p,p->local_cursor);}}
 else if(key==PLATFORM_KEY_ESCAPE||c==3||c==27){p->local_used=p->local_cursor=0;p->history_pos=0;}
 else if(c>=32&&c<127&&p->local_used<sizeof p->local){
  for(unsigned i=p->local_used;i>p->local_cursor;--i)p->local[i]=p->local[i-1];
  p->local[p->local_cursor++]=c;++p->local_used;
 }
 else if(c>=32&&c<127)++p->local_dropped;
}
void vdp_transport_line_mode(struct vdp_transport *p,int enabled) {p->line_mode=(unsigned char)(enabled!=0);flush_local(p);}
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
  else if(p->scratch[3])local(p,p->scratch[0],p->scratch[2]);
 }
 unsigned slot=8;
 for(unsigned i=0;i<8;++i)if(p->replies[i].type==p->type){slot=i;break;}
 if(slot==8)for(unsigned i=0;i<8;++i)if(!p->replies[i].valid){slot=i;break;}
 if(slot==8){slot=0;++p->dropped;}
 struct vdp_reply *r=&p->replies[slot];
 r->type=p->type;r->length=p->length;r->valid=1;
 for(unsigned i=0;i<r->length;++i)r->data[i]=p->scratch[i];
}
int vdp_transport_getc(struct vdp_transport *p) {
 if(p->head==p->tail){flush_local(p);if(p->head==p->tail)return -1;}
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

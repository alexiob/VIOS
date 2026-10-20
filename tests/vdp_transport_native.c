#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "drivers/vdp_transport.h"
int main(void) {
 struct vdp_transport p;vdp_transport_init(&p);
 const unsigned char input[]={0x86,8,0x80,2,0xE0,1,80,30,16,1,0xFE,4,'A',0,0x86,'Z',0x81,4,'K',0,0,1};
 for(size_t i=0;i<sizeof input;++i)vdp_transport_feed(&p,input[i]);
 assert(vdp_transport_getc(&p)=='A');assert(vdp_transport_getc(&p)==0);
 assert(vdp_transport_getc(&p)==0x86);assert(vdp_transport_getc(&p)=='Z');
 assert(vdp_transport_getc(&p)=='K');assert(vdp_transport_getc(&p)==-1);
 unsigned char payload[32];unsigned length=0;
 assert(vdp_transport_packet(&p,0x86,payload,sizeof payload,&length));
 assert(length==8&&payload[0]==0x80&&payload[1]==2&&payload[2]==0xE0);
 assert(!vdp_transport_packet(&p,0x86,payload,sizeof payload,&length));
 /* Full byte length is consumed even when metadata storage is smaller. */
 vdp_transport_feed(&p,0x90);vdp_transport_feed(&p,255);
 for(unsigned i=0;i<255;++i)vdp_transport_feed(&p,(unsigned char)i);
 vdp_transport_feed(&p,0xFE);vdp_transport_feed(&p,1);vdp_transport_feed(&p,'Q');
 assert(vdp_transport_getc(&p)=='Q');
 assert(!vdp_transport_packet(&p,0x90,payload,sizeof payload,&length));
 /* Overflow is counted, never overwrites older unread input. */
 for(unsigned i=0;i<3;++i){vdp_transport_feed(&p,0xFE);vdp_transport_feed(&p,240);for(unsigned j=0;j<240;++j)vdp_transport_feed(&p,'a');}
 assert(p.dropped>0);for(unsigned i=0;i<511;++i)assert(vdp_transport_getc(&p)=='a');
 assert(vdp_transport_getc(&p)==-1);
 /* UI routing isolates physical packets from the FE host console. */
 vdp_transport_keyboard_route(&p,1);
 const unsigned char keys[]={0x81,4,'X',2,163,1,0xFE,1,'H',0x81,4,'X',2,163,0};
 for(size_t i=0;i<sizeof keys;++i)vdp_transport_feed(&p,keys[i]);
 assert(vdp_transport_getc(&p)=='H');assert(vdp_transport_getc(&p)==-1);
 struct platform_key_event event;
 assert(vdp_transport_keyboard_event(&p,&event));
 assert(event.ascii=='X'&&event.modifiers==2&&event.virtual_key==163&&event.down==1);
 assert(vdp_transport_keyboard_event(&p,&event)&&event.down==0);
 assert(!vdp_transport_keyboard_event(&p,&event));
 for(unsigned n=0;n<40;++n){
  vdp_transport_feed(&p,0x81);vdp_transport_feed(&p,4);
  vdp_transport_feed(&p,(unsigned char)n);vdp_transport_feed(&p,0);vdp_transport_feed(&p,150);vdp_transport_feed(&p,1);
 }
 assert(p.key_dropped==8);
 for(unsigned n=0;n<32;++n)assert(vdp_transport_keyboard_event(&p,&event)&&event.ascii==n);
 assert(!vdp_transport_keyboard_event(&p,&event));assert(vdp_transport_getc(&p)==-1);
 /* Toggling routing discards stale UI events and restores legacy ASCII. */
 vdp_transport_keyboard_route(&p,0);
 for(size_t i=0;i<sizeof keys;++i)vdp_transport_feed(&p,keys[i]);
 assert(vdp_transport_getc(&p)=='X');assert(vdp_transport_getc(&p)=='H');assert(vdp_transport_getc(&p)==-1);
 assert(!vdp_transport_keyboard_event(&p,&event));
 /* Line mode (Stage 2a): the ESP edits physical lines; a key that still
  * reaches the CPU is plain input for the REPL's reader, except Esc, Ctrl-C
  * and non-ASCII keys, and never inside a partial host line. */
 vdp_transport_init(&p);vdp_transport_line_mode(&p,1);
 #define KEY(a,vk) do{const unsigned char k_[]={0x81,4,(unsigned char)(a),0,(unsigned char)(vk),1};for(size_t j_=0;j_<sizeof k_;++j_)vdp_transport_feed(&p,k_[j_]);}while(0)
 const unsigned char host_open[]={0xFE,3,'@','1','2'};
 for(size_t i=0;i<sizeof host_open;++i)vdp_transport_feed(&p,host_open[i]);
 KEY('X',0);assert(p.local_dropped==1&&"a key inside a partial host line is counted, not merged");
 const unsigned char host_end[]={0xFE,2,'3','\r'};
 for(size_t i=0;i<sizeof host_end;++i)vdp_transport_feed(&p,host_end[i]);
 KEY('a',0);KEY(27,PLATFORM_KEY_ESCAPE);KEY(3,0);KEY(0,PLATFORM_KEY_LEFT);KEY(8,PLATFORM_KEY_BACKSPACE);KEY('c',0);KEY(13,PLATFORM_KEY_RETURN);KEY('\n',0);
 const char expect[]="@123\ra\bc\r\r";
 for(size_t i=0;expect[i];++i)assert(vdp_transport_getc(&p)==expect[i]);
 assert(vdp_transport_getc(&p)==-1&&!vdp_transport_take_interrupt(&p)&&p.local_dropped==1);
 #undef KEY
 vdp_transport_line_mode(&p,0);
 const unsigned char raw[]={0x81,4,'k',0,0,1};for(size_t j=0;j<sizeof raw;++j)vdp_transport_feed(&p,raw[j]);
 assert(vdp_transport_getc(&p)=='k'&&vdp_transport_getc(&p)==-1);
 /* Host-service reply packets arrive whole and are taken exactly once. */
 {vdp_transport_init(&p);unsigned char reply[240];unsigned n=0;assert(!vdp_transport_host_reply(&p,reply,sizeof reply,&n));
  const unsigned char pkt[]={0xFC,5,'K',1,2,3,4,0xFE,1,'z'};for(size_t j=0;j<sizeof pkt;++j)vdp_transport_feed(&p,pkt[j]);
  assert(vdp_transport_host_reply(&p,reply,sizeof reply,&n)&&n==5&&reply[0]=='K'&&reply[4]==4);
  assert(!vdp_transport_host_reply(&p,reply,sizeof reply,&n));assert(vdp_transport_getc(&p)=='z');
  vdp_transport_feed(&p,0xFC);vdp_transport_feed(&p,200);for(unsigned j=0;j<200;++j)vdp_transport_feed(&p,(unsigned char)j);
  assert(vdp_transport_host_reply(&p,reply,sizeof reply,&n)&&n==200&&reply[199]==199);}
 /* Ctrl-C interrupts only while a command runs (line mode off): a key 3, or a
  * host byte 3 that starts a line; at the prompt it only clears the line, and
  * a 3 inside a host request line is data. */
 {struct vdp_transport q;vdp_transport_init(&q);
  const unsigned char ctrl_c[]={0x81,4,3,0,0,1},host3[]={0xFE,1,3},hostmid[]={0xFE,3,'a','b',3};
  vdp_transport_line_mode(&q,1);for(size_t i=0;i<sizeof ctrl_c;++i)vdp_transport_feed(&q,ctrl_c[i]);
  assert(!vdp_transport_take_interrupt(&q));
  vdp_transport_line_mode(&q,0);for(size_t i=0;i<sizeof ctrl_c;++i)vdp_transport_feed(&q,ctrl_c[i]);
  assert(vdp_transport_take_interrupt(&q)&&!vdp_transport_take_interrupt(&q));
  assert(vdp_transport_getc(&q)==-1&&"the interrupting Ctrl-C is not also input");
  for(size_t i=0;i<sizeof hostmid;++i)vdp_transport_feed(&q,hostmid[i]);assert(!vdp_transport_take_interrupt(&q));
  const unsigned char cr[]={0xFE,1,'\r'};for(size_t i=0;i<sizeof cr;++i)vdp_transport_feed(&q,cr[i]);
  for(size_t i=0;i<sizeof host3;++i)vdp_transport_feed(&q,host3[i]);assert(vdp_transport_take_interrupt(&q));}
 puts("VDP console, keyboard, metadata and bounded packet parsing passed");return 0;
}

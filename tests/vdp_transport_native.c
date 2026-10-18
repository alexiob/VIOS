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
 /* Cooked mode: a physical line waits for the host line boundary, is edited
  * locally and lands whole; raw mode (application KEY) passes bytes through. */
 vdp_transport_init(&p);vdp_transport_line_mode(&p,1);
 const unsigned char host_open[]={0xFE,3,'@','1','2'};
 for(size_t i=0;i<sizeof host_open;++i)vdp_transport_feed(&p,host_open[i]);
 const char typed[]="AB\bC\r";
 for(size_t i=0;typed[i];++i){const unsigned char k[]={0x81,4,(unsigned char)typed[i],0,0,1};for(size_t j=0;j<sizeof k;++j)vdp_transport_feed(&p,k[j]);}
 const unsigned char x[]={0x81,4,'X',0,0,1};for(size_t j=0;j<sizeof x;++j)vdp_transport_feed(&p,x[j]);
 assert(p.local_dropped==1&&"keys after an undelivered Enter are counted, not merged");
 const unsigned char host_end[]={0xFE,2,'3','\r'};
 for(size_t i=0;i<sizeof host_end;++i)vdp_transport_feed(&p,host_end[i]);
 const char expect[]="@123\rAC\r";
 for(size_t i=0;expect[i];++i)assert(vdp_transport_getc(&p)==expect[i]);
 assert(vdp_transport_getc(&p)==-1);
 const char cancel[]="ZZ\x1b" "Q\r";
 for(size_t i=0;cancel[i];++i){const unsigned char k[]={0x81,4,(unsigned char)cancel[i],0,0,1};for(size_t j=0;j<sizeof k;++j)vdp_transport_feed(&p,k[j]);}
 assert(vdp_transport_getc(&p)=='Q'&&vdp_transport_getc(&p)=='\r'&&vdp_transport_getc(&p)==-1);
 /* A full ring defers the line until the reader drains it. */
 for(unsigned i=0;i<2;++i){vdp_transport_feed(&p,0xFE);vdp_transport_feed(&p,255);for(unsigned j=0;j<254;++j)vdp_transport_feed(&p,'h');vdp_transport_feed(&p,'\r');}
 const unsigned char w[]={0x81,4,'W',0,0,1,0x81,4,'\r',0,0,1};for(size_t j=0;j<sizeof w;++j)vdp_transport_feed(&p,w[j]);
 for(unsigned i=0;i<509;++i)assert(vdp_transport_getc(&p)>0);
 assert(vdp_transport_getc(&p)=='\r'&&vdp_transport_getc(&p)=='W'&&vdp_transport_getc(&p)=='\r'&&vdp_transport_getc(&p)==-1);
 /* Line editing: insert at cursor, Left/Home/End, Delete, Backspace, history. */
 {
  vdp_transport_init(&p);vdp_transport_line_mode(&p,1);
  #define KEY(a,vk) do{const unsigned char k_[]={0x81,4,(unsigned char)(a),0,(unsigned char)(vk),1};for(size_t j_=0;j_<sizeof k_;++j_)vdp_transport_feed(&p,k_[j_]);}while(0)
  KEY('a',0);KEY('c',0);KEY(0,PLATFORM_KEY_LEFT);KEY('b',0);            /* abc, cursor 2 */
  assert(p.local_used==3&&!memcmp(p.local,"abc",3)&&p.local_cursor==2);
  KEY(0,PLATFORM_KEY_HOME);KEY(0,PLATFORM_KEY_DELETE);                   /* bc, cursor 0 */
  KEY(0,PLATFORM_KEY_END);KEY(8,PLATFORM_KEY_BACKSPACE);KEY('x',0);       /* bx */
  assert(p.local_used==2&&!memcmp(p.local,"bx",2)&&p.local_cursor==2);
  KEY(0,PLATFORM_KEY_LEFT);KEY(0,PLATFORM_KEY_LEFT);KEY(0,PLATFORM_KEY_LEFT);KEY(8,PLATFORM_KEY_BACKSPACE);
  assert(p.local_cursor==0&&p.local_used==2&&"cursor clamps; backspace at 0 is a no-op");
  KEY(13,PLATFORM_KEY_RETURN);assert(vdp_transport_getc(&p)=='b'&&vdp_transport_getc(&p)=='x'&&vdp_transport_getc(&p)=='\r');
  assert(p.local_erase==1&&p.local_used==0);
  KEY('q',0);KEY(13,PLATFORM_KEY_RETURN);while(vdp_transport_getc(&p)>=0){}
  KEY(0,PLATFORM_KEY_UP);assert(p.local_used==1&&p.local[0]=='q'&&p.local_cursor==1);
  KEY(0,PLATFORM_KEY_UP);assert(p.local_used==2&&!memcmp(p.local,"bx",2));
  KEY(0,PLATFORM_KEY_UP);assert(p.local_used==2&&"oldest entry holds");
  KEY(0,PLATFORM_KEY_DOWN);KEY(0,PLATFORM_KEY_DOWN);assert(p.local_used==0&&"down past newest is an empty line");
  KEY(0,PLATFORM_KEY_UP);KEY(13,PLATFORM_KEY_RETURN);while(vdp_transport_getc(&p)>=0){}
  assert(p.history_count==2&&"re-entering the newest line does not duplicate it");
  #undef KEY
 }
 vdp_transport_line_mode(&p,0);
 const unsigned char raw[]={0x81,4,'k',0,0,1};for(size_t j=0;j<sizeof raw;++j)vdp_transport_feed(&p,raw[j]);
 assert(vdp_transport_getc(&p)=='k'&&vdp_transport_getc(&p)==-1);
 puts("VDP console, keyboard, metadata and bounded packet parsing passed");return 0;
}

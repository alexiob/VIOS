#include <assert.h>
#include <stddef.h>
#include <stdio.h>
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
 puts("VDP console, keyboard, metadata and bounded packet parsing passed");return 0;
}

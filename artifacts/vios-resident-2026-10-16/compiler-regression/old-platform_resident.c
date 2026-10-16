/* Full VDP UART plus framed USB console; no generic dispatch in VM hot paths. */
#include "platform/console.h"
#include "platform/system.h"
#include "platform/vdp.h"
#include "platform/timer.h"
#include "drivers/vdp_transport.h"
#include "arch/ez80/uart0.h"
extern void ez80_uart0_raw_init(void);
static struct vdp_transport rx;
static unsigned char output[128];
static unsigned output_used;
static void poll(void);
static void send(unsigned char c) {
 /* Receive service never transmits, so outgoing VDU frames remain contiguous. */
 while(!(ez80_uart0_modem()&0x10))poll();
 while(!(ez80_uart0_status()&0x20))poll();
 ez80_uart0_write(c);
 poll();
}
static void poll(void) {
 unsigned limit=512;
 while(limit--&&(ez80_uart0_status()&1))vdp_transport_feed(&rx,ez80_uart0_read());
}
void platform_console_init(void) {ez80_uart0_raw_init();vdp_transport_init(&rx);output_used=0;}
void platform_console_flush(void) {
 if(!output_used)return;
 send(23);send(0);send(0xE0);send((unsigned char)output_used);
 for(unsigned i=0;i<output_used;++i)send(output[i]);
 output_used=0;
}
void platform_console_putc(unsigned char c) {
 output[output_used++]=c;
 if(output_used==sizeof output||c=='\n')platform_console_flush();
}
int platform_console_getc(void) {
 int c=vdp_transport_getc(&rx);if(c>=0)return c;
 poll();c=vdp_transport_getc(&rx);if(c>=0)return c;
 platform_console_flush();return -1;
}
void platform_vdp_send(const void *data,size_t length) {
 const unsigned char *p=data;platform_console_flush();
 while(length--) {send(*p++);poll();}
}
int platform_vdp_request(const void *command,size_t length,unsigned char reply,void *data,size_t capacity,unsigned *actual) {
 unsigned char stale[32];unsigned discarded=0;
 poll();(void)vdp_transport_packet(&rx,reply,stale,sizeof stale,&discarded);
 platform_vdp_send(command,length);
 unsigned short previous=platform_timer_read();uint32_t elapsed=0;
 do {
  poll();if(vdp_transport_packet(&rx,reply,data,capacity,actual))return 1;
  unsigned short now=platform_timer_read();elapsed+=(unsigned short)(previous-now);previous=now;
 }while(elapsed<PLATFORM_TIMER_HZ);
 if(actual)*actual=0;return 0;
}
uint32_t platform_millis(void) {
 static const unsigned char command[]={23,0,0xE1};unsigned char data[4];unsigned n;
 if(!platform_vdp_request(command,sizeof command,0xFD,data,sizeof data,&n)||n!=4)return UINT32_MAX;
 return (uint32_t)data[0]|((uint32_t)data[1]<<8)|((uint32_t)data[2]<<16)|((uint32_t)data[3]<<24);
}
unsigned platform_console_dropped(void) {return rx.dropped;}
void platform_boot_ready(void) {static const unsigned char command[]={23,0,0xE2,1};platform_vdp_send(command,sizeof command);}
void platform_boot_trial(void) {static const unsigned char command[]={23,0,0xE2,0};platform_vdp_send(command,sizeof command);}
const char *platform_identity(void) {return "VIOS / Agon Light 2 / native24 / VDP+USB multiplex / SD SPI";}

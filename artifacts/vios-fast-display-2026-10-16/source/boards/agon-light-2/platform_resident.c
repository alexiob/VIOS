/* Full VDP UART plus framed USB console; no generic dispatch in VM hot paths. */
#include "platform/console.h"
#include "platform/system.h"
#include "platform/vdp.h"
#include "platform/timer.h"
#include "platform/input.h"
#include "drivers/vdp_transport.h"
#include "arch/ez80/uart0.h"
extern void ez80_uart0_raw_init(void);
static struct vdp_transport rx;
static unsigned char output[128];
static unsigned output_used;
static unsigned char incoming[32];
static void poll(void);
static void send_bytes(const unsigned char *data,size_t length) {
 if(!length)return;
 /* Stop incoming traffic before native TX work, including an idle RX window.
  * Collection never transmits, so outgoing VDU frames remain contiguous. */
 while(length){
  poll();
  unsigned count=ez80_uart0_tx_burst(data,length>32?32:(unsigned)length);
  data+=count;length-=count;
  /* A stalled peer may need to send before asserting CTS; service that window
   * on every retry, leaving RX paused again before returning to the caller. */
  if(!count)ez80_uart0_rx_resume();
 }
 poll();
}
static void poll(void) {
 /* Native collection precedes parsing; the peer obeys RTS while C runs. */
 unsigned count=ez80_uart0_rx_drain(incoming);
 for(unsigned i=0;i<count;++i)vdp_transport_feed(&rx,incoming[i]);
}
void platform_console_init(void) {ez80_uart0_raw_init();ez80_uart0_rx_pause();ez80_uart0_rx_overruns=0;vdp_transport_init(&rx);output_used=0;}
void platform_console_flush(void) {
 if(!output_used)return;
 unsigned char header[]={23,0,0xE0,(unsigned char)output_used};
 send_bytes(header,sizeof header);send_bytes(output,output_used);
 output_used=0;
}
void platform_console_putc(unsigned char c) {
 output[output_used++]=c;
 if(output_used==sizeof output||c=='\n')platform_console_flush();
}
int platform_console_getc(void) {
 ez80_uart0_rx_pause();
 int c=vdp_transport_getc(&rx);if(c>=0)return c;
 poll();c=vdp_transport_getc(&rx);if(c>=0)return c;
 platform_console_flush();ez80_uart0_rx_resume();return -1;
}
void platform_input_resume(void) {ez80_uart0_rx_resume();}
void platform_keyboard_route(int enabled) {
 ez80_uart0_rx_pause();vdp_transport_keyboard_route(&rx,enabled);poll();
}
int platform_keyboard_event(struct platform_key_event *out) {
 ez80_uart0_rx_pause();
 if(!out)return 0;
 if(vdp_transport_keyboard_event(&rx,out))return 1;
 poll();if(vdp_transport_keyboard_event(&rx,out))return 1;
 return 0;
}
unsigned platform_keyboard_dropped(void) {return rx.key_dropped;}
void platform_vdp_send(const void *data,size_t length) {
 platform_console_flush();send_bytes(data,length);
}
int platform_vdp_request(const void *command,size_t length,unsigned char reply,void *data,size_t capacity,unsigned *actual) {
 unsigned char stale[32];unsigned discarded=0;
 poll();(void)vdp_transport_packet(&rx,reply,stale,sizeof stale,&discarded);
 platform_vdp_send(command,length);
 unsigned short previous=platform_timer_read();uint32_t elapsed=0;
 do {
  poll();if(vdp_transport_packet(&rx,reply,data,capacity,actual))return 1;
  unsigned short now=platform_timer_read();elapsed+=(unsigned short)(previous-now);previous=now;
  if(elapsed<PLATFORM_TIMER_HZ)ez80_uart0_rx_resume();
 }while(elapsed<PLATFORM_TIMER_HZ);
 if(actual)*actual=0;return 0;
}
uint32_t platform_millis(void) {
 static const unsigned char command[]={23,0,0xE1};unsigned char data[4];unsigned n;
 if(!platform_vdp_request(command,sizeof command,0xFD,data,sizeof data,&n)||n!=4)return UINT32_MAX;
 return (uint32_t)data[0]|((uint32_t)data[1]<<8)|((uint32_t)data[2]<<16)|((uint32_t)data[3]<<24);
}
unsigned platform_console_dropped(void) {return rx.dropped+ez80_uart0_rx_overruns;}
void platform_boot_ready(void) {static const unsigned char command[]={23,0,0xE2,1};platform_vdp_send(command,sizeof command);}
void platform_boot_trial(void) {static const unsigned char command[]={23,0,0xE2,0};platform_vdp_send(command,sizeof command);}
const char *platform_identity(void) {return "VIOS / Agon Light 2 / native24 / VDP+USB multiplex / SD SPI";}

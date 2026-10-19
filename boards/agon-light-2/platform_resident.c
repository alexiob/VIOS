/* Full VDP UART plus framed USB console; no generic dispatch in VM hot paths. */
#include "platform/console.h"
#include "platform/system.h"
#include "platform/vdp.h"
#include "platform/timer.h"
#include "platform/input.h"
#include "drivers/vdp_transport.h"
#include "vios/wire.h"
#include "arch/ez80/uart0.h"
extern void ez80_uart0_raw_init(void);
static struct vdp_transport rx;
static unsigned char output[128];
static unsigned output_used;
static unsigned char incoming[32];
static void poll(void);
static void esp_restart_check(void);
/* Console output tap for the VGA console view: RAM only, never VDP I/O. */
static void (*console_tap)(unsigned char);
static unsigned char shown[sizeof rx.local],shown_length,shown_cursor;
static int in_echo,foreign_output;
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
/* Console echo coalescing: while input keeps arriving (a host line streams
 * in), the per-character REPL echo is held and leaves as one frame when the
 * line ends or after ECHO_IDLE_POLLS empty polls. Fewer USB frames lower the
 * exposure to the rare ESP/CH340 byte loss seen in the 2026-10-19 soak. The
 * rule counts polls, not timer ticks, so output never waits on the timer. */
#define ECHO_IDLE_POLLS 16u
static unsigned idle_polls;
static void poll(void) {
 /* Native collection precedes parsing; the peer obeys RTS while C runs. */
 unsigned count=ez80_uart0_rx_drain(incoming);
 if(count)idle_polls=0;
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
 if(console_tap)console_tap(c);
 if(!in_echo&&(shown_length||rx.local_erase))foreign_output=1;
 output[output_used++]=c;
 if(output_used==sizeof output||c=='\n')platform_console_flush();
}
/* Show the cooked local line as it is edited, using only printable bytes,
 * spaces and BS so any terminal (USB or the VGA console view) renders it.
 * Runs outside poll(). Only the changed tail is redrawn and flushed at once. A handed-over line
 * is erased before the REPL echoes its first byte, so it is shown once.
 * Host bytes may interleave with this echo; only display is affected. */
static void back(unsigned n){while(n--)platform_console_putc(8);}
static void echo_local(void) {
 in_echo=1;
 /* Other output moved the terminal cursor: redisplay the line on a fresh
  * row (like readline) instead of editing in place at the wrong column. */
 if(foreign_output){foreign_output=0;if(shown_length||rx.local_erase){if(!rx.local_erase){platform_console_putc('\r');platform_console_putc('\n');}shown_length=shown_cursor=0;rx.local_erase=0;}}
 if(rx.local_erase){
  rx.local_erase=0;back(shown_cursor);for(unsigned i=0;i<shown_length;++i)platform_console_putc(' ');back(shown_length);
  shown_length=shown_cursor=0;
 }
 unsigned length=rx.local_used,cursor=rx.local_cursor,i=0;
 while(i<length&&i<shown_length&&shown[i]==rx.local[i])++i;
 if(i==length&&length==shown_length&&cursor==shown_cursor){in_echo=0;return;}
 if(shown_cursor>i)back(shown_cursor-i);else for(unsigned k=shown_cursor;k<i;++k)platform_console_putc(shown[k]);
 for(unsigned k=i;k<length;++k)platform_console_putc(rx.local[k]);
 unsigned end=length;for(;end<shown_length;++end)platform_console_putc(' ');
 back(end-cursor);
 for(unsigned k=0;k<length;++k)shown[k]=rx.local[k];shown_length=(unsigned char)length;shown_cursor=(unsigned char)cursor;in_echo=0;
 /* The ESP draws the VGA console from these bytes (GUI Stage 1c): send a
  * physical keystroke's echo now. Coalescing still applies to host lines,
  * which never pass through this cooked-line editor. */
 platform_console_flush();
}
int platform_console_getc(void) {
 ez80_uart0_rx_pause();
 int c=vdp_transport_getc(&rx);if(c>=0){echo_local();return c;}
 poll();esp_restart_check();c=vdp_transport_getc(&rx);echo_local();if(c>=0)return c;
 if(idle_polls<ECHO_IDLE_POLLS)++idle_polls;else platform_console_flush();
 ez80_uart0_rx_resume();return -1;
}
void platform_input_resume(void) {ez80_uart0_rx_resume();}
/* Stage 2a: the ESP edits physical lines while the REPL awaits one and keys
 * are not routed to an application; tell it whenever that changes. Pending
 * console output goes first so its echo stays in order. */
static unsigned char esp_line_flags=0xFF;
static void esp_line_update(void){
 unsigned char flags=(unsigned char)((rx.line_mode?1:0)|(rx.key_route?2:0));
 if(flags==esp_line_flags)return;
 esp_line_flags=flags;platform_console_flush();
 const unsigned char command[]={23,0,VIOS_VDU_LINE_MODE,flags};send_bytes(command,sizeof command);
}
/* U5: the ESP restarted while VIOS kept running (user mode; it cannot reload
 * the CPU) and starts with its defaults: resend the line flags, count an
 * epoch for display owners (platform_vdp_epoch). Runs outside poll(). */
static unsigned esp_epoch;
static void esp_restart_check(void){
 unsigned char reason[4];unsigned n;
 if(!vdp_transport_packet(&rx,VIOS_PACKET_ESP_RESTART,reason,sizeof reason,&n))return;
 ++esp_epoch;esp_line_flags=0xFF;esp_line_update();
}
unsigned platform_vdp_epoch(void){esp_restart_check();return esp_epoch;}
void platform_keyboard_route(int enabled) {
 ez80_uart0_rx_pause();vdp_transport_keyboard_route(&rx,enabled);poll();esp_line_update();
}
int platform_keyboard_event(struct platform_key_event *out) {
 ez80_uart0_rx_pause();
 if(!out)return 0;
 if(vdp_transport_keyboard_event(&rx,out))return 1;
 poll();esp_restart_check();if(vdp_transport_keyboard_event(&rx,out))return 1;
 return 0;
}
unsigned platform_keyboard_dropped(void) {return rx.key_dropped;}
int platform_keyboard_hotkey(void) {ez80_uart0_rx_pause();poll();int k=rx.hotkey;rx.hotkey=0;return k;}
void platform_console_tap(void (*tap)(unsigned char)) {console_tap=tap;}
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
/* One host-service exchange (VDU 23,0,E3 -> USB channel 4 -> packet FC).
 * Returns 0 after ~1 s without a reply (no harness, or it is not serving). */
int platform_host_request(const void *request,size_t length,void *reply,size_t capacity,unsigned *actual) {
 if(length>240)return 0;
 unsigned char discard[240];unsigned stale=0;poll();(void)vdp_transport_host_reply(&rx,discard,sizeof discard,&stale);
 unsigned char header[4]={23,0,0xE3,(unsigned char)length};
 platform_console_flush();send_bytes(header,sizeof header);send_bytes(request,length);
 unsigned short previous=platform_timer_read();uint32_t elapsed=0;
 do {
  poll();if(vdp_transport_host_reply(&rx,reply,(unsigned)capacity,actual))return 1;
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
unsigned platform_console_dropped(void) {return rx.dropped+rx.local_dropped+ez80_uart0_rx_overruns;}
void platform_console_line_mode(int awaiting_line) {vdp_transport_line_mode(&rx,awaiting_line);esp_line_update();}
/* Background-task admission: listen ~1 ms with RTS asserted; any received
 * byte or key event defers tasks to the console. Otherwise pause RX so the
 * peer holds new input for the whole slice (the FIFO is not drained then). */
int platform_console_quiet(void) {
 unsigned short start=platform_timer_read();
 /* Each drain pauses RX again, so reopen the window on every iteration. */
 while((unsigned short)(start-platform_timer_read())<PLATFORM_TIMER_HZ/1000u){
  ez80_uart0_rx_resume();poll();if(rx.head!=rx.tail||rx.key_count){ez80_uart0_rx_pause();return 0;}
 }
 ez80_uart0_rx_pause();poll();return rx.head==rx.tail&&!rx.key_count;
}
void platform_boot_ready(void) {static const unsigned char command[]={23,0,0xE2,1};platform_vdp_send(command,sizeof command);}
void platform_boot_trial(void) {static const unsigned char command[]={23,0,0xE2,0};platform_vdp_send(command,sizeof command);}
const char *platform_identity(void) {return "VIOS / Agon Light 2 / native24 / VDP+USB multiplex / SD SPI";}

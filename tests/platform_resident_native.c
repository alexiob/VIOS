/* Actual production board C with a 16-byte receive FIFO and byte-level peer.
 * No serial device is opened. UART progress is stepped by register reads.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "platform/console.h"
#include "platform/vdp.h"
#include "platform/input.h"
#include "drivers/vdp_transport.h"
#include "arch/ez80/uart0.h"
static unsigned char incoming[260],fifo[16],transmitted[512],source[256];
static unsigned next_wire,head,count,overruns,reads,writes,register_reads,timer_reads,wire_length=260;
static int stall_cts,stall_tx;
static int rts;
static unsigned burst_calls,drain_calls;
volatile unsigned ez80_uart0_rx_overruns;
static void arrive(void) {
 if(next_wire<wire_length){
  unsigned char byte=incoming[next_wire++];
  if(count==sizeof fifo)++overruns;
  else{fifo[(head+count)&15]=byte;++count;}
 }
}
static void wire_tick(void) {if(rts)arrive();}
void ez80_uart0_raw_init(void) {}
void ez80_uart0_rx_pause(void) {
 if(rts)arrive(); /* At most one already transmitted character follows CTS. */
 rts=0;
}
void ez80_uart0_rx_resume(void) {rts=1;}
unsigned char ez80_uart0_modem(void) {
 assert(++register_reads<20000);wire_tick(); /* Deadlock fails, not hangs. */
 return stall_cts&&reads<64?0:0x10;
}
unsigned char ez80_uart0_status(void) {
 assert(++register_reads<20000);wire_tick();
 return (unsigned char)((count?1:0)|((!stall_tx||reads>=64)?0x20:0));
}
unsigned ez80_uart0_rx_drain(unsigned char *buffer) {
 ++drain_calls;ez80_uart0_rx_pause();unsigned n=0;
 while(n<32&&(ez80_uart0_status()&1)){buffer[n]=ez80_uart0_read();++n;}
 return n;
}
void actual_feed(struct vdp_transport *,unsigned char);
void vdp_transport_feed(struct vdp_transport *transport,unsigned char byte) {
 assert(!rts); /* A slow parser must never run with peer transmission enabled. */
 for(unsigned i=0;i<100;++i)wire_tick();
 actual_feed(transport,byte);
}
unsigned char ez80_uart0_read(void) {
 assert(count);unsigned char byte=fifo[head];head=(head+1)&15;--count;++reads;return byte;
}
void ez80_uart0_write(unsigned char value) {
 assert(writes<sizeof transmitted);transmitted[writes++]=value;
}
unsigned ez80_uart0_tx_burst(const unsigned char *buffer,unsigned length) {
 ++burst_calls;unsigned n=0;
 while(n<length&&n<32){
  if(!(ez80_uart0_modem()&0x10)||!(ez80_uart0_status()&0x20))break;
  ez80_uart0_write(buffer[n++]);
 }
 return n;
}
static unsigned timer_step=1000;
unsigned short platform_timer_read(void) {++timer_reads;return (unsigned short)(0-timer_reads*timer_step);}
int main(int argc,char **argv) {
 assert(argc==2);stall_cts=!strcmp(argv[1],"cts");stall_tx=!strcmp(argv[1],"tx");
 if(!strcmp(argv[1],"echo")){
  /* Cooked keys echo as typed, Backspace erases, and the handed-over line is
   * erased before the REPL sees it so its own echo is the only copy. */
  timer_step=10;const unsigned char keys[]={0x81,4,'a',0,0,1,0x81,4,'b',0,0,1,0x81,4,8,0,132,1,0x81,4,'c',0,0,1,0x81,4,13,0,143,1};
  memcpy(incoming,keys,sizeof keys);wire_length=sizeof keys;next_wire=0;platform_console_init();platform_console_line_mode(1);
  char got[8];unsigned n=0;for(unsigned i=0;i<400&&n<3;++i){int c=platform_console_getc();if(c>=0)got[n++]=(char)c;}
  assert(n==3&&got[0]=='a'&&got[1]=='c'&&got[2]=='\r');
  platform_console_flush(); /* the REPL's own echo would follow in this buffer */
  char shown[64];unsigned m=0;
  for(unsigned i=0;i+3<writes;){assert(transmitted[i]==23&&transmitted[i+1]==0&&transmitted[i+2]==0xE0);unsigned len=transmitted[i+3];for(unsigned k=0;k<len;++k)shown[m++]=(char)transmitted[i+4+k];i+=4+len;}
  /* Render as a terminal: BS moves left, other bytes overwrite. Echo timing
   * depends on polling, so check what was visible, not the exact bytes. */
  char screen[16]={0};unsigned col=0,best=0;char seen[16]={0};
  for(unsigned i=0;i<m;++i){if(shown[i]==8){if(col)--col;}else{screen[col++]=shown[i];}
   unsigned len=0;while(len<sizeof screen&&screen[len]&&screen[len]!=' ')++len;if(len>=best){best=len;memcpy(seen,screen,len);seen[len]=0;}}
  for(unsigned i=0;i<sizeof screen;++i)assert(!screen[i]||screen[i]==' ');
  assert(!strcmp(seen,"ac")||!strcmp(seen,"ab"));assert(strchr(shown,'c'));
  puts("resident cooked line echoes once");return 0;
 }
 if(!strcmp(argv[1],"edit-echo")){
  /* Mid-line insert and delete redraw only the tail; a terminal rendering of
   * the echo always equals the edited line with the cursor in place. */
  timer_step=10;const unsigned char keys[]={0x81,4,'a',0,0,1,0x81,4,'b',0,0,1,0x81,4,'c',0,0,1,
   0x81,4,0,0,154,1,0x81,4,0,0,154,1,0x81,4,'X',0,0,1,0x81,4,0,0,130,1};
  memcpy(incoming,keys,sizeof keys);wire_length=sizeof keys;next_wire=0;platform_console_init();platform_console_line_mode(1);
  for(unsigned i=0;i<400;++i)(void)platform_console_getc();
  platform_console_flush();
  char screen[32];memset(screen,' ',sizeof screen);unsigned col=0;
  for(unsigned i=0;i+3<writes;){unsigned len=transmitted[i+3];for(unsigned k=0;k<len;++k){unsigned char c=transmitted[i+4+k];if(c==8){assert(col);--col;}else screen[col++]=(char)c;}i+=4+len;}
  assert(!memcmp(screen,"aXc ",4)&&col==2);
  puts("resident echo follows mid-line edits");return 0;
 }
 if(!strcmp(argv[1],"foreign")){
  /* Output arriving while a local line is shown redisplays it on a new row. */
  timer_step=10;const unsigned char keys[]={0x81,4,'a',0,0,1,0x81,4,'b',0,0,1};
  memcpy(incoming,keys,sizeof keys);wire_length=sizeof keys;next_wire=0;platform_console_init();platform_console_line_mode(1);
  for(unsigned i=0;i<200;++i)(void)platform_console_getc();
  const char *tick="tick\r\n";for(const char *q=tick;*q;++q)platform_console_putc((unsigned char)*q);
  for(unsigned i=0;i<20;++i)(void)platform_console_getc();platform_console_flush();
  char shown[128];unsigned m=0;
  for(unsigned i=0;i+3<writes;){unsigned len=transmitted[i+3];for(unsigned k=0;k<len;++k)shown[m++]=(char)transmitted[i+4+k];i+=4+len;}
  shown[m]=0;assert(strstr(shown,"abtick\r\n\r\nab"));
  puts("resident echo redisplays after foreign output");return 0;
 }
 if(!strcmp(argv[1],"gate")){
  /* Quiet line: tasks admitted with RX paused. A host byte inside the ~1 ms
   * window defers tasks and is received, never left to overrun the FIFO. */
  timer_step=10;wire_length=0;platform_console_init();
  assert(platform_console_quiet()==1&&!rts);
  const unsigned char line[]={0xFE,3,'A','B','\r'};memcpy(incoming,line,sizeof line);wire_length=sizeof line;next_wire=0;
  assert(platform_console_quiet()==0&&!rts);
  unsigned got=0;for(unsigned i=0;i<200&&got<3;++i){int c=platform_console_getc();if(c>=0)assert(c==(int)line[2+got++]);}
  assert(got==3&&overruns==0);
  puts("resident task gate admits only quiet input");return 0;
 }
 if(!strcmp(argv[1],"vdp-burst")){
  wire_length=0;platform_console_init();platform_input_resume();
  unsigned char data[256];for(unsigned i=0;i<sizeof data;++i)data[i]=(unsigned char)i;
  platform_vdp_send(data,sizeof data);
  assert(writes==256&&!memcmp(transmitted,data,256));
  assert(burst_calls==8&&drain_calls<=9&&!rts&&overruns==0);
  puts("resident VDP transfer batches RX collection between bounded native bursts");return 0;
 }
 if(!strcmp(argv[1],"keyboard-work")){
  /* The host can send its next tagged line during a long C compose/scan.
   * RX must remain paused until the idle owner explicitly yields. */
  incoming[0]=0xfe;incoming[1]=80;for(unsigned i=0;i<80;++i)incoming[i+2]=(unsigned char)('A'+i%26);wire_length=82;
  platform_console_init();platform_keyboard_route(1);assert(!rts);
  struct platform_key_event event;
  assert(!platform_keyboard_event(&event)&&!rts);
  for(unsigned i=0;i<1000;++i)wire_tick();
  assert(!overruns&&next_wire==0);
  /* Even a null consumer obeys the paused-on-return contract. */
  platform_input_resume();assert(rts);assert(!platform_keyboard_event(NULL)&&!rts);
  for(unsigned i=0;i<1000;++i)wire_tick();assert(!overruns);
  platform_keyboard_route(0);assert(!rts);platform_keyboard_route(1);assert(!rts);
  for(unsigned n=0;n<80;++n){
   int c;unsigned attempts=0;
   do{
    c=platform_console_getc();
    if(c<0){
     assert(!platform_keyboard_event(&event)&&!rts);
     for(unsigned i=0;i<1000;++i)wire_tick();
     assert(!overruns);platform_input_resume();assert(++attempts<1000);
    }
   }while(c<0);
   assert(c=='A'+(int)(n%26)&&!rts);
  }
  assert(!overruns&&reads==82&&platform_console_dropped()==0);
  puts("resident keyboard idle work keeps RX paused until explicit yield; host line survives");return 0;
 }
 if(!strcmp(argv[1],"keyboard")){
  const unsigned char data[]={0x81,4,'K',PLATFORM_MOD_SHIFT,PLATFORM_KEY_F5,1,0xfe,1,'H',0x81,4,'K',PLATFORM_MOD_SHIFT,PLATFORM_KEY_F5,0};
  wire_length=sizeof data;memcpy(incoming,data,sizeof data);platform_console_init();platform_keyboard_route(1);assert(!rts);
  for(unsigned n=0;n<2;++n){
   struct platform_key_event event;unsigned attempts=0;
   while(!platform_keyboard_event(&event)){assert(!rts);assert(++attempts<1000);platform_input_resume();assert(rts);}
   assert(!rts&&event.ascii=='K'&&event.virtual_key==PLATFORM_KEY_F5&&event.modifiers==PLATFORM_MOD_SHIFT&&event.down==(n==0));
  }
  assert(platform_console_getc()=='H');assert(platform_keyboard_dropped()==0&&overruns==0);
  platform_keyboard_route(0);next_wire=0;wire_length=6;incoming[2]='Z';
  int c;unsigned attempts=0;do{c=platform_console_getc();assert(++attempts<1000);}while(c<0);
  assert(c=='Z'&&!rts);assert(platform_console_getc()==-1);
  puts("resident keyboard polling isolates FE console and restores legacy route");return 0;
 }
 if(!strcmp(argv[1],"request")||!strcmp(argv[1],"timeout")){
  const unsigned char command[]={23,0,0xe1};unsigned char reply[4];unsigned actual=99;
  int want_reply=!strcmp(argv[1],"request");wire_length=want_reply?6:0;
  incoming[0]=0xfd;incoming[1]=4;memcpy(incoming+2,"\x01\x80\x00\xff",4);
  platform_console_init();assert(!rts);
  assert(platform_vdp_request(command,sizeof command,0xfd,reply,sizeof reply,&actual)==want_reply);
  assert(!rts&&writes==3&&!memcmp(transmitted,command,3)&&overruns==0);
  assert(actual==(want_reply?4:0));if(want_reply)assert(!memcmp(reply,incoming+2,4));
  puts("resident request opens receive window and leaves RTS paused on success/timeout");return 0;
 }
 for(unsigned i=0;i<256;++i)source[i]=(unsigned char)i;source[255]='\r';
 incoming[0]=0xfe;incoming[1]=240;memcpy(incoming+2,source,240);
 incoming[242]=0xfe;incoming[243]=16;memcpy(incoming+244,source+240,16);
 platform_console_init();
 /* The 128th buffered echo byte forces one atomic 132-byte VDU E0 frame. */
 for(unsigned i=0;i<128;++i)platform_console_putc((unsigned char)('A'+i%26));
 assert(writes==132&&transmitted[0]==23&&transmitted[1]==0&&transmitted[2]==0xe0&&transmitted[3]==128);
 for(unsigned i=0;i<128;++i)assert(transmitted[i+4]==(unsigned char)('A'+i%26));
 for(unsigned i=0;i<256;++i){
  int value;unsigned attempts=0;
  do{value=platform_console_getc();assert(++attempts<1000);}while(value<0);
  if(value!=source[i])fprintf(stderr,"RX mismatch index %u: expected %u got %d, FIFO overruns %u\n",i,source[i],value,overruns);
  assert(value==source[i]);assert(!rts);
 }
 assert(platform_console_getc()==-1&&overruns==0&&reads==260);
 assert(timer_reads==0); /* TX progress does not depend on timer initialization. */
 assert(rts);ez80_uart0_rx_overruns=3;assert(platform_console_dropped()==3);
 puts("resident console flush preserves all 256 input bytes and frame atomicity");return 0;
}

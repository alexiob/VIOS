/* Only explicit device operations pay native service-dispatch overhead. */
#include "vios/runtime.h"
#include "platform/console.h"
#include "platform/system.h"
#include "platform/storage.h"
#include "platform/vdp.h"
#include "platform/timer.h"
static unsigned char file_data[4096];
static int path_from(fcell address,fcell length,char *out){
 if(!length||length>127)return forth_error("path length outside 1..127");
 for(fcell i=0;i<length;++i){unsigned char c=fvm_read8(address+i);if(fvm_error||c<33||c>126)return forth_error("invalid path");out[i]=(char)c;}
 out[length]=0;return 1;
}
static int console_write(void){fcell n=fvm_pop(),a=fvm_pop();if(fvm_error)return 0;while(n--){unsigned char c=fvm_read8(a++);if(fvm_error)return 0;forth_emit(c);}return 1;}
static int ticks(void){return fvm_push(platform_timer_read());}
/* Task clock from the down-counting 288 kHz timer, without a VDP round trip.
 * Sampled at scheduling points; a gap above one ~227 ms wrap loses time. */
static uint32_t clock_ms;static unsigned clock_ticks;static unsigned short clock_last;static int clock_started;
static uint32_t task_clock(void){
 unsigned short now=platform_timer_read();if(!clock_started){clock_started=1;clock_last=now;}
 clock_ticks+=(unsigned short)(clock_last-now);clock_last=now;clock_ms+=clock_ticks/288u;clock_ticks%=288u;return clock_ms;
}
static int milliseconds(void){uint32_t n=platform_millis();if(n==UINT32_MAX)return forth_error("VDP clock timeout");return fvm_push((fcell)n&FVM_MASK);}
static int milliseconds2(void){uint32_t n=platform_millis();if(n==UINT32_MAX)return forth_error("VDP clock timeout");return fvm_push((fcell)n&FVM_MASK)&&fvm_push((fcell)(n>>24));}
static int mount_card(void){return fvm_push(storage_init()?0:1);}
static int file_error(void){vios_out(storage_error());vios_out("\r\n");return 1;}
static int file_read(void){
 fcell capacity=fvm_pop(),dest=fvm_pop(),length=fvm_pop(),address=fvm_pop();char path[128];size_t n=0;
 if(fvm_error||!path_from(address,length,path))return 0;
 if(capacity>sizeof file_data)capacity=sizeof file_data;
 int ok=storage_read_file(path,file_data,(size_t)capacity,&n);
 if(ok)for(size_t i=0;i<n;++i){fvm_write8(dest+(fcell)i,file_data[i]);if(fvm_error)return 0;}
 return fvm_push((fcell)n)&&fvm_push(ok?0:1);
}
static int file_write(void){
 fcell n=fvm_pop(),source=fvm_pop(),length=fvm_pop(),address=fvm_pop();char path[128];
 if(fvm_error||!path_from(address,length,path))return 0;
 if(n>sizeof file_data)return fvm_push(2);
 for(fcell i=0;i<n;++i){file_data[i]=fvm_read8(source+i);if(fvm_error)return 0;}
 return fvm_push(storage_write_file(path,file_data,(size_t)n)?0:1);
}
static int file_remove(void){fcell n=fvm_pop(),a=fvm_pop();char path[128];if(fvm_error||!path_from(a,n,path))return 0;return fvm_push(storage_remove(path)?0:1);}
static int vdu(void){fcell c=fvm_pop();if(fvm_error)return 0;unsigned char b=(unsigned char)c;platform_vdp_send(&b,1);return 1;}
static int vtype(void){
 fcell n=fvm_pop(),a=fvm_pop();unsigned char b[64];if(fvm_error)return 0;
 while(n){unsigned count=n>sizeof b?sizeof b:(unsigned)n;for(unsigned i=0;i<count;++i){b[i]=fvm_read8(a++);if(fvm_error)return 0;}platform_vdp_send(b,count);n-=count;}return 1;
}
static int mode_info(unsigned char *data){static const unsigned char command[]={23,0,0x86};unsigned n;return platform_vdp_request(command,sizeof command,0x86,data,8,&n)&&n==8;}
static int info(void){
 unsigned char data[8];if(!mode_info(data))return forth_error("VDP mode timeout");
 vios_out("vdp width ");vios_uint((unsigned)data[0]+((unsigned)data[1]<<8));vios_out(" height ");vios_uint((unsigned)data[2]+((unsigned)data[3]<<8));
 vios_out(" colours ");vios_uint(data[6]);vios_out(" mode ");vios_uint(data[7]);vios_out(" console-dropped ");vios_uint(platform_console_dropped());vios_out("\r\n");return 1;
}
static int mode(void){fcell n=fvm_pop();if(fvm_error)return 0;unsigned char command[]={22,(unsigned char)n},data[8];platform_vdp_send(command,2);if(!mode_info(data)||data[7]!=(unsigned char)n)return forth_error("VDP mode rejected");return 1;}
static int clear_screen(void){unsigned char c=12;platform_vdp_send(&c,1);return 1;}
static int colour(void){fcell c=fvm_pop();if(fvm_error)return 0;unsigned char cmd[]={17,(unsigned char)c};platform_vdp_send(cmd,2);return 1;}
static int plot(void){fcell y=fvm_pop(),x=fvm_pop(),m=fvm_pop();if(fvm_error)return 0;unsigned char cmd[]={25,(unsigned char)m,(unsigned char)x,(unsigned char)(x>>8),(unsigned char)y,(unsigned char)(y>>8)};platform_vdp_send(cmd,sizeof cmd);return 1;}
static int screen_char(void){
 fcell y=fvm_pop(),x=fvm_pop();if(fvm_error)return 0;unsigned char cmd[]={23,0,0x83,(unsigned char)x,(unsigned char)(x>>8),(unsigned char)y,(unsigned char)(y>>8)},data;unsigned n;
 if(!platform_vdp_request(cmd,sizeof cmd,0x83,&data,1,&n)||n!=1)return forth_error("VDP character timeout");return fvm_push(data);
}
static int screen_pixel(void){
 fcell y=fvm_pop(),x=fvm_pop();if(fvm_error)return 0;unsigned char cmd[]={23,0,0x84,(unsigned char)x,(unsigned char)(x>>8),(unsigned char)y,(unsigned char)(y>>8)},data[4];unsigned n;
 if(!platform_vdp_request(cmd,sizeof cmd,0x84,data,sizeof data,&n)||n!=4)return forth_error("VDP pixel timeout");
 return fvm_push((fcell)data[0]|((fcell)data[1]<<8)|((fcell)data[2]<<16))&&fvm_push(data[3]);
}
static int beep(void){
 fcell duration=fvm_pop(),frequency=fvm_pop();if(fvm_error)return 0;
 if(duration>65535UL||frequency>65535UL)return forth_error("BEEP argument exceeds 16 bits");
 unsigned char cmd[]={23,0,0x85,0,0,64,(unsigned char)frequency,(unsigned char)(frequency>>8),(unsigned char)duration,(unsigned char)(duration>>8)},reply[2];unsigned n;
 if(!platform_vdp_request(cmd,sizeof cmd,0x85,reply,sizeof reply,&n)||n!=2)return forth_error("VDP audio timeout");return fvm_push(reply[1]);
}
static int reboot(void){platform_console_flush();platform_restart();return 1;}
int vios_services_init(void){
 forth_register_clock(task_clock);
 static const struct {const char *name;int (*fn)(void);} services[]={
 {"CONSOLE-WRITE",console_write},{"TIMER@",ticks},{"MILLIS",milliseconds},{"MILLIS2",milliseconds2},
 {"SD-MOUNT",mount_card},{"FS-ERROR",file_error},{"FILE-READ",file_read},{"FILE-WRITE",file_write},{"FILE-REMOVE",file_remove},
 {"VDU",vdu},{"VTYPE",vtype},{"VDP-INFO",info},{"MODE",mode},{"CLS",clear_screen},{"COLOUR",colour},{"PLOT",plot},{"CHAR@",screen_char},{"PIXEL@",screen_pixel},{"BEEP",beep},{"REBOOT",reboot}};
 /* Device services run to completion without re-entering the VM, so tasks
  * may call them; REBOOT stays a foreground decision. */
 for(unsigned i=0;i<sizeof services/sizeof services[0];++i)
  if(!forth_register_native(services[i].name,services[i].fn,vios_same(services[i].name,"REBOOT")?0:FORTH_NATIVE_TASK))return 0;
 return forth_eval("DEFER CONSOLE-EMIT ' EMIT IS CONSOLE-EMIT DEFER CONSOLE-KEY ' KEY IS CONSOLE-KEY DEFER CLOCK-MS ' MILLIS IS CLOCK-MS DEFER FS-READ ' FILE-READ IS FS-READ DEFER FS-WRITE ' FILE-WRITE IS FS-WRITE DEFER DISPLAY-WRITE ' VTYPE IS DISPLAY-WRITE");
}

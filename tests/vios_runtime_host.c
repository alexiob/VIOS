#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include "forth/modules.h"
#include "platform/storage.h"
#include "platform/vdp.h"
#include "vios/runtime.h"
#include "platform/input.h"
static int keyboard_route,have_key,input_paused;
static struct platform_key_event queued_key;
void platform_keyboard_route(int enabled){keyboard_route=enabled;input_paused=1;}
int platform_keyboard_event(struct platform_key_event *out){input_paused=1;if(!have_key)return 0;*out=queued_key;have_key=0;return 1;}
void platform_input_resume(void){input_paused=0;}
unsigned platform_keyboard_dropped(void){return 0;}
static char root[512],error[96];
static unsigned ready;
static unsigned fail_confirmation,tear_package;
static void path(char *out,const char *p){assert(p[0]=='/'&&!strstr(p,".."));assert(strlen(root)+strlen(p)<768);strcpy(out,root);strcat(out,p);}
void platform_console_putc(unsigned char c){putchar(c);}
int platform_console_getc(void){return -1;}
void platform_console_init(void){}
void platform_console_flush(void){}
void platform_restart(void){}
const char *platform_identity(void){return "VIOS host native-device boundary";}
/* Capture the application depth seen at the native device boundary. */
static unsigned timer_depth;
unsigned short platform_timer_read(void){static unsigned short ticks;timer_depth=(unsigned)fvm_depth;return --ticks;}
static unsigned vdp_max,vdp_calls,vdp_modes,vdp_fences;
static int vdp_fence_fail;
static unsigned char vdp_setup[64];static size_t vdp_setup_length;
void platform_vdp_send(const void *data,size_t size){const unsigned char *bytes=data;if(size>=2&&bytes[0]==22){vdp_setup_length=0;++vdp_modes;}if(vdp_setup_length+size<=sizeof vdp_setup){memcpy(vdp_setup+vdp_setup_length,data,size);vdp_setup_length+=size;}++vdp_calls;if(size>vdp_max)vdp_max=(unsigned)size;}
int platform_vdp_request(const void *command,size_t length,unsigned char reply,void *data,size_t capacity,unsigned *actual){
 (void)command;(void)length;unsigned char *p=data;
 if(reply==0x86&&capacity>=8){unsigned char mode[]={128,2,224,1,80,60,4,1};memcpy(p,mode,8);*actual=8;return 1;}
 if(reply==0x83&&capacity>=1){++vdp_fences;if(vdp_fence_fail)return 0;p[0]='V';*actual=1;return 1;}
 if(reply==0x84&&capacity>=4){p[0]=0x12;p[1]=0x34;p[2]=0x56;p[3]=2;*actual=4;return 1;}
 if(reply==0x85&&capacity>=2){p[0]=0;p[1]=1;*actual=2;return 1;}
 return 0;
}
uint32_t platform_millis(void){return 1234567;}
unsigned platform_console_dropped(void){return 0;}
void platform_boot_ready(void){++ready;}
void platform_boot_trial(void){}
int storage_init(void){return 1;}
const char *storage_error(void){return error;}
int storage_read_file(const char *p,void *buffer,size_t capacity,size_t *size){
 char full[768];path(full,p);*size=0;FILE *f=fopen(full,"rb");
 if(!f){strcpy(error,"file not found");return 0;}
 assert(!fseek(f,0,SEEK_END));long n=ftell(f);rewind(f);
 if(n<0||(size_t)n>capacity){fclose(f);strcpy(error,"file exceeds buffer");return 0;}
 *size=fread(buffer,1,(size_t)n,f);assert(!fclose(f));return *size==(size_t)n;
}
int storage_write_file(const char *p,const void *buffer,size_t size){if(fail_confirmation&&strstr(p,".REC")&&size==32&&((const unsigned char *)buffer)[8]==0){--fail_confirmation;strcpy(error,"injected confirmation failure");return 0;}char full[768];path(full,p);FILE *f=fopen(full,"wb");if(!f)return 0;size_t wanted=size;if(tear_package&&(strstr(p,".VIM")||strstr(p,".TMP"))){--tear_package;wanted=size/2;strcpy(error,"injected partial package write");}size_t n=fwrite(buffer,1,wanted,f);return !fclose(f)&&n==size;}
int storage_remove(const char *p){char full[768];path(full,p);return !remove(full);}
int storage_rename(const char *a,const char *b){char x[768],y[768];path(x,a);path(y,b);return !rename(x,y);}
int storage_mkdir(const char *p){char full[768];path(full,p);if(!mkdir(full,0700))return 1;struct stat st;return !stat(full,&st)&&S_ISDIR(st.st_mode);}
int storage_sync(void){return 1;}
static unsigned record_previous(void){unsigned char a[32],b[32];size_t an=0,bn=0;int av=storage_read_file("/VIOS/BOOT0.REC",a,32,&an)&&an==32,bv=storage_read_file("/VIOS/BOOT1.REC",b,32,&bn)&&bn==32;assert(av||bv);return !av?b[6]:!bv?a[6]:vios_get32(a+12)>vios_get32(b+12)?a[6]:b[6];}
static void reset(void){forth_init();assert(vios_runtime_init());}
static void eval(const char *s){if(!forth_eval(s)){fprintf(stderr,"eval failed: %s: %s\n",s,forth_last_error());abort();}}
int main(int argc,char **argv){
 assert(argc==2);assert(strlen(argv[1])<sizeof root);strcpy(root,argv[1]);reset();
 eval("MILLIS");assert(fvm_pop()==1234567);
 eval("S\" MixedCase\" CONSOLE-WRITE");assert(!fvm_depth);
 eval("VDP-INFO");eval("0 0 CHAR@");assert(fvm_pop()=='V');
 eval("0 0 PIXEL@");assert(fvm_pop()==2);assert(fvm_pop()==0x563412);
 assert(storage_mkdir("/VIOS"));
 const char *source=": VALUE 11 ; : TEST VALUE 11 = ;";
 assert(forth_module_stage("demo","1",NULL,0,source,"TEST"));assert(forth_module_activate("demo"));
 assert(vios_module_save("demo","/VIOS/DEMO.VIM"));
 const char *extra=": EXTRA 99 ; : EXTRA-TEST EXTRA 99 = ;";
 assert(forth_module_stage("extra","1",NULL,0,extra,"EXTRA-TEST"));assert(forth_module_activate("extra"));
 tear_package=1;assert(!vios_module_save("extra",NULL));assert(vios_module_save("extra",NULL));
 /* Immutable persistence refuses silently replacing a different package. */
 eval("VALUE");assert(fvm_pop()==11);
 eval("MODULES");eval("MODULE-SOURCE demo");
 assert(vios_boot_trial());reset();assert(vios_boot_startup());
 eval("VALUE");assert(fvm_pop()==11);assert(ready);
 assert(vios_boot_confirm());
 const char *second=": VALUE 22 ; : TEST VALUE 22 = ;";
 assert(forth_module_stage("demo","2",NULL,0,second,"TEST"));assert(forth_module_activate("demo"));
 assert(!vios_module_save("demo","/VIOS/DEMO.VIM"));
 assert(vios_boot_trial());fail_confirmation=1;reset();assert(vios_boot_startup());eval("VALUE");assert(fvm_pop()==11);
 assert(forth_module_stage("demo","2",NULL,0,second,"TEST"));assert(forth_module_activate("demo"));
 assert(vios_boot_trial());reset();assert(vios_boot_startup());eval("VALUE");assert(fvm_pop()==22);
 assert(vios_boot_revert());reset();assert(vios_boot_startup());eval("VALUE");assert(fvm_pop()==11);
 /* A corrupted candidate cannot replace the confirmed source bundle. */
 assert(vios_boot_trial());assert(record_previous()==255);char corrupt='!';assert(storage_write_file("/VIOS/BOOT_B.VIB",&corrupt,1));
 reset();assert(vios_boot_startup());eval("VALUE");assert(fvm_pop()==11);
 /* Existing source files load unchanged, including nested INCLUDE. */
 const char child[]=": CHILD 37 ;";const char parent[]="INCLUDE /VIOS/CHILD.FTH\n: PARENT CHILD 5 + ;";
 assert(storage_write_file("/VIOS/CHILD.FTH",child,sizeof child-1));assert(storage_write_file("/VIOS/PARENT.FTH",parent,sizeof parent-1));
 eval("LOAD /VIOS/PARENT.FTH PARENT");assert(fvm_pop()==42);
 puts("Runtime packages and A/B startup checks passed");return 0;
}

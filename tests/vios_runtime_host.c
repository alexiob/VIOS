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
static int hotkey;int platform_keyboard_hotkey(void){int k=hotkey;hotkey=0;return k;}
static char root[512],error[96];
static unsigned ready;
static unsigned fail_confirmation,tear_package;
static void path(char *out,const char *p){assert(p[0]=='/'&&!strstr(p,".."));assert(strlen(root)+strlen(p)<768);strcpy(out,root);strcat(out,p);}
static char console_capture[4096];static size_t console_used;
static char captured[2048];static size_t captured_length;static int capturing;
void platform_console_putc(unsigned char c){putchar(c);if(console_used+1<sizeof console_capture){console_capture[console_used++]=(char)c;console_capture[console_used]=0;}
 if(capturing&&captured_length+1<sizeof captured){captured[captured_length++]=(char)c;captured[captured_length]=0;}}
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
/* Order of display mode changes ('M') and console view commands ('0'..'2'). */
static char vdp_trace[512];static unsigned vdp_trace_length;
static void trace_vdp(char c){if(vdp_trace_length+1<sizeof vdp_trace){vdp_trace[vdp_trace_length++]=c;vdp_trace[vdp_trace_length]=0;}}
void platform_vdp_send(const void *data,size_t size){const unsigned char *bytes=data;if((size>=2&&bytes[0]==22)||(size==4&&bytes[0]==23&&bytes[1]==0&&bytes[2]==0xE6)){vdp_setup_length=0;++vdp_modes;trace_vdp('M');}
 if(size==4&&bytes[0]==23&&bytes[1]==0&&bytes[2]==0xE5)trace_vdp((char)('0'+bytes[3]));if(vdp_setup_length+size<=sizeof vdp_setup){memcpy(vdp_setup+vdp_setup_length,data,size);vdp_setup_length+=size;}++vdp_calls;if(size>vdp_max)vdp_max=(unsigned)size;}
int platform_vdp_request(const void *command,size_t length,unsigned char reply,void *data,size_t capacity,unsigned *actual){
 (void)command;(void)length;unsigned char *p=data;
 if(reply==0x86&&capacity>=8){unsigned char mode[]={128,2,224,1,80,60,16,0};memcpy(p,mode,8);*actual=8;return 1;}
 if(reply==0x83&&capacity>=1){++vdp_fences;if(vdp_fence_fail)return 0;p[0]='V';*actual=1;return 1;}
 if(reply==0x84&&capacity>=4){p[0]=0x12;p[1]=0x34;p[2]=0x56;p[3]=2;*actual=4;return 1;}
 if(reply==0x85&&capacity>=2){p[0]=0;p[1]=1;*actual=2;return 1;}
 return 0;
}
uint32_t platform_millis(void){return 1234567;}
/* Fake Mac host service over the same protocol (files under ROOT/HOSTSHARE). */
static FILE *host_files[4];static int host_online=1;
static int host_bad_reply;
static int host_serve(const unsigned char *r,size_t length,unsigned char *o,unsigned *actual);
int platform_host_request(const void *request,size_t length,void *reply,size_t capacity,unsigned *actual){
 const unsigned char *q=request;unsigned char *o=reply;*actual=0;(void)capacity;
 if(!host_online||length<2)return 0;
 unsigned n=0;int ok=host_serve(q+1,length-1,o+1,&n);if(!ok)return 0;o[0]=q[0];*actual=n+1;
 if(host_bad_reply==1){o[0]=(unsigned char)(q[0]+1);}                    /* stale id */
 if(host_bad_reply==2&&o[1]=='K'&&q[1]=='R'){memset(o+2,'X',230);*actual=232;} /* oversized */
 return 1;
}
static int host_serve(const unsigned char *r,size_t length,unsigned char *o,unsigned *actual){
 if(r[0]=='O'){char name[200],full[768];size_t n=length-2<sizeof name-1?length-2:sizeof name-1;memcpy(name,r+2,n);name[n]=0;
  if(strstr(name,"..")||name[0]=='/'){memcpy(o,"Eforbidden path",15);*actual=15;return 1;}
  snprintf(full,sizeof full,"%s/HOSTSHARE/%s",root,name);
  for(unsigned h=0;h<4;++h)if(!host_files[h]){host_files[h]=fopen(full,r[1]=='w'?"wb":"rb");if(!host_files[h]){memcpy(o,"Eno such host file",18);*actual=18;return 1;}o[0]='K';o[1]=(unsigned char)h;*actual=2;return 1;}
  memcpy(o,"Etoo many host files",20);*actual=20;return 1;}
 unsigned h=length>1?r[1]:99;if(h>=4||!host_files[h]){memcpy(o,"Ebad host handle",16);*actual=16;return 1;}
 if(r[0]=='R'){size_t got=fread(o+1,1,r[2],host_files[h]);o[0]='K';*actual=(unsigned)got+1;return 1;}
 if(r[0]=='W'){size_t put=fwrite(r+2,1,length-2,host_files[h]);o[0]='K';o[1]=(unsigned char)put;*actual=2;return 1;}
 if(r[0]=='C'){fclose(host_files[h]);host_files[h]=NULL;o[0]='K';*actual=1;return 1;}
 memcpy(o,"Eunknown request",16);*actual=16;return 1;
}
unsigned platform_console_dropped(void){return 0;}
void platform_boot_ready(void){++ready;}
static unsigned esp_epoch_fake;unsigned platform_vdp_epoch(void){return esp_epoch_fake;}
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
#include <dirent.h>
int storage_list(const char *p,void (*each)(const char *,unsigned long,int,void *),void *ctx){
 char full[768];path(full,p);DIR *d=opendir(full);if(!d){strcpy(error,"path not found");return 0;}
 struct dirent *e;while((e=readdir(d))){if(e->d_name[0]=='.')continue;char item[1024];snprintf(item,sizeof item,"%s/%s",full,e->d_name);
  struct stat st;if(stat(item,&st))continue;each(e->d_name,(unsigned long)st.st_size,S_ISDIR(st.st_mode),ctx);}
 closedir(d);return 1;
}
static FILE *streams[STORAGE_STREAMS];
int storage_stream_open(unsigned s,const char *p,int write){char full[768];if(s>=STORAGE_STREAMS||streams[s]){strcpy(error,"invalid stream slot");return 0;}path(full,p);streams[s]=fopen(full,write?"wb":"rb");if(!streams[s]){strcpy(error,"file not found");return 0;}return 1;}
int storage_stream_read(unsigned s,void *b,size_t n,size_t *got){*got=fread(b,1,n,streams[s]);return !ferror(streams[s]);}
int storage_stream_write(unsigned s,const void *b,size_t n,size_t *put){*put=fwrite(b,1,n,streams[s]);return *put==n;}
int storage_stream_close(unsigned s){if(s>=STORAGE_STREAMS||!streams[s])return 0;int ok=!fclose(streams[s]);streams[s]=NULL;return ok;}
static unsigned record_previous(void){unsigned char a[32],b[32];size_t an=0,bn=0;int av=storage_read_file("/VIOS/BOOT0.REC",a,32,&an)&&an==32,bv=storage_read_file("/VIOS/BOOT1.REC",b,32,&bn)&&bn==32;assert(av||bv);return !av?b[6]:!bv?a[6]:vios_get32(a+12)>vios_get32(b+12)?a[6]:b[6];}
static void reset(void){forth_init();assert(vios_runtime_init());}
static void eval(const char *s){if(!forth_eval(s)){fprintf(stderr,"eval failed: %s: %s\n",s,forth_last_error());abort();}}
static const char *manifest(void){captured_length=0;captured[0]=0;capturing=1;eval("BOOT-MANIFEST");platform_console_flush();capturing=0;assert(!fvm_depth);return captured;}
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
 (void)storage_mkdir("/vios");assert(storage_write_file("/vios/boot.cnt","\2",1));   /* the MOS stub's counter */
 assert(vios_boot_trial());reset();assert(vios_boot_startup());
 eval("VALUE");assert(fvm_pop()==11);assert(ready);
 {char c;size_t got;assert(!storage_read_file("/vios/boot.cnt",&c,1,&got));}               /* healthy: cleared */
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
 /* The manifest reports a corrupt candidate without failing or disturbing state. */
 char line[160];const char *m=manifest();
 assert(strstr(m,"manifest A confirmed modules 2 crc32 "));assert(strstr(m,"manifest B candidate error invalid/incompatible boot bundle\r\n"));
 snprintf(line,sizeof line,"manifest-module A demo version 1 crc32 %lu package ok\r\n",(unsigned long)forth_module_crc32(source));assert(strstr(m,line));
 snprintf(line,sizeof line,"manifest-module A extra version 1 crc32 %lu package ok\r\n",(unsigned long)forth_module_crc32(extra));assert(strstr(m,line));
 assert(!strstr(m,"manifest-module B"));
 reset();assert(vios_boot_startup());eval("VALUE");assert(fvm_pop()==11);
 m=manifest();assert(strstr(m,"manifest NONE candidate none\r\n"));
 /* Package problems are per entry: a missing and a corrupt package still list name and CRC. */
 char demo_path[128],extra_path[128];assert(vios_module_path("demo",demo_path,sizeof demo_path));assert(vios_module_path("extra",extra_path,sizeof extra_path));
 assert(storage_remove(extra_path));assert(storage_write_file(demo_path,&corrupt,1));m=manifest();
 snprintf(line,sizeof line,"manifest-module A demo version - crc32 %lu package bad\r\n",(unsigned long)forth_module_crc32(source));assert(strstr(m,line));
 snprintf(line,sizeof line,"manifest-module A extra version - crc32 %lu package unreadable\r\n",(unsigned long)forth_module_crc32(extra));assert(strstr(m,line));
 /* Existing source files load unchanged, including nested INCLUDE. */
 const char child[]=": CHILD 37 ;";const char parent[]="INCLUDE /VIOS/CHILD.FTH\n: PARENT CHILD 5 + ;";
 assert(storage_write_file("/VIOS/CHILD.FTH",child,sizeof child-1));assert(storage_write_file("/VIOS/PARENT.FTH",parent,sizeof parent-1));
 eval("LOAD /VIOS/PARENT.FTH PARENT");assert(fvm_pop()==42);
 /* User startup script: absent -> 0; present -> loaded; failing -> -1. */
 reset();assert(vios_user_startup()==0);
 {const char *user="VARIABLE CONF 42 CONF !\n";assert(storage_write_file("/VIOS/USER.FTH",user,strlen(user)));
  assert(vios_user_startup()==1);assert((eval("CONF @"),fvm_pop())==42);
  const char *bad=": OOPS 1 0 / ; OOPS\n";assert(storage_write_file("/VIOS/USER.FTH",bad,strlen(bad)));
  assert(vios_user_startup()==-1&&fvm_depth==0);assert(storage_remove("/VIOS/USER.FTH"));assert(vios_user_startup()==0);}
 /* Stage 5: reboot into an application from SD. A module defining AUTORUN
  * goes into a boot trial; after the simulated reboot the confirmed slot is
  * loaded and AUTORUN starts as a console task without any host. */
 /* A fresh emulated card keeps this independent of the corruption tests above. */
 {char saved_root[sizeof root];strcpy(saved_root,root);assert(strlen(root)+9<sizeof root);strcat(root,"/apptest");
  assert(!mkdir(root,0700));reset();assert(storage_mkdir("/VIOS"));assert(vios_boot_startup());
  assert(vios_autorun()==0&&"no AUTORUN on a fresh card");
  const char *app="VARIABLE APPRAN : AUTORUN 7 APPRAN ! .\" app running\" CR ;";
  assert(forth_module_stage("app","1",NULL,0,app,""));assert(forth_module_activate("app"));
  assert(vios_boot_trial());reset();assert(vios_boot_startup());
  int id=vios_autorun();assert(id>=1);for(int i=0;i<20&&forth_tasks_run();++i){}
  eval("APPRAN @");assert(fvm_pop()==7);
  strcpy(root,saved_root);}
 /* Core library (Forth prelude) and compiler data/string words. */
 reset();
 {
  struct {const char *src;fcell expect;} cases[]={
   {"3 2 >",(fcell)-1&FVM_MASK},{"2 3 >",0},{"-1 0<",(fcell)-1&FVM_MASK},{"5 0>",(fcell)-1&FVM_MASK},{"4 4 <>",0},
   {"1 2 3 ROT",1},{"1 2 3 -ROT DROP DROP",3},{"1 2 NIP",2},{"1 2 TUCK DROP DROP",2},{"0 ?DUP DEPTH",1},
   {"-7 ABS",7},{"3 9 MAX",9},{"3 9 MIN",3},{"-5 2 MIN",(fcell)-5&FVM_MASK},
   {"1 2 3 4 2SWAP DROP",1},{"1 2 3 4 2OVER + + + + +",13},{"21 2*",42},{"5 0 10 WITHIN",(fcell)-1&FVM_MASK},{"10 0 10 WITHIN",0},
   {"CHAR a",'a'},{"CHAR Z",'Z'},
   {"17 5 /MOD DROP",2},{"17 5 /MOD NIP",3},{"6 7 3 */",14},{"10 3 BOUNDS -",3},
  };
  for(unsigned i=0;i<sizeof cases/sizeof cases[0];++i){eval(cases[i].src);fcell v=fvm_pop();if(v!=cases[i].expect){fprintf(stderr,"%s -> %lu\n",cases[i].src,(unsigned long)v);abort();}fvm_reset();}
  eval("CREATE TABLE 7 , 9 , 65 C, HERE TABLE -");assert(fvm_pop()==7);
  eval("TABLE @ TABLE CELL+ @ + TABLE 6 + C@ +");assert(fvm_pop()==7+9+65);
  eval("CREATE BUF 10 ALLOT BUF 10 + HERE =");assert(fvm_pop()==((fcell)-1&FVM_MASK));
  eval("VARIABLE TOTAL 5 TOTAL ! 3 TOTAL +! TOTAL @");assert(fvm_pop()==8);
  eval("CREATE MB 8 ALLOT MB 8 65 FILL MB 2 + MB 3 MOVE MB 4 + C@ MB 3 ERASE MB 2 + C@ +");assert(fvm_pop()==65);
  eval("CREATE OV 1 C, 2 C, 3 C, 4 C, OV OV 1+ 3 MOVE OV 3 + C@ OV 1+ C@ 10 * +");assert(fvm_pop()==3+10*1);
  eval("CREATE DB 171 C, 5 C, 16 C,");console_used=0;eval("DB 3 DUMP BASE @");assert(fvm_pop()==10&&"DUMP restores BASE");
  assert(strstr(console_capture,"AB 05 ")&&strstr(console_capture,"10 ")&&"bytes in hex; a new line starts each 16-byte boundary");
  eval(": GREET .\" hi\" [CHAR] ! EMIT ; : PAD3 3 SPACES ; S\" xyz\" DROP COUNT");assert(fvm_pop()=='x');fvm_reset();
  assert(!forth_eval(": BAD HERE ;")&&strstr(forth_last_error(),"data-space word inside definition"));
  assert(!forth_eval("-1 ALLOT")&&strstr(forth_last_error(),"negative ALLOT"));
  assert(!forth_eval(": BAD2 CHAR x ;")&&strstr(forth_last_error(),"[CHAR]"));
  fflush(stdout);eval("GREET PAD3 S\" ok\" TYPE CR");
 }
 /* AUTORUN convention: absent -> nothing; present -> console task. */
 reset();assert(vios_autorun()==0);
 eval("VARIABLE RAN : AUTORUN 1 RAN ! ;");{int id=vios_autorun();assert(id==1);forth_tasks_run();assert((eval("RAN @"),fvm_pop())==1);}
 /* Pipelines: producer stdout feeds consumer KEY; EOF follows the producer. */
 reset();
 eval(": SAY S\" hello pipes\" TYPE CR ; : UPPER BEGIN KEY DUP -1 <> WHILE DUP 97 123 WITHIN IF 32 - THEN EMIT REPEAT DROP ;");
 console_used=0;console_capture[0]=0;eval("' SAY ' UPPER PIPELINE 2DROP");
 for(int i=0;i<200&&forth_tasks_run();++i){}
 assert(strstr(console_capture,"HELLO PIPES")&&"pipeline output reached the console");
 {struct forth_task_info a,b;assert(forth_task_info(1,&a)&&forth_task_info(2,&b));assert(a.state==3&&b.state==3);}
 eval("VARIABLE GOT : BIG 1000 0 DO 65 EMIT LOOP ; : CNT 0 GOT ! BEGIN KEY -1 <> WHILE 1 GOT +! REPEAT ;");
 eval("' BIG ' CNT PIPELINE 2DROP");for(int i=0;i<2000&&forth_tasks_run();++i){}
 assert((eval("GOT @"),fvm_pop())==1000&&"backpressure delivered every byte");
 {struct forth_task_info a;for(unsigned id=1;id<=2;++id){assert(forth_task_info(id,&a)&&a.output_dropped==0);}}
 /* Native TYPE resumes the rest of a long string after backpressure. */
 eval("CREATE LONGB 300 ALLOT LONGB 300 90 FILL : LONGT LONGB 300 TYPE ;");
 eval("' LONGT ' CNT PIPELINE 2DROP");for(int i=0;i<2000&&forth_tasks_run();++i){}
 assert((eval("GOT @"),fvm_pop())==300&&"TYPE delivered the whole string through the pipe");
 /* A failing producer keeps its own error and its buffered bytes arrive. */
 eval(": FAILP 65 EMIT 66 EMIT DROP ; VARIABLE FG : FCNT 0 FG ! BEGIN KEY -1 <> WHILE 1 FG +! REPEAT ;");
 eval("' FAILP ' FCNT PIPELINE DROP");{fcell producer=fvm_pop();for(int i=0;i<200&&forth_tasks_run();++i){}
  struct forth_task_info t;assert(forth_task_info((unsigned)producer,&t)&&t.state==4&&!strcmp(t.error,"data stack underflow"));
  assert((eval("FG @"),fvm_pop())==2&&"buffered output of a failed task is delivered");}
 eval(": QUIT1 ; ' BIG ' QUIT1 PIPELINE 2DROP");for(int i=0;i<200&&forth_tasks_run();++i){}
 {struct forth_task_info a;unsigned failed=0;for(unsigned id=1;id<=FORTH_TASKS_MAX;++id)if(forth_task_info(id,&a)&&a.state==4&&!strcmp(a.error,"broken pipe"))++failed;assert(failed==1);}
 console_used=0;console_capture[0]=0;eval("HANDLES");assert(!strstr(console_capture,"handle ")&&"pipeline ends are closed by their tasks");
 /* Host file service: DIR, HANDLE-PUT, HANDLE-GET through handles. */
 {char cmd[96];eval("VARIABLE SVH");eval("S\" /VIOS/SVC.TXT\" 2 OPEN SVH !");fcell h=(eval("SVH @"),fvm_pop());
  snprintf(cmd,sizeof cmd,"HANDLE-PUT %u 68656C6C6F",(unsigned)h);console_used=0;eval(cmd);assert(strstr(console_capture,"put 5"));
  eval("SVH @ CLOSE DROP");eval("S\" /VIOS/SVC.TXT\" 1 OPEN SVH !");h=(eval("SVH @"),fvm_pop());
  snprintf(cmd,sizeof cmd,"HANDLE-GET %u 64",(unsigned)h);console_used=0;eval(cmd);assert(strstr(console_capture,"get 68656C6C6F"));
  console_used=0;eval(cmd);assert(strstr(console_capture,"get -"));eval("SVH @ CLOSE DROP");
  console_used=0;eval("DIR /VIOS");assert(strstr(console_capture,"entry SVC.TXT size 5 dir 0"));
  assert(!forth_eval("HANDLE-GET 99 4")&&strstr(forth_last_error(),"bad handle"));}
 /* Mac files over USB: /host/ handles through the host service. */
 {char share[768];snprintf(share,sizeof share,"%s/HOSTSHARE",root);mkdir(share,0700);
  eval("VARIABLE HH CREATE HB 64 ALLOT");
  eval("S\" /host/notes.txt\" 2 OPEN HH !");assert((eval("HH @"),fvm_pop())>=1);
  assert((eval("S\" from the agon\" HH @ WRITE-ALL"),fvm_pop())==0&&(eval("HH @ CLOSE"),fvm_pop())==0);
  char full[800];snprintf(full,sizeof full,"%s/notes.txt",share);FILE *f=fopen(full,"rb");char text[32]={0};assert(f&&fread(text,1,sizeof text,f)==13);fclose(f);assert(!strcmp(text,"from the agon"));
  eval("S\" /host/notes.txt\" 1 OPEN HH !");assert((eval("HB 64 HH @ READ"),fvm_pop())==13&&(eval("HB C@"),fvm_pop())=='f');
  assert((eval("HB 64 HH @ READ"),fvm_pop())==0);eval("HH @ CLOSE DROP");
  assert((eval("S\" /host/missing.txt\" 1 OPEN"),fvm_pop())==((fcell)-5&FVM_MASK));
  assert((eval("S\" /host/../etc/passwd\" 1 OPEN"),fvm_pop())==((fcell)-5&FVM_MASK));
  host_online=0;assert((eval("S\" /host/notes.txt\" 1 OPEN"),fvm_pop())==((fcell)-5&FVM_MASK));host_online=1;
  {char app[800];snprintf(app,sizeof app,"%s/app.fth",share);FILE *g=fopen(app,"wb");fputs(": FROMMAC 6 7 * ;\n",g);fclose(g);}
  eval("LOAD /host/app.fth FROMMAC");assert(fvm_pop()==42&&"LOAD reads source from the Mac");
  assert(!forth_eval("LOAD /host/nope.fth")&&strstr(forth_last_error(),"no such host file"));
  /* Hostile/confused replies: a stale id or an oversized read never corrupt memory. */
  host_bad_reply=1;assert((eval("S\" /host/notes.txt\" 1 OPEN"),fvm_pop())==((fcell)-5&FVM_MASK));host_bad_reply=0;
  for(unsigned h=0;h<4;++h)if(host_files[h]){fclose(host_files[h]);host_files[h]=NULL;}
  eval("S\" /host/notes.txt\" 1 OPEN HH !");host_bad_reply=2;
  assert((eval("HB 4 HH @ READ"),fvm_pop())==((fcell)-6&FVM_MASK)&&"oversized reply rejected");host_bad_reply=0;eval("HH @ CLOSE DROP");
  for(unsigned h=0;h<4;++h)assert(!host_files[h]&&"every host handle was closed");}
 /* User-space devices: a Forth word serves /dev/NAME through a pipe. */
 {eval("CREATE DVB 16 ALLOT VARIABLE DVH : COUNTER 5 0 DO I 48 + EMIT LOOP ; S\" count\" ' COUNTER DEVICE");
  eval("S\" /dev/count\" 1 OPEN DVH !");assert((eval("DVH @"),fvm_pop())>=1);
  fcell total=0,n;char first=0;while((n=(eval("DVB 16 DVH @ READ-WAIT"),fvm_pop()))&&n<0x800000){if(!total)first=(char)(eval("DVB C@"),fvm_pop());total+=n;}
  assert(total==5&&first=='0'&&"read device delivered the server's output then EOF");eval("DVH @ CLOSE DROP");
  eval("VARIABLE SINKN : SINK 0 SINKN ! BEGIN KEY -1 <> WHILE 1 SINKN +! REPEAT ; S\" sink\" ' SINK DEVICE");
  eval("S\" /dev/sink\" 2 OPEN DVH ! S\" hello\" DVH @ WRITE-ALL DROP DVH @ CLOSE DROP");
  for(int i=0;i<50&&forth_tasks_run();++i){}
  assert((eval("SINKN @"),fvm_pop())==5&&"write device consumed bytes until EOF");
  console_used=0;eval("HANDLES");assert(strstr(console_capture,"device /dev/count")&&!strstr(console_capture,"handle "));
  eval("S\" count\" 0 DEVICE");assert((eval("S\" /dev/count\" 1 OPEN"),fvm_pop())==((fcell)-5&FVM_MASK));}
 /* Handles: files, pipes between tasks, devices, ownership cleanup. */
 reset();
 #define VALUE(s) (eval(s),fvm_pop())
 #define NEG(n) ((fcell)(-(n))&FVM_MASK)
 eval("S\" ........................................\" DROP CONSTANT BUFA VARIABLE HF VARIABLE PR VARIABLE PW");
 eval("S\" /VIOS/H.TXT\" 2 OPEN HF !");assert(VALUE("HF @")>=1);
 assert(VALUE("S\" hello handles\" HF @ WRITE-ALL")==0&&VALUE("HF @ CLOSE")==0);
 {char full[768];path(full,"/VIOS/H.TXT");FILE *f=fopen(full,"rb");char b[32]={0};assert(f&&fread(b,1,sizeof b,f)==13&&!fclose(f));assert(!memcmp(b,"hello handles",13));}
 eval("S\" /VIOS/H.TXT\" 1 OPEN HF !");
 assert(VALUE("BUFA 5 HF @ READ-WAIT")==5&&VALUE("BUFA C@")=='h');
 assert(VALUE("BUFA 40 HF @ READ")==8&&VALUE("BUFA 40 HF @ READ")==0);
 assert(VALUE("BUFA 4 HF @ WRITE")==NEG(7));assert(VALUE("HF @ CLOSE")==0&&VALUE("HF @ CLOSE")==NEG(3));
 assert(VALUE("S\" /VIOS/NOPE.TXT\" 1 OPEN")==NEG(5));
 /* A producer task fills a pipe; the foreground READ-WAIT runs it via PAUSE. */
 eval("PIPE PW ! PR ! : PROD S\" abc\" PW @ WRITE-ALL DROP ; ' PROD SPAWN DROP");
 assert(VALUE("BUFA 10 PR @ READ-WAIT")==3&&VALUE("BUFA 2 + C@")=='c');
 assert(VALUE("BUFA 10 PR @ READ")==NEG(1));
 eval("PW @ CLOSE DROP");assert(VALUE("BUFA 10 PR @ READ")==0);
 eval("PIPE PW ! PR ! PR @ CLOSE DROP");assert(VALUE("S\" x\" PW @ WRITE")==NEG(2));eval("PW @ CLOSE DROP");
 /* Back-pressure: a 200-byte write into a 128-byte pipe is short, then -1. */
 eval("PIPE PW ! PR !");assert(VALUE("BUFA 200 PW @ WRITE")==128&&VALUE("BUFA 1 PW @ WRITE")==NEG(1));
 eval("PR @ CLOSE DROP PW @ CLOSE DROP");
 /* Handles a task opened are closed when it ends; /dev/cons uses its ring. */
 char close_cmd[32];fcell lowest=VALUE("S\" /dev/null\" 2 OPEN");
 snprintf(close_cmd,sizeof close_cmd,"%u CLOSE",(unsigned)lowest);assert(VALUE(close_cmd)==0);
 eval(": OWN S\" /dev/null\" 2 OPEN DROP ; : CON S\" /dev/cons\" 2 OPEN >R S\" hey\" R@ WRITE DROP R> CLOSE DROP ;");
 eval("' OWN SPAWN DROP");forth_tasks_run();
 assert(VALUE("S\" /dev/null\" 2 OPEN")==lowest&&"task-owned handle was released at exit");assert(VALUE(close_cmd)==0);
 fcell con=VALUE("' CON SPAWN");forth_tasks_run();{struct forth_task_info t;assert(forth_task_info((unsigned)con,&t)&&t.output_bytes==3&&t.state==3);}
 eval("S\" /dev/tasks\" 1 OPEN HF !");assert(VALUE("BUFA 40 HF @ READ")==40&&VALUE("BUFA C@")=='1');eval("HF @ CLOSE DROP");
 assert(VALUE("S\" /dev/tasks\" 2 OPEN")==NEG(7)&&VALUE("S\" /dev/zzz\" 1 OPEN")==NEG(5));
 for(int i=0;i<8;++i)eval("S\" /dev/null\" 1 OPEN DROP");assert(VALUE("S\" /dev/null\" 1 OPEN")==NEG(4));
 eval("HANDLES");
 puts("Runtime packages and A/B startup checks passed");return 0;
}

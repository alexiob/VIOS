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
void platform_console_putc(unsigned char c){putchar(c);if(console_used+1<sizeof console_capture){console_capture[console_used++]=(char)c;console_capture[console_used]=0;}}
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
 /* Stage 5: reboot into an application from SD. A module defining AUTORUN
  * goes into a boot trial; after the simulated reboot the confirmed slot is
  * loaded and AUTORUN starts as a console task without any host. */
 reset();assert(vios_boot_startup());
 {const char *app="VARIABLE APPRAN : AUTORUN 7 APPRAN ! .\" app running\" CR ;";
  assert(forth_module_stage("app","1",NULL,0,app,""));assert(forth_module_activate("app"));
  assert(vios_boot_trial());reset();assert(vios_boot_startup());
  int id=vios_autorun();assert(id>=1);for(int i=0;i<20&&forth_tasks_run();++i){}
  eval("APPRAN @");assert(fvm_pop()==7);
  /* Remove the app again: revert to the previously confirmed bundle. */
  assert(vios_boot_revert());reset();assert(vios_boot_startup());assert(vios_autorun()==0);}
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

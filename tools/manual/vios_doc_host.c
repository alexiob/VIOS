/* Host runtime for the manual tools: the real resident Forth system and
 * kernel words over stubbed devices (from tests/vios_runtime_host.c). Files
 * live under ROOT; the display, the Mac host service and the clock are fakes.
 *   vios_doc_host ROOT --words        print the dictionary (WORDS)
 *   vios_doc_host ROOT --inspect      one line per word: name TAB kind TAB source
 *   vios_doc_host ROOT --check < IN   evaluate blocks separated by lines
 *                                     "#BLOCK name"; report each result
 * GUI batches (VDU 23,0,E4) go to the real ESP display server (lib/vios-vds)
 * as in tests/vios_tools_host.c, so UI words that wait for replies work.
 * Built and run by tools/vios_manual.py. */
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
#include "forth/inspect.h"
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
static void stub_vdp_send(const void *data,size_t size){const unsigned char *bytes=data;if((size>=2&&bytes[0]==22)||(size==4&&bytes[0]==23&&bytes[1]==0&&bytes[2]==0xE6)){vdp_setup_length=0;++vdp_modes;trace_vdp('M');}
 if(size==4&&bytes[0]==23&&bytes[1]==0&&bytes[2]==0xE5)trace_vdp((char)('0'+bytes[3]));if(vdp_setup_length+size<=sizeof vdp_setup){memcpy(vdp_setup+vdp_setup_length,data,size);vdp_setup_length+=size;}++vdp_calls;if(size>vdp_max)vdp_max=(unsigned)size;}
static int stub_vdp_request(const void *command,size_t length,unsigned char reply,void *data,size_t capacity,unsigned *actual){
 (void)command;(void)length;unsigned char *p=data;
 if(reply==0x86&&capacity>=8){unsigned char mode[]={128,2,224,1,80,60,16,0};memcpy(p,mode,8);*actual=8;return 1;}
 if(reply==0x83&&capacity>=1){++vdp_fences;if(vdp_fence_fail)return 0;p[0]='V';*actual=1;return 1;}
 if(reply==0x84&&capacity>=4){p[0]=0x12;p[1]=0x34;p[2]=0x56;p[3]=2;*actual=4;return 1;}
 if(reply==0x85&&capacity>=2){p[0]=0;p[1]=1;*actual=2;return 1;}
 if(reply==0xF9&&capacity>=4){const unsigned char *q=command;assert(length==4&&q[2]==0xE9&&q[3]==5);p[0]=5;p[1]=127;p[2]=255;p[3]=0;*actual=4;return 1;}
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

void (*platform_console_divert(void (*sink)(unsigned char)))(unsigned char){(void)sink;return 0;}
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
/* The ESP side: the real display server behind the stubbed VDP link. */
#include "vds_ui.h"
static uint8_t fb[480][320],*fb_rows[480],font[256*8];
static vds_cell cell_store[VDS_MAX_SURFACES][80*60];
static vds esp_v;static vds_proto esp_p;static vds_ui esp_u;
static uint8_t esp_reply[32],esp_reply_n,esp_reply_valid;static int esp_nudged;
static vds_cell *esp_cells(void *ctx,uint8_t id,uint16_t n){(void)ctx;return id<VDS_MAX_SURFACES&&n<=80*60?cell_store[id]:0;}
static void esp_send(void *ctx,uint8_t code,const uint8_t *d,uint8_t n){(void)ctx;if(code==VDS_PACKET_EVENT){esp_nudged=1;return;}if(n>sizeof esp_reply)n=sizeof esp_reply;memcpy(esp_reply,d,n);esp_reply_n=n;esp_reply_valid=1;}
static void esp_frames(void){for(int i=0;i<4;++i)vds_proto_frame(&esp_p,20,UINT32_MAX);}
static int esp_batches(const unsigned char *b,size_t n){            /* whole 23,0,E4,len,payload packets */
 if(n<4||b[0]!=23||b[1]!=0||b[2]!=0xE4)return 0;
 for(size_t i=0;i+4<=n&&b[i]==23&&b[i+1]==0&&b[i+2]==0xE4;i+=4u+b[i+3])if(i+4u+b[i+3]<=n)vds_proto_batch(&esp_p,b+i+4,b[i+3]);
 return 1;
}
void platform_vdp_send(const void *data,size_t size){if(!esp_batches(data,size))stub_vdp_send(data,size);}
int platform_vdp_request(const void *command,size_t length,unsigned char reply,void *data,size_t capacity,unsigned *actual){
 if(reply!=0xF1)return stub_vdp_request(command,length,reply,data,capacity,actual);
 esp_reply_valid=0;esp_batches(command,length);
 if(!esp_reply_valid||esp_reply_n>capacity){*actual=0;return 0;}
 memcpy(data,esp_reply,esp_reply_n);*actual=esp_reply_n;return 1;
}
int platform_vdp_nudged(void){int n=esp_nudged;esp_nudged=0;return n;}
static void esp_init(const char *font_path){
 FILE *f=fopen(font_path,"rb");if(!f||fread(font,1,sizeof font,f)!=sizeof font){fprintf(stderr,"font %s\n",font_path);exit(2);}fclose(f);
 for(int y=0;y<480;++y)fb_rows[y]=fb[y];
 if(!vds_init(&esp_v,fb_rows,640,480,font)){fprintf(stderr,"vds_init\n");exit(2);}
 vds_proto_init(&esp_p,&esp_v,esp_cells,esp_send,0);vds_ui_init(&esp_u,&esp_p);
}
static void reset(void){forth_init();if(!vios_runtime_init()){fprintf(stderr,"runtime init: %s\n",forth_last_error());exit(2);}}
/* Unused test hooks kept so the copied stubs compile unchanged. */
static void unused(void){(void)vdp_max;(void)vdp_fences;(void)vdp_fence_fail;(void)timer_depth;(void)ready;(void)host_bad_reply;(void)host_online;(void)input_paused;(void)keyboard_route;(void)console_used;(void)record_previous;(void)queued_key;}
static char input[1<<20];
int main(int argc,char **argv){
 (void)unused;
 if(argc<3||argc>4||strlen(argv[1])>=sizeof root){fprintf(stderr,"usage: vios_doc_host ROOT --words|--inspect|--check [FONT.bin]\n");return 2;}
 strcpy(root,argv[1]);
 esp_init(argc==4?argv[3]:"build/manual/font.bin");reset();
 if(!strcmp(argv[2],"--words")){printf("#WORDS");fflush(stdout);forth_eval("WORDS");printf("\n");return 0;}
 if(!strcmp(argv[2],"--inspect")){
  for(unsigned i=0;i<forth_word_count();++i){struct forth_word_info w;if(!forth_inspect_word(i,&w))continue;
   printf("%s\t%u\t",w.name,w.kind);for(unsigned k=0;w.source&&k<w.source_length;++k){char c=w.source[k];putchar(c=='\n'||c=='\t'?' ':c);}putchar('\n');}
  return 0;}
 if(strcmp(argv[2],"--check"))return 2;
 size_t n=fread(input,1,sizeof input-1,stdin);input[n]=0;
 int failed=0;char *p=input;
 while((p=strstr(p,"#BLOCK "))){
  char *name=p+7,*eol=strchr(name,'\n');if(!eol)break;*eol=0;
  char *body=eol+1,*next=strstr(body,"\n#BLOCK ");if(next)*next=0;
  printf("\n#RUN %s\n",name);fflush(stdout);
  int ok=forth_eval(body);if(ok&&forth_compiling()){ok=0;}esp_frames();
  printf("\n#RESULT %s %s %s\n",name,ok?"ok":"FAIL",ok?"":forth_last_error());fflush(stdout);
  if(!ok){failed=1;forth_eval("ABORT");}
  if(!next)break;p=next+1;
 }
 return failed;
}

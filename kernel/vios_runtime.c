/* Outer development tools. Device and filesystem details remain outside Forth. */
#include "vios/runtime.h"
#include "vios/ui.h"
#include "vios/workbench_runtime.h"
#include "platform/console.h"
#include "platform/storage.h"
static char include_source[4][FORTH_MODULE_SOURCE_MAX+1];
static unsigned include_depth;
void vios_out(const char *s){while(*s)platform_console_putc((unsigned char)*s++);}
void vios_uint(uint32_t n){char b[11];unsigned i=0;do{b[i++]=(char)('0'+n%10);n/=10;}while(n);while(i)platform_console_putc((unsigned char)b[--i]);}
void vios_hex(uint32_t n,unsigned digits){static const char hex[]="0123456789ABCDEF";while(digits--)platform_console_putc((unsigned char)hex[(n>>(digits*4))&15]);}
int vios_same(const char *a,const char *b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
size_t vios_length(const char *s){size_t n=0;while(s[n])++n;return n;}
int vios_arg(const char **p,char *out,size_t capacity){
 while(**p==' '||**p=='\t'||**p=='\r'||**p=='\n')++*p;
 size_t n=0;while(**p&&**p!=' '&&**p!='\t'&&**p!='\r'&&**p!='\n'){
  if(n+1>=capacity)return forth_error("argument too long");out[n++]=*(*p)++;
 }
 out[n]=0;return n?1:forth_error("missing argument");
}
uint32_t vios_crc32(const void *data,size_t length){
 const unsigned char *p=data;uint32_t crc=0xffffffffUL;
 while(length--){crc^=*p++;for(unsigned i=0;i<8;++i)crc=(crc>>1)^((crc&1)?0xedb88320UL:0);}
 return ~crc;
}
void vios_put32(unsigned char *p,uint32_t v){for(unsigned i=0;i<4;++i){p[i]=(unsigned char)v;v>>=8;}}
uint32_t vios_get32(const unsigned char *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
int vios_load_source(const char *path){
 if(include_depth==4)return forth_error("INCLUDE nesting exceeds 4");
 size_t n=0;char *source=include_source[include_depth];
 /* /host/ paths come from the Mac over USB; everything else from SD. */
 if(path[0]=='/'&&path[1]=='h'&&path[2]=='o'&&path[3]=='s'&&path[4]=='t'&&path[5]=='/'){if(!vios_host_read_file(path,source,FORTH_MODULE_SOURCE_MAX,&n))return forth_error(vios_host_error());}
 else if(!storage_read_file(path,source,FORTH_MODULE_SOURCE_MAX,&n))return forth_error(storage_error());
 for(size_t i=0;i<n;++i)if(!source[i])return forth_error("NUL in source file");
 source[n]=0;++include_depth;int ok=forth_eval(source);--include_depth;
 if(ok&&forth_compiling())return forth_error("incomplete definition in source file");return ok;
}
static int command(const char *word,const char **rest){
 int wb=vios_workbench_command(word,rest);if(wb)return wb;
 if(vios_same(word,"HANDLES")){vios_handles_report();return 1;}
 {int h=vios_handles_command(word,rest);if(h)return h;}
 if(vios_same(word,"LOAD")||vios_same(word,"INCLUDE")){
  char path[128];if(!vios_arg(rest,path,sizeof path)||!vios_load_source(path))return -1;return 1;
 }
 int result=vios_module_command(word,rest);if(result)return result;
 return vios_boot_command(word,rest);
}
/* Durable user configuration, separate from module packages and boot slots:
 * /VIOS/USER.FTH is LOADed once per boot if present. A failure is reported
 * and boot continues; the normal execution budget stops runaway code. */
int vios_user_startup(void){
 char probe[1];size_t n=0;
 if(!storage_read_file("/VIOS/USER.FTH",probe,0,&n)&&vios_same(storage_error(),"file not found"))return 0;
 vios_out("user startup: /VIOS/USER.FTH\r\n");
 if(!vios_load_source("/VIOS/USER.FTH")){vios_out("user startup failed: ");vios_out(forth_last_error());vios_out("\r\n");fvm_reset();return -1;}
 return 1;
}
/* Boot convention: a word named AUTORUN (normally from a boot-slot module)
 * starts as a console-attached background task, so the REPL stays usable
 * and TASK-KILL can stop a misbehaving application. */
int vios_autorun(void){
 if(!forth_find_xt("AUTORUN"))return 0;
 if(!forth_eval("' AUTORUN SPAWN-CON")){vios_out("autorun failed: ");vios_out(forth_last_error());vios_out("\r\n");return 0;}
 fcell id=fvm_pop();vios_out("autorun task ");vios_uint((uint32_t)id);vios_out("\r\n");return (int)id;
}
int vios_runtime_init(void){
 include_depth=0;forth_transport_crc(1);
 return vios_prelude_init()&&vios_services_init()&&vios_handles_init()&&vios_ui_init()&&vios_workbench_runtime_init()&&forth_register_command(command);
}

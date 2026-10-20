#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "forth/modules.h"
static char output[8192];static unsigned used,side_effects;
void platform_console_putc(unsigned char c){assert(used+1<sizeof output);output[used++]=(char)c;output[used]=0;}
int platform_console_getc(void){return -1;}
void platform_console_init(void){}
const char *platform_identity(void){return "test";}
void platform_restart(void){}
static int pure(void){return fvm_push(42);}
static int impure(void){++side_effects;return 1;}
static int command(const char *word,const char **rest){(void)rest;return strcmp(word,"HELLO")==0?fvm_push(99):0;}
static fcell result(const char *s){assert(forth_eval(s));return fvm_pop();}
static void fresh(void){forth_init();used=side_effects=0;output[0]=0;assert(forth_register_native("PURE",pure,FORTH_NATIVE_PURE));assert(forth_register_native("IMPURE",impure,0));}
static void failed_modules(void){
 fresh();assert(forth_eval("VARIABLE X 7 X ! DEFER SERVICE : OLD 11 ; ' OLD IS SERVICE"));
 fcell xt=forth_find_xt("X");assert(xt);assert(strcmp(forth_xt_name(xt),"X")==0);
 assert(forth_eval("X"));fcell variable=fvm_pop();
 assert(forth_eval("HEX"));assert(fvm_push(123));
 size_t before=forth_code_bytes(FORTH_TOKEN);
 assert(!forth_module_stage("bad","1",NULL,0,": NEW 22 ; DECIMAL 99 X ! ' NEW IS SERVICE MISSING",NULL));
 assert(!forth_find_xt("NEW")&&forth_code_bytes(FORTH_TOKEN)==before);
 assert(fvm_depth==1&&fvm_pop()==123&&forth_get_base()==16);
 assert(fvm_read_cell(variable)==7);assert(result("SERVICE")==11);
 assert(!forth_module_stage("io","1",NULL,0,"IMPURE",NULL));assert(!side_effects);
 assert(!forth_module_stage("key","1",NULL,0,"KEY",NULL));
 assert(!forth_module_stage("abort","1",NULL,0,"ABORT",NULL));
 assert(!forth_module_stage("engine","1",NULL,0,"ENGINE TOKEN",NULL));
 assert(!forth_module_stage("memory","1",NULL,0,"DECIMAL 5 0 !",NULL));
 assert(!forth_module_stage("badtest","1",NULL,0,"DECIMAL : NEW 22 ; 99 X ! : TEST 0 ;","TEST"));
 assert(!forth_find_xt("NEW")&&fvm_read_cell(variable)==7&&result("SERVICE")==11);
 assert(!forth_module_stage("compiled-is","1",NULL,0,": NEW 22 ; : BIND ['] NEW IS SERVICE ; BIND MISSING",NULL));
 assert(!forth_find_xt("NEW")&&!forth_find_xt("BIND")&&result("SERVICE")==11);
 assert(!forth_module_stage("invalid-is","1",NULL,0,": BREAK 0 IS SERVICE ; BREAK",NULL));
 assert(!forth_find_xt("BREAK")&&result("SERVICE")==11);
 assert(!forth_module_stage("large-is","1",NULL,0,": BREAK $FFFFFF IS SERVICE ; BREAK",NULL));
 assert(!forth_find_xt("BREAK")&&result("SERVICE")==11);
 assert(!forth_module_stage("emit","1",NULL,0,"65 EMIT",NULL));assert(!used);
 assert(fvm_push(456));assert(!forth_module_stage("bad name","1",NULL,0,": A 1 ;",NULL));
 assert(fvm_depth==1&&fvm_pop()==456);
 assert(forth_eval(": OPEN 1"));assert(forth_compiling());
 assert(!forth_module_stage("idle","1",NULL,0,": A 1 ;",NULL));
 assert(forth_compiling());assert(forth_eval("; OPEN"));assert(fvm_pop()==1);
}
static void activation(void){
 fresh();assert(forth_eval("VARIABLE X 7 X ! DEFER SERVICE : OLD 11 ; ' OLD IS SERVICE"));
 const char *a="DECIMAL : NEW 22 ; 9 X ! ' NEW IS SERVICE : TEST 77 X ! PURE 42 = ;";
 assert(forth_eval("HEX"));
 assert(forth_module_stage("console","1",NULL,0,a,"test"));
 struct forth_module_info staged;assert(forth_module_pending_info(&staged));
 assert(!staged.active&&staged.words==2&&staged.token_bytes>0&&staged.direct_bytes>0);
 assert(strcmp(forth_module_pending_source(),a)==0);
 assert(forth_get_base()==16);
 assert(!forth_find_xt("NEW")&&result("SERVICE")==11&&result("X @")==7);
 assert(forth_module_activate("console"));
 assert(forth_get_base()==16);
 assert(result("SERVICE")==22&&result("X @")==9); /* test writes are not initialization */
 assert(forth_module_count()==1);
 struct forth_module_info info;assert(forth_module_info(0,&info));
 assert(strcmp(info.name,"console")==0&&strcmp(info.version,"1")==0);
 assert(info.source_crc32==forth_module_crc32(a)&&info.source_length==strlen(a));
 assert(strcmp(forth_module_source("console",0),a)==0);
 assert(forth_eval("SEE NEW"));assert(strstr(output,"module console version 1"));
 assert(forth_module_rollback("console"));
 assert(result("SERVICE")==11&&result("X @")==7&&!forth_find_xt("NEW"));
 assert(forth_module_count()==0);
}
static void canonical_base(void){fresh();assert(forth_eval("HEX"));
 assert(forth_module_stage("radix","1",NULL,0,": VALUE 22 ;",NULL));
 assert(forth_get_base()==16);assert(forth_module_activate("radix"));
 assert(forth_get_base()==16&&result("VALUE")==22);
}
static void generations_dependencies(void){
 fresh();assert(forth_eval("DEFER SERVICE : OLD 11 ; ' OLD IS SERVICE"));
 const char *a=": VALUE 22 ; ' VALUE IS SERVICE";
 const char *b=": VALUE 33 ; ' VALUE IS SERVICE";
 assert(forth_module_stage("provider","1",NULL,0,a,NULL));assert(forth_module_activate("provider"));
 struct forth_module_dependency dep={"provider",forth_module_crc32(a)};
 assert(forth_module_stage("consumer","1",&dep,1,": CLIENT SERVICE ;",NULL));
 assert(forth_module_discard());assert(!forth_find_xt("CLIENT"));
 assert(forth_module_stage("provider","2",NULL,0,b,NULL));assert(forth_module_activate("provider"));
 assert(result("SERVICE")==33);struct forth_module_info old;
 assert(forth_module_previous_info("provider",&old)&&strcmp(old.version,"1")==0);
 assert(!forth_module_stage("third","1",&dep,1,": STALE 1 ;",NULL));
 assert(!forth_find_xt("STALE"));
 assert(forth_module_stage("provider","3",NULL,0,": THIRD 3 ;",NULL));assert(forth_module_discard());  /* a third generation may stage */
 assert(forth_module_rollback("provider"));assert(result("SERVICE")==22);
 assert(forth_module_stage("new","1",NULL,0,": NEWEST 44 ;",NULL));assert(forth_module_activate("new"));
 assert(forth_eval(": LATER NEWEST ;"));
 assert(!forth_module_rollback("new"));assert(result("LATER")==44);
}
static void extension_hook(void){fresh();assert(forth_register_command(command));
 assert(result("HELLO")==99);assert(result("1 2 +")==3);
 assert(!forth_module_stage("hook","1",NULL,0,"HELLO",NULL));
 assert(forth_register_command(NULL));}
static void capacities(void){
 fresh();assert(forth_module_crc32("123456789")==0xcbf43926UL);
 for(unsigned i=0;i<FORTH_MODULES_MAX;++i){char name[3]={'m',(char)('0'+i),0};
  assert(forth_module_stage(name,"1",NULL,0,"\\ empty",NULL));assert(forth_module_activate(name));}
 assert(fvm_push(17));assert(!forth_module_stage("extra","1",NULL,0,"",NULL));
 assert(forth_module_count()==FORTH_MODULES_MAX&&fvm_depth==1&&fvm_pop()==17);
 fresh();char large[FORTH_MODULE_SOURCE_MAX+1];large[0]='\\';
 for(unsigned i=1;i<FORTH_MODULE_SOURCE_MAX;++i)large[i]='x';large[FORTH_MODULE_SOURCE_MAX]=0;
 for(unsigned i=0;i<7;++i){char name[3]={'m',(char)('0'+i),0};
  assert(forth_module_stage(name,"1",NULL,0,large,NULL));assert(forth_module_activate(name));}
 assert(!forth_module_stage("full","1",NULL,0,large,NULL));assert(forth_module_count()==7);
 assert(strstr(forth_last_error(),"source pool full"));
}
/* Repeated activations retire the oldest generation; rollback still restores
 * the one before the latest; the pools fill and then refuse cleanly. */
static void retired_generations(void){
 fresh();char source[64],version[8];
 for(unsigned n=1;n<=5;++n){
  snprintf(source,sizeof source,": V %u ; : W V 1+ ;",n);snprintf(version,sizeof version,"%u",n);
  assert(forth_module_stage("m",version,NULL,0,source,NULL));assert(forth_module_activate("m"));
  assert(result("V")==n&&result("W")==n+1);
  struct forth_module_info cur,old;assert(forth_module_info(0,&cur)&&!strcmp(cur.version,version));
  if(n>1){assert(forth_module_previous_info("m",&old));snprintf(version,sizeof version,"%u",n-1);assert(!strcmp(old.version,version));}
 }
 assert(forth_module_rollback("m"));assert(result("V")==4);
 struct forth_module_info cur,old;assert(forth_module_info(0,&cur)&&!strcmp(cur.version,"4"));
 assert(forth_module_previous_info("m",&old)&&!strcmp(old.version,"3"));
 assert(!forth_module_rollback("m"));                       /* one checkpoint only */
 /* A 3 KB module again and again: the source pool (32 KB) runs out, the
  * live module stays as it was. */
 static char big[3001];memset(big,' ',3000);big[0]=':';big[1]=' ';big[2]='V';big[3]=' ';big[4]='7';big[2998]=';';big[3000]=0;
 unsigned accepted=0;
 for(;accepted<20;++accepted){
  snprintf(version,sizeof version,"b%u",accepted);
  if(!forth_module_stage("m",version,NULL,0,big,NULL))break;
  if(!forth_module_activate("m"))break;
 }
 assert(accepted>=8&&accepted<20&&strstr(forth_last_error(),"pool full"));
 assert(forth_module_discard()&&result("V")==7);
 assert(forth_module_info(0,&cur)&&cur.source_length==3000);
}
int main(void){failed_modules();activation();canonical_base();generations_dependencies();extension_hook();capacities();retired_generations();puts("Module transaction checks passed");return 0;}

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "forth/inspect.h"
#include "forth/modules.h"
void platform_console_putc(unsigned char c){(void)c;}
static const int *input_script;static unsigned input_index,input_length;static jmp_buf repl_done;
int platform_console_getc(void){if(input_script){if(input_index==input_length)longjmp(repl_done,1);return input_script[input_index++];}return -1;}
void platform_console_init(void){}
const char *platform_identity(void){return "test";}
void platform_restart(void){}
static unsigned paints;
static int paint(void){assert(fvm_depth==0);++paints;forth_set_base(2);forth_set_engine(FORTH_TOKEN);return 1;}
static int native_failure(void){forth_set_base(2);forth_set_engine(FORTH_DIRECT);return forth_error("paint rejected");}
static int nested(void){char error[64];assert(!forth_observe("PAINT",12,error,sizeof error));assert(strstr(error,"idle")||strstr(error,"observer"));return 1;}
static struct forth_word_info word(const char *name){struct forth_word_info out;fcell xt=forth_find_xt(name);assert(xt);assert(forth_inspect_word((unsigned)xt-1,&out));return out;}
static void metadata(void){
 forth_init();assert(forth_register_native("PAINT",paint,0));
 assert(forth_eval("VARIABLE X 7 X ! $FFFFFF CONSTANT BADPTR DEFER SERVICE : OLD 11 ; ' OLD IS SERVICE : SAY S\" aBc\" 2DROP ;"));
 struct forth_word_info out=word("+");assert(out.kind==FORTH_WORD_PRIMITIVE&&!out.module&&!out.module_version&&!out.value_valid);
 out=word("X");assert(out.kind==FORTH_WORD_VARIABLE&&out.value_valid&&out.value==7);
 out=word("BADPTR");assert(out.kind==FORTH_WORD_CONSTANT&&out.value_valid&&out.value==0xffffffUL);
 out=word("SERVICE");assert(out.kind==FORTH_WORD_DEFER&&out.value_valid&&out.target_xt==forth_find_xt("OLD")&&!strcmp(out.target_name,"OLD"));
 assert(forth_eval("DEFER UNBOUND"));out=word("UNBOUND");assert(out.value_valid&&!out.target_xt&&!out.target_name);
 assert(forth_eval("UNBOUND" )==0); /* Unbound execution must not make inspection unsafe. */
 out=word("SAY");assert(out.kind==FORTH_WORD_COLON&&out.source_length==22);assert(!memcmp(out.source,": SAY S\" aBc\" 2DROP ; ",22));
 assert(out.token_address&&out.direct_address&&out.token_bytes&&out.direct_bytes);
 out=word("PAINT");assert(out.kind==FORTH_WORD_NATIVE&&!out.value_valid);
 const char *source="\\ Exact comment\n: PACKAGE 23 ;";
 assert(forth_module_stage("demo","v1",NULL,0,source,NULL));assert(forth_module_activate("demo"));
 out=word("PACKAGE");assert(out.origin&&out.module&&!strcmp(out.module,"demo")&&!strcmp(out.module_version,"v1"));
 assert(out.module_crc32==forth_module_crc32(source));assert(!strcmp(forth_module_source("demo",0),source));
 struct forth_resources resources;forth_inspect_resources(&resources);
 assert(resources.words==forth_word_count()&&resources.words<resources.words_max);
 assert(resources.token_bytes>0&&resources.token_bytes<resources.token_capacity&&resources.direct_bytes<resources.direct_capacity);
 assert(resources.data_bytes>=9&&resources.data_bytes<resources.data_capacity&&resources.source_bytes>0&&resources.source_bytes<resources.source_capacity);
 assert(!forth_inspect_word(forth_word_count(),&out)&&!forth_inspect_word(0,NULL));forth_inspect_resources(NULL);
}
static void readonly(void){
 forth_init();assert(forth_eval("VARIABLE X 99 X ! DEFER D"));
 struct forth_word_info x=word("X");assert(forth_eval("X"));fcell a=fvm_pop();
 struct forth_word_info d=word("D");fcell binding=fvm_read_cell(d.token_address+1);fvm_write_cell(binding,0xffffffUL);
 d=word("D");assert(d.value_valid&&d.target_xt==0xffffffUL&&!d.target_name);
 /* Corrupted binding contents are an xt candidate, never a pointer. */
 assert(forth_eval("D")==0);assert(forth_eval("$FFFFFF CONSTANT C"));
 fvm_ds[0]=17;fvm_depth=1;fvm_rs[0]=19;fvm_rdepth=1;fvm_loops[0]=21;fvm_ldepth=1;
 fvm_ip=23;fvm_budget=29;fvm_error=FE_ARITH;forth_set_engine(FORTH_DIRECT);
 assert(!forth_transaction_error("keep error"));
 for(unsigned i=0;i<forth_word_count();++i){struct forth_word_info out;assert(forth_inspect_word(i,&out));}
 x=word("X");assert(x.value==99&&fvm_read_cell(a)==99);
 struct forth_resources resources;forth_inspect_resources(&resources);assert(resources.engine==FORTH_DIRECT);
 assert(fvm_depth==1&&fvm_ds[0]==17&&fvm_rdepth==1&&fvm_rs[0]==19&&fvm_ldepth==1&&fvm_loops[0]==21);
 assert(fvm_ip==23&&fvm_budget==29&&fvm_error==FE_ARITH&&!strcmp(forth_last_error(),"keep error"));
}
static void revisions(void){
 forth_init();uint32_t initial=forth_catalog_revision();assert(initial);
 assert(forth_eval(": LIVE 1 ;"));uint32_t live=forth_catalog_revision();assert(live!=initial);
 assert(!forth_module_stage("bad","1",NULL,0,": TEMP 2 ; MISSING",NULL));assert(forth_catalog_revision()==live&&!forth_find_xt("TEMP"));
 assert(forth_module_stage("demo","1",NULL,0,": TEMP 2 ;",NULL));assert(forth_catalog_revision()==live&&!forth_find_xt("TEMP"));
 assert(forth_module_activate("demo"));uint32_t committed=forth_catalog_revision();fcell old=forth_find_xt("TEMP");assert(committed!=live&&old);
 assert(forth_module_rollback("demo"));uint32_t rolled=forth_catalog_revision();assert(rolled!=committed&&!forth_find_xt("TEMP"));
 assert(forth_eval(": REUSED 3 ;"));assert(forth_find_xt("REUSED")==old&&forth_catalog_revision()!=rolled);
 uint32_t before_init=forth_catalog_revision();forth_init();assert(forth_catalog_revision()!=before_init);
}
static fcell app_stack[FVM_DS];
static void application(void){
 forth_init();paints=0;assert(forth_register_native("PAINT",paint,0));assert(forth_register_native("NESTED",nested,0));assert(forth_register_native("NATIVEFAIL",native_failure,0));
 assert(forth_eval(": BAD DROP ; : BADBASE HEX DROP ; DEFER SERVICE ' PAINT IS SERVICE : LEAK 7 ; : FOREVER BEGIN AGAIN ; : RETERR R> ; : LOOPERR I ; : BURN 0 100 0 DO 1 + LOOP DROP ;"));
 forth_set_base(16);forth_set_engine(FORTH_TOKEN);
 for(unsigned i=0;i<FVM_DS;++i)app_stack[i]=fvm_ds[i]=(fcell)(100+i);
 fvm_depth=3;fvm_budget=123;fvm_ip=321;fvm_error=FE_ARITH;fvm_ds_peak=77;fvm_rs_peak=88;
 assert(!forth_transaction_error("application error"));
}
static void preserved(void){
 assert(fvm_depth==3&&fvm_rdepth==0&&fvm_ldepth==0);
 for(unsigned i=0;i<FVM_DS;++i)assert(fvm_ds[i]==app_stack[i]);
 assert(forth_get_base()==16&&forth_engine()==FORTH_TOKEN&&fvm_budget==123&&fvm_ip==321&&fvm_error==FE_ARITH);
 assert(fvm_ds_peak==77&&fvm_rs_peak==88&&!strcmp(forth_last_error(),"application error"));
}
static void success(void){application();char error[96];assert(forth_observe("PAINT",4096,error,sizeof error));assert(!error[0]&&paints==1);preserved();assert(forth_observe("SERVICE",4096,NULL,0));preserved();}
static void errors(void){
 const char *names[]={"BAD","BADBASE","NATIVEFAIL","LEAK","RETERR","LOOPERR","MISSING","KEY"};
 for(unsigned i=0;i<8;++i){application();char error[96];assert(!forth_observe(names[i],4096,error,sizeof error));assert(error[0]);preserved();}
 application();char short_error[2];assert(!forth_observe("BAD",2,short_error,sizeof short_error));assert(short_error[0]&&short_error[1]==0);preserved();
}
static void budget(void){application();char error[96];assert(!forth_observe("FOREVER",17,error,sizeof error));assert(strstr(error,"budget"));preserved();assert(!forth_observe("BURN",10,error,sizeof error));assert(strstr(error,"budget"));preserved();assert(forth_observe("BURN",4096,error,sizeof error));preserved();}
static void guards(void){
 application();char error[96];assert(!forth_observe("PAINT",0,error,sizeof error));preserved();
 fvm_rdepth=1;assert(!forth_observe("PAINT",20,error,sizeof error));assert(fvm_rdepth==1);fvm_rdepth=0;preserved();
 fvm_ldepth=1;assert(!forth_observe("PAINT",20,error,sizeof error));assert(fvm_ldepth==1);fvm_ldepth=0;preserved();
 assert(forth_eval(": OPEN 1"));assert(!forth_observe("PAINT",20,error,sizeof error)&&forth_compiling());assert(forth_eval(";"));
 assert(forth_register_native("CHECK-IDLE",nested,FORTH_NATIVE_PURE));
 assert(forth_module_stage("guard","1",NULL,0,"CHECK-IDLE",NULL));assert(!paints);
}
static unsigned idle_calls;
static void idle(int changed){
 static const int expected[]={1,0,1,1,1,1};
 assert(idle_calls<sizeof expected/sizeof expected[0]);assert(changed==expected[idle_calls++]);
 assert(!forth_compiling());
 fcell depth=fvm_depth,top=depth?fvm_ds[depth-1]:0;char error[96];
 assert(forth_observe("PAINT",100,error,sizeof error));
 assert(fvm_depth==depth&&(!depth||fvm_ds[depth-1]==top));
}
static void repl(void){
 static const int script[]={-1,-1,'1',-1,'\n',-1,':',' ','O','P','E','N',' ','1','\n',-1,';','\n',-1,3,-1,'\n',-1};
 forth_init();assert(forth_register_native("PAINT",paint,0));forth_register_idle(idle);
 input_script=script;input_length=sizeof script/sizeof script[0];input_index=idle_calls=paints=0;
 if(!setjmp(repl_done))forth_repl();
 assert(idle_calls==6&&paints==6);input_script=NULL;
 /* Initialization must remove an old callback. */
 forth_init();static const int reset_script[]={-1};input_script=reset_script;input_length=1;input_index=0;
 if(!setjmp(repl_done))forth_repl();assert(idle_calls==6);input_script=NULL;
}
static void reentry(void){application();char error[96];assert(forth_observe("NESTED",100,error,sizeof error));assert(!paints);preserved();}
int main(int argc,char **argv){assert(argc==2);const char *name=argv[1];
 if(!strcmp(name,"metadata"))metadata();else if(!strcmp(name,"readonly"))readonly();else if(!strcmp(name,"revisions"))revisions();
 else if(!strcmp(name,"success"))success();else if(!strcmp(name,"errors"))errors();else if(!strcmp(name,"budget"))budget();else if(!strcmp(name,"guards"))guards();else if(!strcmp(name,"reentry"))reentry();else if(!strcmp(name,"repl"))repl();else assert(0);
 puts("Forth inspection checks passed");return 0;}

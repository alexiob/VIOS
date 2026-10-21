/* The extensible compiler (docs/superpowers/specs/2026-10-21-vios-extensible-
 * compiler.md): STATE [ ] LITERAL IMMEDIATE POSTPONE COMPILE, FIND, run-time
 * CREATE and DOES> >BODY, EVALUATE, MARKER; inside module transactions, the
 * boot-cache snapshot, tasks and observers. Runs the C reference engine. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "forth/modules.h"
#include "forth/inspect.h"
static char output[16384];static unsigned used;
void platform_console_putc(unsigned char c){if(used+1<sizeof output){output[used++]=(char)c;output[used]=0;}}
int platform_console_getc(void){return -1;}
void platform_console_init(void){}
const char *platform_identity(void){return "test";}
void platform_restart(void){}
static void fresh(void){forth_init();used=0;output[0]=0;}
static fcell top(const char *s){
 if(!forth_eval(s)){fprintf(stderr,"failed: %s -> %s\n",s,forth_last_error());assert(0);}
 assert(fvm_depth>=1);return fvm_pop();
}
static void ok(const char *s){if(!forth_eval(s)){fprintf(stderr,"failed: %s -> %s\n",s,forth_last_error());assert(0);}}
static void refused(const char *s,const char *why){
 if(forth_eval(s)||!strstr(forth_last_error(),why)){fprintf(stderr,"expected '%s' from: %s, got: %s\n",why,s,forth_last_error());assert(0);}
 fvm_reset();
}
#define TRUE_FLAG 0xffffffUL
static void state_and_brackets(void){
 fresh();
 assert(top("STATE @")==0);
 assert(top(": FOLDED [ 2 3 + ] LITERAL ; FOLDED")==5);
 assert(top(": SEEN [ STATE @ ] LITERAL ; SEEN")==0);
 assert(top(": IN-DEF? STATE @ ; IMMEDIATE : PROBE IN-DEF? LITERAL ; PROBE")==TRUE_FLAG);
 assert(top(": AT [ HERE ] LITERAL ; AT HERE =")==TRUE_FLAG);   /* data-space words under [ */
 refused("]","] needs an open definition");
 refused("5 LITERAL","compile-only word");
 refused(": BAD [ IF ] ;","compile-only word");
 assert(!forth_compiling()&&top("STATE @")==0);   /* a failed definition leaves interpretation state */
}
static void immediate_and_postpone(void){
 fresh();
 assert(top(": LIT7 7 POSTPONE LITERAL ; IMMEDIATE : X LIT7 ; X")==7);
 assert(top(": DUP, POSTPONE DUP ; IMMEDIATE : SQ DUP, * ; 5 SQ")==25);
 ok(": UNLESS POSTPONE 0= POSTPONE IF ; IMMEDIATE : T UNLESS 1 ELSE 2 THEN ;");
 assert(top("0 T")==1&&top("5 T")==2);
 assert(top(": LIT7B POSTPONE LIT7 ; IMMEDIATE : Y LIT7B ; Y")==7);   /* POSTPONE of an immediate word */
 assert(top(": TICK-DUP POSTPONE ['] ; IMMEDIATE : Z TICK-DUP DUP ; Z")==top("' DUP"));   /* a parsing keyword */
 assert(top(": END POSTPONE ; ; IMMEDIATE : W 9 END W")==9);
 assert(top(": C-DUP ['] DUP COMPILE, ; IMMEDIATE : Q 3 C-DUP + ; Q")==6);
 assert(top(": TIMES POSTPONE DO ; IMMEDIATE : COUNT3 0 3 0 TIMES 1+ LOOP ; COUNT3")==3);
 refused(": P POSTPONE CHAR ;","keyword cannot be postponed");
 refused(": P POSTPONE NOSUCHWORD ;","unknown word: NOSUCHWORD");
 refused("POSTPONE DUP","compile-only word");
 ok("SEE X");assert(strstr(output,": X LIT7 ;"));   /* SEE shows what was typed */
 used=0;ok("SEE LIT7");assert(strstr(output," immediate"));
 /* IMMEDIATE acts on this unit's latest definition only. */
 fresh();refused("IMMEDIATE","IMMEDIATE needs a definition");
 ok(": A 1 ;");used=0;ok("SEE A");assert(!strstr(output,"immediate"));
 struct forth_word_info info;assert(forth_inspect_word((unsigned)forth_find_xt("A")-1,&info)&&!info.immediate&&info.kind==FORTH_WORD_COLON);
 ok("IMMEDIATE");assert(forth_inspect_word((unsigned)forth_find_xt("A")-1,&info)&&info.immediate&&info.kind==FORTH_WORD_COLON);
}
static void find_word(void){
 fresh();
 ok("CREATE NM 3 C, CHAR D C, CHAR u C, CHAR P C,");
 ok("NM FIND");assert(fvm_pop()==TRUE_FLAG&&fvm_pop()==top("' DUP"));
 ok(": IMM ; IMMEDIATE CREATE NI 3 C, CHAR I C, CHAR M C, CHAR M C, NI FIND");assert(fvm_pop()==1&&fvm_pop()==top("' IMM"));
 ok("CREATE NX 2 C, CHAR Q C, CHAR Q C, NX FIND");assert(fvm_pop()==0&&fvm_pop()==top("NX"));
 ok("CREATE NK 2 C, CHAR I C, CHAR F C, NK FIND");assert(fvm_pop()==0);fvm_reset();   /* keywords have no xt */
}
static void defining_words(void){
 fresh();
 ok(": CONST CREATE , DOES> @ ; 42 CONST ANSWER");
 assert(top("ANSWER")==42);
 assert(top(": USE ANSWER 1+ ; USE")==43);   /* inlined at the call site */
 assert(top("' ANSWER EXECUTE")==42);
 assert(top("' ANSWER >BODY @")==42);
 assert(top(": FIELD CREATE , DOES> @ + ; 6 FIELD .Y 100 .Y")==106);
 assert(top(": ARRAY CREATE CELLS ALLOT DOES> SWAP CELLS + ; 5 ARRAY AR 99 2 AR ! 2 AR @")==99);
 assert(top(": V2 VARIABLE ; V2 FOO 5 FOO ! FOO @")==5);
 assert(top(": K2 CONSTANT ; 12 K2 TWELVE TWELVE")==12);
 assert(top(": COUNTER CREATE 0 , DOES> DUP @ 1+ DUP >R SWAP ! R> ; COUNTER HITS HITS DROP HITS DROP HITS")==3);
 struct forth_word_info info;assert(forth_inspect_word((unsigned)forth_find_xt("ANSWER")-1,&info)&&info.kind==FORTH_WORD_DOES&&info.value_valid&&info.value==42);
 refused(": NOT-CREATED DOES> @ ; : USE2 NOT-CREATED ; 5 USE2","DOES> needs a CREATE word");
 refused(": IN-IF IF DOES> THEN ;","DOES> inside control flow");
 refused("' DUP >BODY",">BODY needs a CREATE word");
 refused(": LATE CREATE , ; 3 LATE L1 : OTHER ; : SETDOES DOES> @ ; SETDOES","DOES> needs a CREATE word");
 /* A failed first ALLOT still removes the CREATEd word (from a defining word too). */
 ok(": BIG CREATE 9000 ALLOT ;");refused("BIG HUGE","data space full");assert(!forth_find_xt("HUGE"));
 /* ... also after DOES> rewrote the child's code: every pool is back where it was. */
 ok(": MK CREATE DOES> @ ;");struct forth_resources before,after;forth_inspect_resources(&before);
 refused("MK X 9000 ALLOT","data space full");forth_inspect_resources(&after);
 assert(!forth_find_xt("X")&&after.words==before.words&&after.token_bytes==before.token_bytes&&after.direct_bytes==before.direct_bytes&&after.source_bytes==before.source_bytes);
}
static void evaluate(void){
 fresh();
 assert(top("S\" 2 3 +\" EVALUATE")==5);
 assert(top(": E S\" 4 5 *\" EVALUATE ; E")==20);
 assert(top(": SQ S\" DUP *\" EVALUATE ; IMMEDIATE : CUBE DUP SQ * ; 3 CUBE")==27);
 used=0;ok("SEE CUBE");assert(strstr(output,": CUBE DUP SQ * ;"));   /* nested tokens are not recorded */
 ok("S\" : NEWW 77 ;\" EVALUATE");assert(top("NEWW")==77);used=0;ok("SEE NEWW");assert(strstr(output,": NEWW 77 ;"));
 assert(top(": OUTER 1 S\" 2\" EVALUATE + 10 * ; OUTER")==30);   /* the caller's return stack survives */
 assert(top(": L 0 3 0 DO S\" I\" EVALUATE + LOOP ; L")==3);   /* and its loop stack */
 ok(": REC S\" REC\" EVALUATE ;");refused("REC","EVALUATE nested too deep");
 refused("S\" WORDS\" EVALUATE","interactive tool inside EVALUATE");
 refused(": BADX S\" 1 UNKNOWNWORD\" EVALUATE ; BADX","unknown word: UNKNOWNWORD");
 assert(fvm_depth==0&&fvm_rdepth==0);
 assert(top("2 3 +")==5);   /* the system carries on */
}
static void markers(void){
 fresh();ok(": KEEP 1 ;");
 struct forth_resources before;forth_inspect_resources(&before);
 ok("MARKER FRESH : A1 1 ; VARIABLE V1 CREATE BUF 30 ALLOT : A2 S\" text\" ;");
 assert(forth_find_xt("A2"));ok("FRESH");
 assert(!forth_find_xt("A1")&&!forth_find_xt("A2")&&!forth_find_xt("FRESH")&&forth_find_xt("KEEP"));
 struct forth_resources after;forth_inspect_resources(&after);
 assert(after.words==before.words&&after.token_bytes==before.token_bytes&&after.direct_bytes==before.direct_bytes&&after.data_bytes==before.data_bytes&&after.source_bytes==before.source_bytes&&after.map_entries==before.map_entries);
 assert(top("KEEP")==1&&top(": REDO 3 ; REDO")==3);
 ok("DEFER D MARKER M2 : T 5 ; ' T IS D");refused("M2","deferred word bound to a removed word");
 ok("' KEEP IS D M2");assert(!forth_find_xt("T"));
 ok("MARKER M3 : RUNM M3 ;");refused("RUNM","run a marker from the interpreter");
}
static void transactions(void){
 fresh();ok(": BEFORE 1 ;");
 assert(forth_module_install("defs","1",NULL,0,
  ": CONST CREATE , DOES> @ ; IMMEDIATE-FREE-HELPER-IS-NOT-HERE",NULL)==0);   /* unknown word: whole load fails */
 assert(!forth_find_xt("CONST"));
 assert(forth_module_install("defs","1",NULL,0,
  ": CONST CREATE , DOES> @ ; 9 CONST NINE : LIT2 2 POSTPONE LITERAL ; IMMEDIATE : ELEVEN NINE LIT2 + ; "
  "S\" : FROM-EVAL 4 ;\" EVALUATE MARKER TMP : SCRATCH ; TMP",NULL));
 assert(top("ELEVEN")==11&&top("FROM-EVAL")==4&&!forth_find_xt("SCRATCH")&&!forth_find_xt("TMP"));
 struct forth_word_info info;assert(forth_inspect_word((unsigned)forth_find_xt("NINE")-1,&info)&&info.module&&!strcmp(info.module,"defs"));
 /* The rollback removes DOES> children and immediates like any module word. */
 assert(forth_module_rollback("defs")&&!forth_find_xt("NINE")&&!forth_find_xt("LIT2"));
 /* IMMEDIATE at a module's start cannot flag an older word. */
 assert(!forth_module_install("flag","1",NULL,0,"IMMEDIATE",NULL)&&strstr(forth_last_error(),"IMMEDIATE needs"));
 assert(forth_inspect_word((unsigned)forth_find_xt("BEFORE")-1,&info)&&!info.immediate);
 /* A module test may not change the dictionary. */
 assert(!forth_module_install("tst","1",NULL,0,": TEST CREATE TRUE ; ","TEST")&&strstr(forth_last_error(),"module test"));
 /* A marker cannot cut into a module, or the activation's rollback point. */
 fresh();ok("MARKER OLD");assert(forth_module_install("m","1",NULL,0,": MW 1 ;",NULL));
 refused("OLD","rollback point");forth_transaction_forget_rollback();refused("OLD","module words");
 assert(!forth_module_install("m2","1",NULL,0,"OLD",NULL)&&strstr(forth_last_error(),"predates"));
}
static unsigned char image[200000];static size_t image_used,image_read;
static int put(void *data,size_t size,void *ctx){(void)ctx;assert(image_used+size<=sizeof image);memcpy(image+image_used,data,size);image_used+=size;return 1;}
static int get(void *data,size_t size,void *ctx){(void)ctx;if(image_read+size>image_used)return 0;memcpy(data,image+image_read,size);image_read+=size;return 1;}
static void snapshot(void){
 fresh();struct forth_snapshot_base base;forth_snapshot_base(&base);
 assert(forth_module_install("lib","1",NULL,0,": CONST CREATE , DOES> @ ; 5 CONST FIVE : TWICE DUP + ; : LIT3 3 POSTPONE LITERAL ; IMMEDIATE",NULL));
 image_used=image_read=0;assert(forth_snapshot_write(&base,put,0)&&forth_modules_snapshot_write(put,0));
 fresh();assert(forth_snapshot_read(&base,get,0)&&forth_modules_snapshot_read(get,0));
 assert(top("FIVE TWICE")==10&&top(": SIX LIT3 TWICE ; SIX")==6&&top("7 CONST SEVEN SEVEN")==7);
}
static void tasks_and_observers(void){
 fresh();ok(": MAKER CREATE ; : TOUCH HERE DROP ; : DEFINE S\" 1\" EVALUATE ;");
 fcell xt=forth_find_xt("MAKER");unsigned id=forth_task_start(xt,0,0);assert(id);forth_tasks_run();
 struct forth_task_info t;assert(forth_task_info(id,&t)&&strstr(t.error,"background task"));
 id=forth_task_start(forth_find_xt("TOUCH"),0,0);assert(id);forth_tasks_run();assert(forth_task_info(id,&t)&&!t.error[0]);
 id=forth_task_start(forth_find_xt("DEFINE"),0,0);assert(id);forth_tasks_run();assert(forth_task_info(id,&t)&&strstr(t.error,"background task"));
 char error[96];ok(": OBS 1 ALLOT ;");assert(!forth_observe("OBS",1000,error,sizeof error)&&strstr(error,"observer"));
}
int main(void){
 state_and_brackets();immediate_and_postpone();find_word();defining_words();evaluate();markers();transactions();snapshot();tasks_and_observers();
 puts("Extensible compiler checks passed");return 0;
}

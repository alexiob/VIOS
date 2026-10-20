/* VIOS resident dual-emitting compiler. CPU execution stays behind vm.h. */
#include "forth/system.h"
#include "forth/inspect.h"
#include "forth/modules.h"
#include "platform/console.h"
#include "platform/system.h"
#include "vios/config.h"
#define WORDS_MAX VIOS_FORTH_WORDS_MAX
#define NAME_MAX 31
#define TOKEN_BYTES 16384
#define DIRECT_BYTES 32768
#define DATA_BYTES 8192
#define SOURCE_BYTES 8192
#define CONTROL_MAX 64
#define HASH_BUCKETS VIOS_FORTH_HASH_BUCKETS

enum { WK_PRIMITIVE,WK_COLON,WK_CONSTANT,WK_VARIABLE,WK_DEFER,WK_NATIVE };
enum { CS_IF,CS_ELSE,CS_BEGIN,CS_WHILE,CS_DO };
struct word {
 char name[NAME_MAX+1]; fcell token,direct,aux;
 unsigned short link,source,length,token_length,direct_length,origin; unsigned char kind,opcode;
};
struct control { unsigned char kind; size_t token,direct; };
static struct word words[WORDS_MAX];
static unsigned short buckets[HASH_BUCKETS];
static unsigned char token_code[TOKEN_BYTES],direct_code[DIRECT_BYTES],data_space[DATA_BYTES];
static char source_space[SOURCE_BYTES];
static struct control controls[CONTROL_MAX];
/* LEAVE and ?DO record forward branches resolved by their loop's end. */
#define LEAVES_MAX 16
static struct control leaves[LEAVES_MAX];
static unsigned leave_used;
static size_t token_used,direct_used,data_used,source_used,control_used;
static size_t mark_token,mark_direct,mark_data,mark_source;
/* Source maps: before each token of a colon definition the compiler records
 * (word, token offset, direct offset, source offset). The code from one
 * entry's offset up to the next entry's belongs to that source token.
 * When the pool is full, later code is simply unmapped. */
#define SOURCE_MAP_MAX 640
static struct {unsigned short word,token,direct,source;} source_map[SOURCE_MAP_MAX];
static unsigned map_used,mark_map;
static unsigned count;
static int current;
static enum forth_engine selected;
static fcell base_address;
static char error_text[96];
static unsigned char transport_crc;
static const char *vm_message(void);
#define NATIVES_MAX 112 /* 6 B each on the eZ80; 102 registered (GUI 45, sound 6), 2026-10-20 */
static struct {int (*callback)(void);unsigned flags;} natives[NATIVES_MAX];
static unsigned native_count,origin_id;
static int (*outer_hook)(const char *,const char **);
static void (*idle_hook)(int);
static void (*line_mode_hook)(int);
static int transaction_active,have_activation;
static fcell transaction_budget;
static uint32_t catalog_revision;
static int observer_active;
static fcell observer_budget;
/* The observer saves application values, not the 8 KiB transaction arena. */
static struct {
 fcell stack[FVM_DS],depth,ip,error,budget,ds_peak,rs_peak,base;
 enum forth_engine engine;
 char error_text[96];
} observer_before;
static void catalog_changed(void) {if(!++catalog_revision)++catalog_revision;}
struct checkpoint {
 size_t token,direct,data,source;unsigned count,map;
 unsigned short buckets[HASH_BUCKETS];
 unsigned char data_bytes[DATA_BYTES];
 fcell stack[FVM_DS],depth,ip,error,budget,ds_peak,rs_peak,base;
 enum forth_engine engine;
};
static struct checkpoint transaction_before,activation_before;
static unsigned char test_data[DATA_BYTES];
static size_t activation_token,activation_direct,activation_data,activation_source;
static unsigned activation_count;
static unsigned transaction_words;
static size_t transaction_token,transaction_direct,transaction_data;
/* Background tasks (docs/superpowers/specs/2026-10-18-vios-tasks.md). The
 * foreground keeps the VM globals; a task's context is swapped in only for a
 * budget-bounded slice and the foreground context is restored afterwards. */
enum { TS_FREE,TS_READY,TS_SLEEPING,TS_DONE,TS_FAILED,TS_KILLED };
struct task {
 fcell ds[FORTH_TASK_DS],rs[FORTH_TASK_RS],loops[FORTH_TASK_LOOPS*2];
 fcell xt,depth,rdepth,ldepth,ip,ds_peak,rs_peak;
 uint32_t wake,slices,steps;
 unsigned short out_head,out_count;unsigned out_dropped;
 unsigned char state,console,in_handle,out_handle,finishing,out[FORTH_TASK_OUT];
 char error[48];
};
static struct task tasks[FORTH_TASKS_MAX];
static struct {
 fcell ds[FVM_DS],rs[FVM_RS],loops[FVM_LOOPS*2];
 fcell depth,rdepth,ldepth,ip,error,budget,ds_peak,rs_peak;
 char error_text[96];
} foreground;
static int running=-1,scheduling,yield_request;
static fcell task_slice=FORTH_TASK_SLICE; /* instructions per background slice */
/* Tick-sliced preemption at VM safe points (docs/superpowers/specs/
 * 2026-10-19-vios-tasks.md, option B, stage B2). The board's tick ISR counts
 * down the delay planned here; when it expires it moves fvm_budget into
 * forth_budget_stolen, zeroes fvm_budget and sets forth_tick_pending, and
 * repeats that on every later tick until the next plan (a steal lost to the
 * engine's budget store is retried). The engine then stops before its next
 * fetch with FE_BUDGET, and dispatch() turns that into a switch: a background
 * slice ends, the foreground runs one scheduler round. The engine is unchanged.
 * Without a registered arm hook (host builds, ZDI boots) nothing changes. */
volatile unsigned char forth_tick_pending;
volatile fcell forth_budget_stolen;
static void (*preempt_arm)(unsigned delay_ms);  /* 0 disarms */
static void (*tick_source)(void);                /* host tests: simulated ticks */
static int (*interrupt_poll)(void);              /* Ctrl-C, checked at foreground ticks */
#define PREEMPT_INTERRUPT_MS 50u                 /* foreground Ctrl-C check when nothing else is due */
static unsigned char preempt_on=1,critical;
static uint32_t preemptions,ticks_ignored;
#define PREEMPT_SLICE_MS 4u                      /* a background slice */
#define PREEMPT_QUANTUM_MS 4u                    /* foreground, while a task is ready */
static void preempt_plan(void);
static int tasks_live(void);
static void preempt_clear(void){forth_tick_pending=0;forth_budget_stolen=0;}
/* Take back a pending steal before C saves or replaces fvm_budget: the ISR
 * keeps stealing until the next plan, also while C runs (gate, KEY, MS). */
static void preempt_reclaim(void){if(forth_tick_pending&&forth_budget_stolen)fvm_budget=forth_budget_stolen;preempt_clear();}
/* TRACE: foreground single-stepping via one-instruction budgets. */
static uint32_t trace_left,trace_steps;static int tracing;
static void trace_step(void);
static unsigned next_task;
static uint32_t (*clock_hook)(void);
static int (*task_gate)(void);
static void (*task_exit_hook)(unsigned);
/* Task stdin/stdout redirection onto kernel handles (kernel/vios_handles.c):
 * write returns 1 accepted, 0 full, -1 broken; read 1 byte, 0 empty, -1 EOF. */
static int (*io_write)(unsigned handle,unsigned char c);
static int (*io_read)(unsigned handle,unsigned char *c);
static int (*io_give)(unsigned handle,unsigned task);
#define OUT_HIGH (FORTH_TASK_OUT-32) /* a BASE 2 . prints up to 26 bytes */
static void out(unsigned char c);

static size_t length(const char *s) { size_t n=0;while(s[n])++n;return n; }
static int equal(const char *a,const char *b) { while(*a&&*a==*b){++a;++b;}return *a==*b; }
static char upper(char c) {return c>='a'&&c<='z'?(char)(c-'a'+'A'):c;}
static void copy(char *d,const char *s) {while((*d++=*s++)) {}}
static void print(const char *s) {while(*s)platform_console_putc((unsigned char)*s++);}
static void unsigned_number(fcell n) {
 char buf[24];unsigned i=0;fcell radix=fvm_read_cell(base_address);
 if(radix<2||radix>36)radix=10;
 do {unsigned d=(unsigned)(n%radix);buf[i++]=(char)(d<10?'0'+d:'A'+d-10);n/=radix;}while(n);
 while(i)out((unsigned char)buf[--i]);
}
static void number(fcell n) {
 if(n&0x800000UL) {out('-');n=((~n)+1)&FVM_MASK;}
 unsigned_number(n);out(' ');
}
static void decimal_number(fcell n) {
 char b[16];unsigned i=0;do {b[i++]=(char)('0'+n%10);n/=10;}while(n);
 while(i)platform_console_putc((unsigned char)b[--i]);
}
static void rollback(void) {
 if(current>=0) {token_used=mark_token;direct_used=mark_direct;data_used=mark_data;source_used=mark_source;map_used=mark_map;}
 current=-1;control_used=0;leave_used=0;
}
/* Stage 4: the foreground's last runtime failure, captured before fail()
 * unwinds the stacks. Frames hold xts; names resolve at report time. */
#define CRASH_DS 8
#define CRASH_RS 16
static struct {
 uint32_t serial,catalog;fcell ip,xt,offset,base,ds[CRASH_DS],ds_depth,rdepth,ldepth;
 fcell frame_xt[CRASH_RS],frame_offset[CRASH_RS];unsigned frames;enum forth_engine engine;char message[96];
} crash;
static unsigned executing;
static fcell code_xt(fcell ip,fcell *offset){
 for(unsigned i=0;i<count;++i){
  fcell start=selected==FORTH_DIRECT?words[i].direct:words[i].token,length=selected==FORTH_DIRECT?words[i].direct_length:words[i].token_length;
  if(ip>=start&&ip<start+length){*offset=ip-start;return (fcell)(i+1);}
 }
 *offset=ip;return 0;
}
static void capture(const char *s){
 fcell saved=fvm_error;++crash.serial;copy(crash.message,s);crash.engine=selected;crash.catalog=catalog_revision;
 /* A budget fault stops before fetching, so IP names the next instruction;
  * other faults happen after the fetch that advanced IP. */
 crash.ip=fvm_ip;crash.xt=code_xt(saved==FE_BUDGET||!fvm_ip?fvm_ip:fvm_ip-1,&crash.offset);crash.base=fvm_read_cell(base_address);
 crash.ds_depth=fvm_depth;crash.rdepth=fvm_rdepth;crash.ldepth=fvm_ldepth;
 for(fcell i=0;i<CRASH_DS;++i)crash.ds[i]=i<fvm_depth&&fvm_depth<=FVM_DS?fvm_ds[fvm_depth-1-i]:0;
 crash.frames=0;
 for(fcell i=0;i<fvm_rdepth&&fvm_rdepth<=FVM_RS&&crash.frames<CRASH_RS;++i){
  fcell r=fvm_rs[fvm_rdepth-1-i];crash.frame_xt[crash.frames]=code_xt(r?r-1:0,&crash.frame_offset[crash.frames]);++crash.frames;
 }
 fvm_error=saved;
}
static int fail(const char *s) {
 if(executing&&running<0&&!transaction_active&&!observer_active)capture(s);
 copy(error_text,s);rollback();fvm_depth=fvm_rdepth=fvm_ldepth=0;return 0;
}
int forth_error(const char *s){return fail(s?s:"native service failed");}
int forth_transaction_error(const char *s){copy(error_text,s);return 0;}
int forth_transaction_active(void){return transaction_active;}
static int unknown(const char *name) {
 char s[96];copy(s,"unknown word: ");copy(s+14,name);return fail(s);
}
static unsigned hash(const char *name) {
 unsigned h=0;while(*name)h=(h*33u+(unsigned char)*name++)&(HASH_BUCKETS-1);return h;
}
static int find(const char *name) {
 unsigned n=buckets[hash(name)];while(n) {if(equal(words[n-1].name,name))return (int)n-1;n=words[n-1].link;}return -1;
}
static fcell address(const unsigned char *p) {return fvm_address(p);}
static void put_cell(unsigned char *p,fcell n) {p[0]=(unsigned char)n;p[1]=(unsigned char)(n>>8);p[2]=(unsigned char)(n>>16);}
static fcell primitive(unsigned op) {
#ifdef __ez80__
 return fvm_direct_primitives[op];
#else
 return (fcell)(0xf00000UL+op*4u);
#endif
}
static fcell docol_address(void) {
#ifdef __ez80__
 return (fcell)(uintptr_t)&fvm_docol;
#else
 return 0xf10000UL;
#endif
}
static int room(size_t t,size_t d) {
 if(t>TOKEN_BYTES-token_used||d>DIRECT_BYTES-direct_used)return fail("code dictionary full");return 1;
}
static int op(unsigned n) {
 if(!room(1,3))return 0;
 token_code[token_used++]=(unsigned char)n;put_cell(direct_code+direct_used,primitive(n));direct_used+=3;return 1;
}
static int operand(fcell t,fcell d) {
 if(!room(3,3))return 0;
 put_cell(token_code+token_used,t);token_used+=3;put_cell(direct_code+direct_used,d);direct_used+=3;return 1;
}
static int literal(fcell n) {return op(F_LIT)&&operand(n,n);}
static int begin_word(const char *name,unsigned kind) {
 if(current>=0)return fail("nested definition");
 if(count==WORDS_MAX)return fail("dictionary full");
 if(!*name||length(name)>NAME_MAX)return fail("invalid name");
 mark_token=token_used;mark_direct=direct_used;mark_data=data_used;mark_source=source_used;mark_map=map_used;
 current=(int)count;words[count].kind=(unsigned char)kind;copy(words[count].name,name);
 words[count].origin=(unsigned short)origin_id;
 words[count].aux=0;words[count].source=(unsigned short)source_used;words[count].length=0;
 words[count].token=address(token_code+token_used);words[count].direct=address(direct_code+direct_used);
 if(!room(0,4))return 0;
 direct_code[direct_used++]=0xcd;put_cell(direct_code+direct_used,docol_address());direct_used+=3;return 1;
}
static int source_add(const char *s) {
 size_t n=length(s);
 if(n+1>SOURCE_BYTES-source_used)return fail("source dictionary full");
 while(*s)source_space[source_used++]=*s++;
 source_space[source_used++]=' ';return 1;
}
static int string_literal(const char **input) {
 const char *p=*input;if(*p==' '||*p=='\t')++p;
 const char *start=p;while(*p&&*p!='"'&&*p!='\r'&&*p!='\n')++p;
 if(*p!='"')return fail("unterminated string");
 size_t n=(size_t)(p-start);
 if(n+1>DATA_BYTES-data_used)return fail("data space full");
 if(current<0&&fvm_depth>FVM_DS-2)return fail("data stack overflow");
 if(current>=0) {
  if(n+2>SOURCE_BYTES-source_used)return fail("source dictionary full");
  for(size_t i=0;i<n;++i)source_space[source_used++]=start[i];
  source_space[source_used++]='"';source_space[source_used++]=' ';
 }
 fcell a=address(data_space+data_used);data_used+=n+1;
 for(size_t i=0;i<n;++i)fvm_write8(a+(fcell)i,(unsigned char)start[i]);
 fvm_write8(a+(fcell)n,0);*input=p+1;
 if(fvm_error)return fail(vm_message());
 return current>=0?(literal(a)&&literal((fcell)n)):(fvm_push(a)&&fvm_push((fcell)n));
}
static int publish(void) {
 unsigned h=hash(words[count].name);
 words[count].link=buckets[h];words[count].length=(unsigned short)(source_used-words[count].source);
 words[count].token_length=(unsigned short)(token_used-mark_token);
 words[count].direct_length=(unsigned short)(direct_used-mark_direct);
 buckets[h]=(unsigned short)(count+1);++count;current=-1;
 if(!transaction_active)catalog_changed();return 1;
}
static int compile_word(int n) {
 struct word *w=&words[n];
 if(w->kind==WK_PRIMITIVE)return op(w->opcode);
 if(w->kind==WK_CONSTANT||w->kind==WK_VARIABLE)return literal(w->aux);
 if(!room(4,3))return 0;
 token_code[token_used++]=F_CALL;put_cell(token_code+token_used,w->token);token_used+=3;
 put_cell(direct_code+direct_used,w->direct);direct_used+=3;return 1;
}
static const char *vm_message(void) {
 static const char *const messages[]={"ok","data stack underflow","data stack overflow","return stack underflow","return stack overflow","loop stack underflow","loop stack overflow","invalid memory","invalid opcode","execution budget exhausted","arithmetic error"};
 return fvm_error<sizeof(messages)/sizeof(messages[0])?messages[fvm_error]:"VM error";
}
static int call_xt(fcell xt) {
 if(!xt||xt>count)return fail("invalid execution token");
 if(fvm_rdepth==FVM_RS)return fail("return stack overflow");
 fvm_rs[fvm_rdepth++]=fvm_ip;
 if(fvm_rdepth>fvm_rs_peak)fvm_rs_peak=fvm_rdepth;
 fvm_ip=selected==FORTH_DIRECT?words[xt-1].direct+4:words[xt-1].token;
 return 1;
}
static fcell entry_ip(unsigned n){return selected==FORTH_DIRECT?words[n].direct+4:words[n].token;}
/* 1: returned, 0: failed (error_text set), 2: background slice ended. */
static int dispatch(void) {
 for(;;) {
  unsigned char result;
#ifdef __ez80__
  if(selected==FORTH_DIRECT)result=fvm_direct_run();
  else if(selected==FORTH_TOKEN)result=fvm_token_run();
  else result=fvm_reference_run();
#else
  result=fvm_reference_run();
#endif
  if(fvm_error==FE_BUDGET&&forth_tick_pending){
   fcell stolen=forth_budget_stolen;preempt_clear();
   if(stolen){   /* a tick, not real exhaustion: resume with the exact budget */
    fvm_error=0;fvm_budget=stolen;
    if(running<0&&interrupt_poll&&interrupt_poll()){preempt_plan();return fail("interrupted");}
    if(!critical&&!tracing&&!transaction_active&&!observer_active){
     if(running>=0){++preemptions;return 2;}
     if(!scheduling&&current<0&&tasks_live()){++preemptions;if(preempt_arm)preempt_arm(0);forth_tasks_run();}
     else ++ticks_ignored;
    }else ++ticks_ignored;
    preempt_plan();if(tick_source)tick_source();continue;
   }
  }
  if(transaction_active)transaction_budget=fvm_budget;
  if(observer_active)observer_budget=fvm_budget;
  if(running>=0&&fvm_error==FE_BUDGET){fvm_error=0;return 2;}
  if(tracing&&running<0&&fvm_error==FE_BUDGET){
   if(!trace_left)return fail("trace step limit reached");
   fvm_error=0;trace_step();fvm_budget=1;continue;
  }
  if(fvm_error)return fail(vm_message());
  if(!result)return 1;
  unsigned code=result-0x80u;
  if(observer_active&&code==F_KEY)return fail("KEY disabled during observer");
  if(transaction_active&&(code==F_EMIT||code==F_DOT||code==F_CR||code==F_KEY))return fail("I/O disabled during module transaction");
  if(running>=0&&code==F_KEY) {
   struct task *t=&tasks[running];unsigned char c=0;
   if(!t->in_handle||!io_read)return fail("KEY unavailable in background task");
   int got=io_read(t->in_handle,&c);
   if(!got){fvm_ip-=selected==FORTH_DIRECT?3:1;return 2;} /* empty: retry this KEY next slice */
   if(!fvm_push(got<0?FVM_MASK:(fcell)c))return fail(vm_message());continue;
  }
  if(code==F_EMIT) {fcell c=fvm_pop();if(!fvm_error)out((unsigned char)c);}
  else if(code==F_DOT) {fcell nvalue=fvm_pop();if(!fvm_error)number(nvalue);}
  else if(code==F_CR){out('\r');out('\n');}
  else if(code==F_KEY) {int c;while((c=platform_console_getc())<0)forth_tasks_run();fvm_push((fcell)c);}
  else if(code==F_EXECUTE||code==F_DEFER) {fcell xt=fvm_pop();if(!fvm_error&&!call_xt(xt))return 0;}
  else if(code==F_NATIVE) {
   fcell id=fvm_pop();if(fvm_error)return fail(vm_message());
   if(id>=native_count)return fail("invalid native service");
   if(transaction_active&&!(natives[id].flags&FORTH_NATIVE_PURE))return fail("native I/O disabled during module transaction");
   if(running>=0&&!(natives[id].flags&(FORTH_NATIVE_PURE|FORTH_NATIVE_TASK)))return fail("native service unavailable in background task");
   if(!natives[id].callback())return error_text[0]?0:fail("native service failed");
   if(running>=0&&yield_request){yield_request=0;if(fvm_error)return fail(vm_message());return 2;}
  }
  else if(!fvm_complex(code))return fail(vm_message());
  if(fvm_error)return fail(vm_message());
  if(running>=0&&yield_request){yield_request=0;return 2;}
  if(tick_source)tick_source();
 }
}
static int execute_index(unsigned n) {
 fvm_error=0;fvm_rdepth=fvm_ldepth=0;
 /* Clear before the budget is set: a steal after this is a pending tick, a
  * steal before it is overwritten (never a budget left at 0 with no tick). */
 preempt_clear();
 fvm_budget=observer_active?observer_budget:transaction_active?transaction_budget:1000000UL;
 fvm_ip=entry_ip(n);++executing;preempt_plan();if(tick_source)tick_source();
 int result=dispatch();--executing;preempt_plan();return result;
}
static void out(unsigned char c) {
 if(running<0||tasks[running].console){platform_console_putc(c);return;}
 struct task *t=&tasks[running];
 if(t->out_count==FORTH_TASK_OUT){++t->out_dropped;return;}
 t->out[(t->out_head+t->out_count)%FORTH_TASK_OUT]=c;++t->out_count;
 /* Redirected output drains between slices; end the slice before the
  * ring can overflow (a . prints at most a few bytes past this mark). */
 if(t->out_handle&&t->out_count>=OUT_HIGH)yield_request=1;
}
static void task_flush(unsigned id);
static void task_drain(unsigned id);
static int task_id(fcell id);
static uint32_t now_ms(void){return clock_hook?clock_hook():0;}
static int tasks_live(void){for(unsigned i=0;i<FORTH_TASKS_MAX;++i)if(tasks[i].state==TS_READY||tasks[i].state==TS_SLEEPING)return 1;return 0;}
static void task_fail(struct task *t,const char *message,unsigned char state) {
 unsigned i=0;while(message[i]&&i+1<sizeof t->error){t->error[i]=message[i];++i;}t->error[i]=0;t->state=state;
}
static void task_exited(unsigned id){if(task_exit_hook)task_exit_hook(id+1);}
static void cells(fcell *to,const fcell *from,fcell n){for(fcell i=0;i<n;++i)to[i]=from[i];}
static void run_slice(unsigned id) {
 struct task *t=&tasks[id];
 if(preempt_arm)preempt_arm(0);preempt_reclaim();   /* the foreground's real budget, not a stolen 0 */
 foreground.depth=fvm_depth;foreground.rdepth=fvm_rdepth;foreground.ldepth=fvm_ldepth;foreground.ip=fvm_ip;
 foreground.error=fvm_error;foreground.budget=fvm_budget;foreground.ds_peak=fvm_ds_peak;foreground.rs_peak=fvm_rs_peak;
 cells(foreground.ds,fvm_ds,fvm_depth);cells(foreground.rs,fvm_rs,fvm_rdepth);cells(foreground.loops,fvm_loops,fvm_ldepth*2);
 copy(foreground.error_text,error_text);
 cells(fvm_ds,t->ds,t->depth);cells(fvm_rs,t->rs,t->rdepth);cells(fvm_loops,t->loops,t->ldepth*2);
 fvm_depth=t->depth;fvm_rdepth=t->rdepth;fvm_ldepth=t->ldepth;fvm_ip=t->ip;fvm_error=0;fvm_budget=task_slice;
 fvm_ds_peak=t->ds_peak;fvm_rs_peak=t->rs_peak;error_text[0]=0;
 running=(int)id;yield_request=0;preempt_clear();fvm_budget=task_slice;preempt_plan();if(tick_source)tick_source();
 int result=dispatch();running=-1;preempt_clear();
 t->steps+=task_slice-fvm_budget;++t->slices;t->ds_peak=fvm_ds_peak;t->rs_peak=fvm_rs_peak;
 if(t->state==TS_KILLED){}
 else if(result==1&&t->out_handle&&t->out_count)t->finishing=1; /* drain, then done */
 else if(result==1)t->state=TS_DONE;
 else if(!result)task_fail(t,error_text[0]?error_text:"task failed",TS_FAILED);
 else if(fvm_depth>FORTH_TASK_DS||fvm_rdepth>FORTH_TASK_RS||fvm_ldepth>FORTH_TASK_LOOPS)task_fail(t,"task stack exceeds saved capacity",TS_FAILED);
 if(t->state!=TS_READY&&t->state!=TS_SLEEPING){task_drain(id);task_exited(id);}
 else if(!t->finishing){
  t->depth=fvm_depth;t->rdepth=fvm_rdepth;t->ldepth=fvm_ldepth;t->ip=fvm_ip;
  cells(t->ds,fvm_ds,fvm_depth);cells(t->rs,fvm_rs,fvm_rdepth);cells(t->loops,fvm_loops,fvm_ldepth*2);
 }
 cells(fvm_ds,foreground.ds,foreground.depth);cells(fvm_rs,foreground.rs,foreground.rdepth);cells(fvm_loops,foreground.loops,foreground.ldepth*2);
 fvm_depth=foreground.depth;fvm_rdepth=foreground.rdepth;fvm_ldepth=foreground.ldepth;fvm_ip=foreground.ip;
 fvm_error=foreground.error;fvm_budget=foreground.budget;fvm_ds_peak=foreground.ds_peak;fvm_rs_peak=foreground.rs_peak;
 copy(error_text,foreground.error_text);
}
unsigned forth_tasks_run(void) {
 /* Sample the clock on every call so MS deadlines see gaps the gate skips. */
 uint32_t now=now_ms();
 if(scheduling||running>=0||current>=0||transaction_active||observer_active||!tasks_live())return 0;
 if(task_gate&&!task_gate())return 0;
 scheduling=1;unsigned ran=0;
 for(unsigned k=0;k<FORTH_TASKS_MAX;++k) {
  unsigned id=(next_task+k)%FORTH_TASKS_MAX;struct task *t=&tasks[id];
  if(t->state==TS_SLEEPING&&(int32_t)(now-t->wake)>=0)t->state=TS_READY;
  if(t->state!=TS_READY)continue;
  task_flush(id);
  if(t->state!=TS_READY)continue;
  if(t->finishing){if(!t->out_count){t->finishing=0;t->state=TS_DONE;task_exited(id);}continue;}
  if(t->out_handle&&t->out_count>=OUT_HIGH)continue; /* consumer is behind: backpressure */
  run_slice(id);++ran;task_flush(id);
 }
 next_task=(next_task+1)%FORTH_TASKS_MAX;scheduling=0;preempt_plan();return ran;
}
/* Plan the next tick steal: a running slice ends after PREEMPT_SLICE_MS; the
 * foreground is interrupted only when a task is ready (every
 * PREEMPT_QUANTUM_MS) or a sleeper's wake time comes, so a foreground with
 * nothing to share runs untouched. */
static void preempt_plan(void){
 if(!preempt_arm)return;
 if(!preempt_on){preempt_arm(0);return;}
 if(running>=0){preempt_arm(PREEMPT_SLICE_MS);return;}
 uint32_t now=now_ms();unsigned delay=interrupt_poll&&executing?PREEMPT_INTERRUPT_MS:0;
 for(unsigned i=0;i<FORTH_TASKS_MAX;++i){
  unsigned d=0;
  if(tasks[i].state==TS_READY)d=PREEMPT_QUANTUM_MS;
  else if(tasks[i].state==TS_SLEEPING){int32_t w=(int32_t)(tasks[i].wake-now);d=w<=0?1u:w>60000?60000u:(unsigned)w;}
  if(d&&(!delay||d<delay))delay=d;
 }
 preempt_arm(delay);
}
void forth_register_preempt(void (*arm)(unsigned delay_ms)){preempt_arm=arm;preempt_clear();preempt_plan();}
void forth_register_tick_source(void (*hook)(void)){tick_source=hook;}
void forth_register_interrupt(int (*poll)(void)){interrupt_poll=poll;}
static int native_preempt(void){fcell f=fvm_pop();if(fvm_error)return fail(vm_message());preempt_on=f!=0;preempt_plan();return 1;}
static int native_critical_begin(void){if(critical==255)return fail("CRITICAL( nested too deep");++critical;return 1;}
static int native_critical_end(void){if(critical)--critical;return 1;}
static int native_preempt_stats(void){
 const char *s="preempt ";for(;*s;++s)out((unsigned char)*s);
 s=preempt_arm?(preempt_on?"on":"off"):"none";for(;*s;++s)out((unsigned char)*s);
 s=" switches ";for(;*s;++s)out((unsigned char)*s);number((fcell)preemptions);
 s="ignored ";for(;*s;++s)out((unsigned char)*s);number((fcell)ticks_ignored);
 s="critical ";for(;*s;++s)out((unsigned char)*s);number(critical);out('\r');out('\n');return 1;
}
void forth_register_clock(uint32_t (*hook)(void)){clock_hook=hook;}
void forth_register_task_gate(int (*gate)(void)){task_gate=gate;}
void forth_register_task_exit(void (*hook)(unsigned)){task_exit_hook=hook;}
void forth_register_task_io(int (*w)(unsigned,unsigned char),int (*r)(unsigned,unsigned char *),int (*g)(unsigned,unsigned)){io_write=w;io_read=r;io_give=g;}
/* Best-effort delivery of buffered redirected output for a task that has
 * already ended (done, failed, killed), before its handles are closed. */
static void task_drain(unsigned id) {
 struct task *t=&tasks[id];if(!t->out_handle||!io_write)return; /* unredirected output stays for TASK-OUT */
 while(t->out_count&&io_write(t->out_handle,t->out[t->out_head])>0){t->out_head=(unsigned short)((t->out_head+1)%FORTH_TASK_OUT);--t->out_count;}
 t->out_dropped+=t->out_count;t->out_count=0;
}
static void task_flush(unsigned id) {
 struct task *t=&tasks[id];if(!t->out_handle||!io_write||(t->state!=TS_READY&&t->state!=TS_SLEEPING))return;
 while(t->out_count){
  int r=io_write(t->out_handle,t->out[t->out_head]);
  if(!r)return;
  if(r<0){t->out_count=0;t->finishing=0;task_fail(t,"broken pipe",TS_FAILED);task_exited(id);return;}
  t->out_head=(unsigned short)((t->out_head+1)%FORTH_TASK_OUT);--t->out_count;
 }
}
/* ( id h -- ) attach a handle as a task's stdin/stdout; the task owns it. */
static int redirect(int output) {
 fcell h=fvm_pop(),id=fvm_pop();if(fvm_error)return fail(vm_message());if(!task_id(id))return 0;
 struct task *t=&tasks[id-1];if(t->state!=TS_READY&&t->state!=TS_SLEEPING)return fail("task is not live");
 if(!h||h>255||!io_give||!io_give((unsigned)h,(unsigned)id))return fail("invalid handle for redirection");
 if(output)t->out_handle=(unsigned char)h;else t->in_handle=(unsigned char)h;return 1;
}
static int native_task_input(void){return redirect(0);}
static int native_task_output(void){return redirect(1);}
void forth_emit(unsigned char c){out(c);}
unsigned forth_current_task(void){return running<0?0u:(unsigned)running+1u;}
int forth_task_info(unsigned id,struct forth_task_info *o) {
 if(!o||!id||id>FORTH_TASKS_MAX)return 0;const struct task *t=&tasks[id-1];
 o->state=t->state;o->xt=t->xt;o->name=t->xt&&t->xt<=count?words[t->xt-1].name:"";o->error=t->error;
 o->depth=t->depth;o->rdepth=t->rdepth;o->ds_peak=t->ds_peak;o->rs_peak=t->rs_peak;
 o->slices=t->slices;o->steps=t->steps;o->output_bytes=t->out_count;o->output_dropped=t->out_dropped;return 1;
}
static int task_id(fcell id){if(!id||id>FORTH_TASKS_MAX)return fail("invalid task id");return 1;}
/* console=1 sends the task's EMIT/./CR output to the shared console (and the
 * VGA view) instead of its ring: for applications such as AUTORUN. */
/* Prepare a free slot without running Forth; returns 1..N or 0. */
static unsigned task_start(fcell xt,int console,unsigned in_handle,unsigned out_handle) {
 if(observer_active||transaction_active||!xt||xt>count)return 0;
 for(unsigned i=0;i<FORTH_TASKS_MAX;++i) {
  struct task *t=&tasks[i];
  if(t->state==TS_READY||t->state==TS_SLEEPING||(int)i==running)continue;
  t->xt=xt;t->depth=t->rdepth=t->ldepth=0;t->ip=entry_ip((unsigned)(xt-1));t->ds_peak=t->rs_peak=0;
  t->wake=t->slices=t->steps=0;t->out_head=t->out_count=0;t->out_dropped=0;t->error[0]=0;t->console=(unsigned char)console;
  t->in_handle=(unsigned char)in_handle;t->out_handle=(unsigned char)out_handle;t->finishing=0;t->state=TS_READY;
  preempt_plan();return i+1;   /* a ready task: arm the foreground quantum now */
 }
 return 0;
}
unsigned forth_task_start(fcell xt,unsigned in_handle,unsigned out_handle){return task_start(xt,0,in_handle,out_handle);}
static int spawn(int console) {
 fcell xt=fvm_pop();if(fvm_error)return fail(vm_message());
 if(observer_active||transaction_active)return fail("SPAWN unavailable here");
 if(!xt||xt>count)return fail("invalid execution token");
 unsigned id=task_start(xt,console,0,0);if(!id)return fail("no free task slot");
 return fvm_push((fcell)id)?1:fail(vm_message());
}
static int native_spawn(void){return spawn(0);}
/* Memory and output helpers through checked VM accessors. */
static int native_fill(void){
 fcell c=fvm_pop(),u=fvm_pop(),a=fvm_pop();if(fvm_error)return fail(vm_message());
 for(fcell i=0;i<u;++i){fvm_write8((a+i)&FVM_MASK,(unsigned char)c);if(fvm_error)return fail(vm_message());}return 1;
}
static int native_move(void){
 fcell u=fvm_pop(),d=fvm_pop(),s=fvm_pop();if(fvm_error)return fail(vm_message());
 if(d<=s)for(fcell i=0;i<u;++i){fvm_write8((d+i)&FVM_MASK,fvm_read8((s+i)&FVM_MASK));if(fvm_error)return fail(vm_message());}
 else for(fcell i=u;i--;){fvm_write8((d+i)&FVM_MASK,fvm_read8((s+i)&FVM_MASK));if(fvm_error)return fail(vm_message());}
 return 1;
}
/* TYPE ( addr u -- ): one trap per string instead of one EMIT per byte. A
 * redirected task stops early when its ring nears full; the rest of the
 * string is retried after the slice (the native leaves addr+k u-k pushed and
 * rewinds to re-execute TYPE). */
static int native_type(void){
 fcell u=fvm_pop(),a=fvm_pop();if(fvm_error)return fail(vm_message());
 for(fcell i=0;i<u;++i){
  unsigned char c=fvm_read8((a+i)&FVM_MASK);if(fvm_error)return fail(vm_message());out(c);
  if(running>=0&&yield_request&&i+1<u){
   if(!fvm_push((a+i+1)&FVM_MASK)||!fvm_push(u-i-1))return fail(vm_message());
   /* Re-run this native: NATIVE op plus its 3-byte id operand. */
   fvm_ip-=selected==FORTH_DIRECT?6:4;return 1;
  }
 }
 return 1;
}
static int native_udot(void){fcell n=fvm_pop();if(fvm_error)return fail(vm_message());unsigned_number(n);out(' ');return 1;}
/* Loop helpers on the VM loop stack (pure VM state; frames are limit,index). */
static int native_j(void){if(fvm_ldepth<2){fvm_error=FE_LOOP_UNDER;return fail(vm_message());}return fvm_push(fvm_loops[(fvm_ldepth-2)*2+1])?1:fail(vm_message());}
static int native_unloop(void){if(!fvm_ldepth){fvm_error=FE_LOOP_UNDER;return fail(vm_message());}--fvm_ldepth;return 1;}
static int native_plus_loop(void){
 /* ( n -- exit? ) exits when the index crosses the limit-1/limit boundary. */
 fcell n=fvm_pop();if(fvm_error)return fail(vm_message());if(!fvm_ldepth){fvm_error=FE_LOOP_UNDER;return fail(vm_message());}
 fcell *f=&fvm_loops[(fvm_ldepth-1)*2];fcell d0=(f[1]-f[0])&FVM_MASK,next=(f[1]+n)&FVM_MASK,d1=(next-f[0])&FVM_MASK;
 int neg0=(d0&0x800000UL)!=0,neg1=(d1&0x800000UL)!=0,exit=(n&0x800000UL)?(!neg0&&neg1):(neg0&&!neg1);
 if(exit)--fvm_ldepth;else f[1]=next;
 return fvm_push(exit?FVM_MASK:0)?1:fail(vm_message());
}
static int native_spawn_console(void){return spawn(1);}
static int native_pause(void) {if(running>=0)yield_request=1;else forth_tasks_run();return 1;}
static int native_ms(void) {
 fcell ms=fvm_pop();if(fvm_error)return fail(vm_message());uint32_t deadline=now_ms()+ms;
 if(running>=0){tasks[running].wake=deadline;tasks[running].state=TS_SLEEPING;yield_request=1;return 1;}
 while((int32_t)(now_ms()-deadline)<0){if(interrupt_poll&&interrupt_poll())return fail("interrupted");forth_tasks_run();}return 1;
}
/* FRAME-WAIT: sleep to the next 60 Hz frame boundary (multiples of 50/3 ms
 * on the task clock), so a render task wakes at a steady rate. */
static int native_frame_wait(void) {
 uint32_t now=now_ms(),deadline=(((now*3u)/50u+1u)*50u+2u)/3u;   /* rounded up: never before the boundary */if((int32_t)(deadline-now)<=0)++deadline;
 if(running>=0){tasks[running].wake=deadline;tasks[running].state=TS_SLEEPING;yield_request=1;return 1;}
 while((int32_t)(now_ms()-deadline)<0){if(interrupt_poll&&interrupt_poll())return fail("interrupted");forth_tasks_run();}return 1;
}
static int native_kill(void) {
 fcell id=fvm_pop();if(fvm_error)return fail(vm_message());if(!task_id(id))return 0;
 struct task *t=&tasks[id-1];if(t->state==TS_READY||t->state==TS_SLEEPING||(int)(id-1)==running)task_fail(t,"killed",TS_KILLED);
 else return 1;
 /* A running task's slice end reports its own exit. */
 if((int)(id-1)==running)yield_request=1;else{t->finishing=0;task_drain((unsigned)(id-1));task_exited((unsigned)(id-1));}return 1;
}
static int native_state(void) {
 fcell id=fvm_pop();if(fvm_error)return fail(vm_message());if(!task_id(id))return 0;
 return fvm_push(tasks[id-1].state)?1:fail(vm_message());
}
static void hex_text(const char *p){static const char d[]="0123456789ABCDEF";if(!*p){print("-");return;}while(*p){unsigned char c=(unsigned char)*p++;platform_console_putc((unsigned char)d[c>>4]);platform_console_putc((unsigned char)d[c&15]);}}
/* Print the source token compiled at code offset of xt (current engine). */
static void print_source_token(fcell xt,fcell offset,enum forth_engine engine){
 if(!xt||xt>count){print("?");return;}
 unsigned w=(unsigned)(xt-1);int best=-1;
 for(unsigned i=0;i<map_used;++i)if(source_map[i].word==w){unsigned at=engine==FORTH_DIRECT?source_map[i].direct:source_map[i].token;if(at<=offset)best=(int)i;}
 if(best<0){print("-");return;}
 const char *p=source_space+words[w].source+source_map[best].source,*end=source_space+words[w].source+words[w].length;
 while(p<end&&*p!=' ')platform_console_putc((unsigned char)*p++);
}
static void trace_step(void) {
 fcell offset;fcell xt=code_xt(fvm_ip,&offset);--trace_left;++trace_steps;
 print("step ");decimal_number(trace_steps);print(" word ");print(xt&&xt<=count?words[xt-1].name:"?");print("+");decimal_number(offset);
 print(" src ");print_source_token(xt,offset,selected);
 print(" depth ");decimal_number(fvm_depth);print(" rdepth ");decimal_number(fvm_rdepth);
 if(fvm_depth&&fvm_depth<=FVM_DS){print(" top ");number(fvm_ds[fvm_depth-1]);}print("\r\n");
}
/* Run one word with a budget of one instruction per dispatch, reporting the
 * next instruction's word/offset and stacks before each. Uses the exact
 * budget-exhaustion resume the task scheduler relies on (all engines). */
static int trace_word(unsigned n,uint32_t limit) {
 tracing=1;trace_left=limit;trace_steps=0;
 fvm_error=0;fvm_rdepth=fvm_ldepth=0;fvm_budget=1;fvm_ip=entry_ip(n);trace_step();
 ++executing;int ok=dispatch();--executing;tracing=0;
 print("trace ");print(ok?"returned":"failed");print(" steps ");decimal_number(trace_steps);print("\r\n");return ok;
}
static void crash_report(void) {
 print("last-error serial ");decimal_number(crash.serial);if(!crash.serial){print("\r\n");return;}
 print(" engine ");print(crash.engine==FORTH_DIRECT?"DIRECT":crash.engine==FORTH_TOKEN?"TOKEN":"C");
 print(" stale ");decimal_number(crash.catalog!=catalog_revision);print(" base ");decimal_number(crash.base);
 print(" message ");hex_text(crash.message);print(" ip ");decimal_number(crash.ip);
 print(" word ");hex_text(crash.xt&&crash.xt<=count?words[crash.xt-1].name:"");print(" offset ");decimal_number(crash.offset);
 print(" src ");print_source_token(crash.catalog!=catalog_revision?0:crash.xt,crash.offset,crash.engine);
 print(" ds-depth ");decimal_number(crash.ds_depth);print(" rdepth ");decimal_number(crash.rdepth);print(" ldepth ");decimal_number(crash.ldepth);
 print(" ds");for(fcell i=0;i<CRASH_DS&&i<crash.ds_depth;++i){print(" ");decimal_number(crash.ds[i]);}print("\r\n");
 for(unsigned i=0;i<crash.frames;++i){
  print("frame ");decimal_number(i);print(" word ");hex_text(crash.frame_xt[i]&&crash.frame_xt[i]<=count?words[crash.frame_xt[i]-1].name:"");
  print(" offset ");decimal_number(crash.frame_offset[i]);print(" src ");print_source_token(crash.catalog!=catalog_revision?0:crash.frame_xt[i],crash.frame_offset[i],crash.engine);print("\r\n");
 }
}
static void task_report(void) {
 static const char *const names[]={"free","ready","sleeping","done","failed","killed"};
 for(unsigned i=0;i<FORTH_TASKS_MAX;++i) {
  const struct task *t=&tasks[i];
  print("task ");decimal_number(i+1);print(" state ");print(names[t->state]);print(" xt ");decimal_number(t->xt);
  print(" name ");hex_text(t->xt&&t->xt<=count?words[t->xt-1].name:"");print(" slices ");decimal_number(t->slices);
  print(" steps ");decimal_number(t->steps);print(" depth ");decimal_number(t->depth);print(" rdepth ");decimal_number(t->rdepth);
  print(" ds-peak ");decimal_number(t->ds_peak);print(" rs-peak ");decimal_number(t->rs_peak);
  print(" out ");decimal_number(t->out_count);print(" dropped ");decimal_number(t->out_dropped);print(" error ");hex_text(t->error);print("\r\n");
 }
}
static int task_output(fcell id) {
 if(!task_id(id))return 0;struct task *t=&tasks[id-1];static const char d[]="0123456789ABCDEF";
 print("task-out ");decimal_number(id);print(" hex ");if(!t->out_count)print("-");
 while(t->out_count){unsigned char c=t->out[t->out_head];t->out_head=(unsigned short)((t->out_head+1)%FORTH_TASK_OUT);--t->out_count;platform_console_putc((unsigned char)d[c>>4]);platform_console_putc((unsigned char)d[c&15]);}
 print(" dropped ");decimal_number(t->out_dropped);print("\r\n");return 1;
}
int forth_execute(const char *name) {
 if(!name||length(name)>NAME_MAX)return fail("invalid word name");
 fcell xt=forth_find_xt(name);return xt?execute_index((unsigned)(xt-1)):unknown(name);
}
fcell forth_find_xt(const char *name) {
 char key[NAME_MAX+1];unsigned i=0;
 if(!name)return 0;
 while(name[i]){if(i==NAME_MAX)return 0;key[i]=upper(name[i]);++i;}key[i]=0;
 int n=find(key);return n<0?0:(fcell)(n+1);
}
const char *forth_xt_name(fcell xt){return xt&&xt<=count?words[xt-1].name:NULL;}
int forth_register_command(int (*hook)(const char *,const char **)) {
 if(transaction_active)return fail("command registration during module transaction");
 outer_hook=hook;return 1;
}
void forth_register_idle(void (*hook)(int)) {idle_hook=hook;}
void forth_register_line_mode(void (*hook)(int)) {line_mode_hook=hook;}
int forth_register_native(const char *name,int (*callback)(void),unsigned flags) {
 char key[NAME_MAX+1];unsigned i=0;
 if(transaction_active)return fail("native registration during module transaction");
 if(!name||!callback||native_count==NATIVES_MAX)return fail("native registry full or invalid");
 while(name[i]){if(i==NAME_MAX)return fail("invalid name");key[i]=upper(name[i]);++i;}key[i]=0;
 if(!begin_word(key,WK_NATIVE))return 0;
 words[count].aux=native_count;
 if(!op(F_NATIVE)||!operand(native_count,native_count)||!op(F_EXIT))return 0;
 natives[native_count].callback=callback;natives[native_count++].flags=flags;
 return publish();
}
static int next_token(const char **p,char *buf) {
 for(;;) {
  while(**p&&(**p==' '||**p=='\t'||**p=='\r'||**p=='\n'))++*p;
  if(**p=='\\') {while(**p&&**p!='\n')++*p;continue;}
  if(**p=='(') {while(**p&&**p!=')')++*p;if(**p)++*p;continue;}break;
 }
 if(!**p)return 0;
 unsigned n=0;while(**p&&**p!=' '&&**p!='\t'&&**p!='\r'&&**p!='\n') {
  if(n==NAME_MAX){fail("token too long");return -1;}
  buf[n++]=upper(*(*p)++);
 }
 buf[n]=0;return 1;
}
static int need_token(const char **p,char *buf) {return next_token(p,buf)>0?1:fail("missing name");}
static int parse_number(const char *s,fcell *value) {
 int negative=0;fcell n=0,radix=fvm_read_cell(base_address);
 if(*s=='-'||*s=='+'){negative=*s=='-';++s;}
 if(*s=='$'){radix=16;++s;}else if(*s=='%'){radix=2;++s;}
 if(!*s||radix<2||radix>36)return 0;
 while(*s) {
  unsigned d=*s>='0'&&*s<='9'?(unsigned)(*s-'0'):*s>='A'&&*s<='Z'?(unsigned)(*s-'A'+10):99u;
  if(d>=radix)return 0;
  if(n>(FVM_MASK-d)/radix)return -1;
  n=n*radix+d;++s;
 }
 *value=negative?((~n)+1)&FVM_MASK:n;return 1;
}
static int control_push(unsigned kind,size_t t,size_t d) {
 if(control_used==CONTROL_MAX)return fail("compiler control stack full");
 controls[control_used].kind=(unsigned char)kind;controls[control_used].token=t;controls[control_used++].direct=d;return 1;
}
static void patch(struct control c) {
 put_cell(token_code+c.token,address(token_code+token_used));
 put_cell(direct_code+c.direct,address(direct_code+direct_used));
}
static unsigned loop_depth(void){unsigned n=0;for(unsigned i=0;i<control_used;++i)if(controls[i].kind==CS_DO)++n;return n;}
static int leave_push(unsigned depth){
 if(leave_used==LEAVES_MAX)return fail("too many LEAVE/?DO exits");
 leaves[leave_used].kind=(unsigned char)depth;leaves[leave_used].token=token_used;leaves[leave_used].direct=direct_used;++leave_used;
 return operand(0,0);
}
static void leaves_resolve(unsigned depth){
 unsigned kept=0;for(unsigned i=0;i<leave_used;++i){if(leaves[i].kind==depth)patch(leaves[i]);else leaves[kept++]=leaves[i];}leave_used=kept;
}
static int compile_named(const char *name){int n=find(name);return n<0?fail("loop helper missing"):compile_word(n);}
static int control_word(const char *s) {
 struct control c;
 if(current<0)return fail("compile-only word");
 if(equal(s,"LEAVE")) {
  unsigned depth=loop_depth();if(!depth)return fail("LEAVE needs DO");
  return compile_named("UNLOOP")&&op(F_BRANCH)&&leave_push(depth);
 }
 if(equal(s,"?DO")) {
  /* 2DUP = IF 2DROP skip-loop THEN DO ... : the skip is resolved at LOOP. */
  if(!compile_named("2DUP")||!op(F_EQ)||!op(F_ZBRANCH))return 0;
  size_t t=token_used,d=direct_used;if(!operand(0,0))return 0;
  if(!compile_named("2DROP")||!op(F_BRANCH)||!leave_push(loop_depth()+1))return 0;
  struct control enter={CS_IF,t,d};patch(enter);
  if(!op(F_DO))return 0;return control_push(CS_DO,token_used,direct_used);
 }
 if(equal(s,"IF")||equal(s,"WHILE")) {
  if(equal(s,"WHILE")&&(!control_used||controls[control_used-1].kind!=CS_BEGIN))return fail("WHILE needs BEGIN");
  if(!op(F_ZBRANCH))return 0;
  if(!control_push(equal(s,"IF")?CS_IF:CS_WHILE,token_used,direct_used))return 0;
  return operand(0,0);
 }
 if(equal(s,"BEGIN"))return control_push(CS_BEGIN,token_used,direct_used);
 if(equal(s,"DO")){if(!op(F_DO))return 0;return control_push(CS_DO,token_used,direct_used);}
 if(!control_used)return fail("unbalanced control flow");
 c=controls[--control_used];
 if(equal(s,"ELSE")) {
  if(c.kind!=CS_IF)return fail("ELSE needs IF");
  if(!op(F_BRANCH))return 0;
  size_t t=token_used,d=direct_used;if(!operand(0,0))return 0;patch(c);return control_push(CS_ELSE,t,d);
 }
 if(equal(s,"THEN")){if(c.kind!=CS_IF&&c.kind!=CS_ELSE)return fail("THEN needs IF");patch(c);return 1;}
 if(equal(s,"REPEAT")) {
  if(c.kind!=CS_WHILE||!control_used||controls[control_used-1].kind!=CS_BEGIN)return fail("REPEAT needs BEGIN WHILE");
  struct control begin=controls[--control_used];
  if(!op(F_BRANCH)||!operand(address(token_code+begin.token),address(direct_code+begin.direct)))return 0;patch(c);return 1;
 }
 if(equal(s,"LOOP")||equal(s,"+LOOP")) {
  if(c.kind!=CS_DO)return fail("LOOP needs DO");
  unsigned depth=loop_depth()+1;
  int ok=equal(s,"LOOP")?op(F_LOOP)&&operand(address(token_code+c.token),address(direct_code+c.direct))
   :compile_named("(+LOOP)")&&op(F_ZBRANCH)&&operand(address(token_code+c.token),address(direct_code+c.direct));
  if(ok)leaves_resolve(depth);return ok;
 }
 if(c.kind!=CS_BEGIN)return fail("loop needs BEGIN");
 return op(equal(s,"UNTIL")?F_ZBRANCH:F_BRANCH)&&operand(address(token_code+c.token),address(direct_code+c.direct));
}
static int defining(const char *kind,const char *name) {
 unsigned k=equal(kind,"CONSTANT")?WK_CONSTANT:equal(kind,"DEFER")?WK_DEFER:WK_VARIABLE;
 fcell value=0;
 if(k==WK_CONSTANT){value=fvm_pop();if(fvm_error)return fail(vm_message());}
 if(!begin_word(name,k))return 0;
 if(k==WK_CONSTANT)words[count].aux=value;
 else {
  if(DATA_BYTES-data_used<3)return fail("data space full");
  words[count].aux=address(data_space+data_used);data_used+=3;fvm_write_cell(words[count].aux,0);
 }
 if(k==WK_DEFER) {if(!op(F_DEFER)||!operand(words[count].aux,words[count].aux))return 0;}
 else if(!literal(words[count].aux))return 0;
 if(!op(F_EXIT))return 0;
 return publish();
}
/* CREATE: a variable-like word naming the next data address, allocating
 * nothing; ALLOT/,/C, then reserve its body. */
static int create_word(const char *name) {
 if(!begin_word(name,WK_VARIABLE))return 0;
 words[count].aux=address(data_space+data_used);
 if(!literal(words[count].aux)||!op(F_EXIT))return 0;
 return publish();
}
static void show_stack(void) {
 print("<");unsigned_number(fvm_depth);print("> ");
 for(fcell i=0;i<fvm_depth;++i)number(fvm_ds[i]);print("\r\n");
}
static void show_word(int n) {
 if(words[n].length) {
  const char *p=source_space+words[n].source;
  for(unsigned i=0;i<words[n].length;++i)platform_console_putc((unsigned char)p[i]);
 }else {print(words[n].kind==WK_PRIMITIVE?"primitive ":"data word ");print(words[n].name);}
 print("\r\n");
 print("word ");print(words[n].name);print(" xt ");decimal_number((fcell)(n+1));
 print(" kind ");decimal_number(words[n].kind);print(" token ");decimal_number(words[n].token);
 print(" direct ");decimal_number(words[n].direct);print(" token-bytes ");decimal_number(words[n].token_length);
 print(" direct-bytes ");decimal_number(words[n].direct_length);print("\r\n");
 if(words[n].origin) {
  const char *module,*version;uint32_t crc;
  if(forth_module_origin(words[n].origin,&module,&version,&crc)) {
   print("module ");print(module);print(" version ");print(version);print("\r\n");
  }
 }
}
int forth_eval(const char *source) {
 const char *p=source;char s[NAME_MAX+1],name[NAME_MAX+1];int got;
 error_text[0]=0;fvm_error=0;
 while((got=next_token(&p,s))>0) {
  if(transaction_active&&(equal(s,"TRACE")||equal(s,"LAST-ERROR")||equal(s,"TASKS")||equal(s,"TASK-OUT")||equal(s,"ABORT")||equal(s,"ENGINE")||equal(s,"BENCH")||equal(s,"SYNC")||equal(s,"WORDS")||equal(s,".S")||equal(s,"SEE")||equal(s,"RESOURCES")))return fail("interactive tool disabled during module transaction");
  if(current<0&&!transaction_active&&outer_hook) {
   int handled=outer_hook(s,&p);if(handled<0)return 0;if(handled)continue;
  }
  /* Harness framing consumes no VM values and never enters a definition. */
  if(equal(s,"SYNC")) {
   if(!need_token(&p,name))return 0;
   print("@");print(name);print("\r\n");continue;
  }
  if(equal(s,"ABORT")){rollback();fvm_reset();return 1;}
  if(equal(s,":")) {
   if(!need_token(&p,name)||!begin_word(name,WK_COLON))return 0;
   if(!source_add(":")||!source_add(name))return 0;continue;
  }
  if(current>=0&&map_used<SOURCE_MAP_MAX){
   source_map[map_used].word=(unsigned short)current;source_map[map_used].token=(unsigned short)(token_used-mark_token);
   source_map[map_used].direct=(unsigned short)(direct_used-mark_direct);source_map[map_used].source=(unsigned short)(source_used-words[current].source);++map_used;
  }
  if(current>=0&&!source_add(s))return 0;
  if(equal(s,"S\"")){if(!string_literal(&p))return 0;continue;}
  if(equal(s,".\"")) {
   /* ." text" = S" text" TYPE; TYPE is library Forth (kernel prelude). */
   int type=find("TYPE");if(type<0)return fail(".\" requires TYPE");
   if(!string_literal(&p))return 0;
   if(current>=0){if(!compile_word(type))return 0;}else if(!execute_index((unsigned)type))return 0;continue;
  }
  if(equal(s,"CHAR")||equal(s,"[CHAR]")) {
   if(equal(s,"CHAR")&&current>=0)return fail("use [CHAR] in definitions");
   /* Read the next token raw: the tokenizer case-folds names. */
   while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')++p;
   if(!*p)return fail("missing name");
   unsigned char ch=(unsigned char)*p;unsigned n=0;
   while(*p&&*p!=' '&&*p!='\t'&&*p!='\r'&&*p!='\n'){if(n<NAME_MAX)name[n++]=*p;++p;}name[n]=0;
   if(current>=0&&!source_add(name))return 0;
   if(current>=0){if(!literal(ch))return 0;}else if(!fvm_push(ch))return fail(vm_message());
   continue;
  }
  if(equal(s,"CREATE")||equal(s,"ALLOT")||equal(s,"HERE")||equal(s,",")||equal(s,"C,")) {
   /* Data-space words act at interpretation time only: VIOS definitions
    * cannot grow managed data at run time (transactions and overlays
    * account for data_used per definition). */
   if(current>=0)return fail("data-space word inside definition");
   if(equal(s,"CREATE")){if(!need_token(&p,name)||!create_word(name))return 0;continue;}
   if(equal(s,"HERE")){if(!fvm_push(address(data_space+data_used)))return fail(vm_message());continue;}
   fcell n=fvm_pop();if(fvm_error)return fail(vm_message());
   size_t need=equal(s,"ALLOT")?(size_t)n:equal(s,",")?3u:1u;
   if(equal(s,"ALLOT")&&(n&0x800000UL))return fail("negative ALLOT");
   if(need>DATA_BYTES-data_used)return fail("data space full");
   fcell a=address(data_space+data_used);
   if(equal(s,",")){fvm_write_cell(a,n);}else if(equal(s,"C,")){fvm_write8(a,(unsigned char)n);}else for(size_t i=0;i<need;++i)fvm_write8(a+(fcell)i,0);
   if(fvm_error)return fail(vm_message());
   data_used+=need;continue;
  }
  if(equal(s,";")) {
   if(current<0)return fail("unexpected semicolon");
   if(control_used)return fail("unbalanced control flow");
   if(!op(F_EXIT)||!publish())return 0;continue;
  }
  if(equal(s,"VARIABLE")||equal(s,"CONSTANT")||equal(s,"DEFER")) {
   if(current>=0)return fail("defining word inside definition");
   if(!need_token(&p,name)||!defining(s,name))return 0;continue;
  }
  if(equal(s,"IF")||equal(s,"ELSE")||equal(s,"THEN")||equal(s,"BEGIN")||equal(s,"UNTIL")||equal(s,"AGAIN")||equal(s,"WHILE")||equal(s,"REPEAT")||equal(s,"DO")||equal(s,"LOOP")||equal(s,"+LOOP")||equal(s,"?DO")||equal(s,"LEAVE")) {
   if(!control_word(s))return 0;continue;
  }
  if(equal(s,"RECURSE")) {if(current<0)return fail("RECURSE outside definition");if(!compile_word(current))return 0;continue;}
  if(equal(s,"EXIT")&&current>=0){if(!op(F_EXIT))return 0;continue;}
  if(equal(s,"'")||equal(s,"[']")||equal(s,"IS")) {
   if(!need_token(&p,name))return 0;
   if(current>=0&&!source_add(name))return 0;
   int n=find(name);if(n<0)return unknown(name);
   if(equal(s,"IS")) {
    if(words[n].kind!=WK_DEFER)return fail("IS requires deferred word");
    if(current>=0){if(!literal(words[n].aux)||!op(F_STORE))return 0;}
    else {fcell xt=fvm_pop();if(fvm_error)return fail(vm_message());if(!xt||xt>count)return fail("invalid execution token");fvm_write_cell(words[n].aux,xt);}
   }else {
    if(equal(s,"'")&&current>=0)return fail("use ['] in definitions");
    if(current>=0){if(!literal((fcell)(n+1)))return 0;}else if(!fvm_push((fcell)(n+1)))return fail(vm_message());
   }
   continue;
  }
#ifdef __ez80__
  if(equal(s,"BENCH")) {
   extern void forth_benchmark(void);
   if(current>=0)return fail("interactive tool inside definition");
   forth_benchmark();continue;
  }
#endif
  if(equal(s,"TRACE")) {
   if(current>=0)return fail("interactive tool inside definition");
   char count_text[NAME_MAX+1];fcell limit;
   if(!need_token(&p,name)||!need_token(&p,count_text))return 0;
   int n=find(name);if(n<0)return unknown(name);
   if(parse_number(count_text,&limit)<=0||!limit||limit>100000)return fail("TRACE name n needs 1..100000 steps");
   if(!trace_word((unsigned)n,limit))return 0;continue;
  }
  if(equal(s,"TASKS")||equal(s,"TASK-OUT")||equal(s,"LAST-ERROR")||equal(s,"TASK-SLICE")) {
   if(current>=0)return fail("interactive tool inside definition");
   if(equal(s,"TASKS")){print("task-slice ");decimal_number(task_slice);print("\r\n");task_report();continue;}
   if(equal(s,"TASK-SLICE")){fcell n;if(!need_token(&p,name))return 0;if(parse_number(name,&n)<=0||n<64||n>65535)return fail("TASK-SLICE needs 64..65535");task_slice=n;continue;}
   if(equal(s,"LAST-ERROR")){crash_report();continue;}
   if(!need_token(&p,name))return 0;fcell id;if(parse_number(name,&id)<=0)return fail("task id required");
   if(!task_output(id))return 0;continue;
  }
  if(equal(s,"WORDS")||equal(s,".S")||equal(s,"SEE")||equal(s,"RESOURCES")||equal(s,"ENGINE")) {
   if(current>=0)return fail("interactive tool inside definition");
   if(equal(s,"WORDS")){for(unsigned i=0;i<count;++i){print(" ");print(words[i].name);}print(" \r\n");}
   else if(equal(s,".S"))show_stack();
   else if(equal(s,"SEE")){if(!need_token(&p,name))return 0;int n=find(name);if(n<0)return unknown(name);show_word(n);}
   else if(equal(s,"RESOURCES"))forth_report();
   else {
    if(!need_token(&p,name))return 0;
    if(tasks_live())return fail("ENGINE refused while background tasks are live");
    if(equal(name,"C"))selected=FORTH_REFERENCE;
#ifdef __ez80__
    else if(equal(name,"TOKEN"))selected=FORTH_TOKEN;
    else if(equal(name,"DIRECT"))selected=FORTH_DIRECT;
#endif
    else return fail("engine unavailable");
   }
   continue;
  }
  int n=find(s);
  if(n>=0){if(current>=0){if(!compile_word(n))return 0;}else if(!execute_index((unsigned)n))return 0;continue;}
  fcell value;int numeric=parse_number(s,&value);
  if(numeric<0)return fail("number outside 24-bit range");
  if(!numeric)return unknown(s);
  if(current>=0){if(!literal(value))return 0;}else if(!fvm_push(value))return fail(vm_message());
 }
 return got<0?0:1;
}
void forth_init(void) {
 static const struct {const char *name;unsigned char op;} primitives[]={
 {"DUP",F_DUP},{"DROP",F_DROP},{"SWAP",F_SWAP},{"OVER",F_OVER},{"+",F_ADD},{"-",F_SUB},{"*",F_MUL},{"/",F_DIV},{"MOD",F_MOD},
 {"AND",F_AND},{"OR",F_OR},{"XOR",F_XOR},{"INVERT",F_INVERT},{"=",F_EQ},{"<",F_LT},{"U<",F_ULT},{"0=",F_ZEROEQ},
 {"@",F_FETCH},{"!",F_STORE},{"C@",F_CFETCH},{"C!",F_CSTORE},{"EMIT",F_EMIT},{".",F_DOT},{"CR",F_CR},{"KEY",F_KEY},
 {">R",F_TOR},{"R>",F_RFROM},{"R@",F_RFETCH},{"I",F_I},{"EXECUTE",F_EXECUTE},{"LSHIFT",F_LSHIFT},{"RSHIFT",F_RSHIFT},{"DEPTH",F_DEPTH}};
 token_used=direct_used=data_used=source_used=control_used=0;count=0;current=-1;map_used=mark_map=0;
 native_count=origin_id=0;outer_hook=NULL;idle_hook=NULL;transaction_active=have_activation=0;
 observer_active=0;catalog_changed();
 forth_modules_reset();transport_crc=0;
 for(unsigned i=0;i<HASH_BUCKETS;++i)buckets[i]=0;
 selected=FORTH_REFERENCE;fvm_reset();fvm_memory_reset();
#ifndef __ez80__
 fvm_region(0x10000,TOKEN_BYTES,token_code);fvm_region(0x20000,DIRECT_BYTES,direct_code);fvm_region(0x40000,DATA_BYTES,data_space);
#endif
 for(unsigned i=0;i<sizeof(primitives)/sizeof(primitives[0]);++i) {
  begin_word(primitives[i].name,WK_PRIMITIVE);words[count].opcode=primitives[i].op;
  op(primitives[i].op);op(F_EXIT);publish();
 }
 defining("VARIABLE","BASE");base_address=words[count-1].aux;fvm_write_cell(base_address,10);
 for(unsigned i=0;i<FORTH_TASKS_MAX;++i){tasks[i].state=TS_FREE;tasks[i].console=tasks[i].in_handle=tasks[i].out_handle=tasks[i].finishing=0;tasks[i].xt=0;tasks[i].slices=tasks[i].steps=0;tasks[i].out_count=0;tasks[i].out_dropped=0;tasks[i].error[0]=0;tasks[i].depth=tasks[i].rdepth=0;tasks[i].ds_peak=tasks[i].rs_peak=0;}
 running=-1;scheduling=yield_request=0;next_task=0;
 forth_register_native("J",native_j,FORTH_NATIVE_PURE);forth_register_native("UNLOOP",native_unloop,FORTH_NATIVE_PURE);
 forth_register_native("(+LOOP)",native_plus_loop,FORTH_NATIVE_PURE);
 forth_register_native("FILL",native_fill,FORTH_NATIVE_PURE);forth_register_native("MOVE",native_move,FORTH_NATIVE_PURE);
 forth_register_native("U.",native_udot,FORTH_NATIVE_TASK);forth_register_native("TYPE",native_type,FORTH_NATIVE_TASK);
 forth_register_native("SPAWN",native_spawn,FORTH_NATIVE_TASK);forth_register_native("SPAWN-CON",native_spawn_console,FORTH_NATIVE_TASK);forth_register_native("PAUSE",native_pause,FORTH_NATIVE_TASK);
 forth_register_native("MS",native_ms,FORTH_NATIVE_TASK);forth_register_native("TASK-KILL",native_kill,FORTH_NATIVE_TASK);
 forth_register_native("TASK-STATE",native_state,FORTH_NATIVE_TASK);
 critical=0;preemptions=ticks_ignored=0;preempt_clear();
 forth_register_native("PREEMPT",native_preempt,FORTH_NATIVE_TASK);forth_register_native("PREEMPT-STATS",native_preempt_stats,FORTH_NATIVE_TASK);
 forth_register_native("FRAME-WAIT",native_frame_wait,FORTH_NATIVE_TASK);
 forth_register_native("CRITICAL(",native_critical_begin,FORTH_NATIVE_TASK);forth_register_native(")CRITICAL",native_critical_end,FORTH_NATIVE_TASK);
 forth_register_native("TASK-INPUT",native_task_input,FORTH_NATIVE_TASK);forth_register_native("TASK-OUTPUT",native_task_output,FORTH_NATIVE_TASK);
 forth_eval(": 1+ 1 + ; : 1- 1 - ; : NEGATE 0 SWAP - ; : 2DUP OVER OVER ; : 2DROP DROP DROP ; : CELLS 3 * ; : CELL+ 3 + ; : TRUE -1 ; : FALSE 0 ; : HEX 16 BASE ! ; : DECIMAL 10 BASE ! ;");
}
void forth_set_engine(enum forth_engine engine) {selected=engine;}
void forth_transport_crc(int required) {transport_crc=(unsigned char)(required!=0);}
enum forth_engine forth_engine(void) {return selected;}
const char *forth_last_error(void) {return error_text;}
int forth_compiling(void) {return current>=0;}
size_t forth_code_bytes(enum forth_engine engine) {return engine==FORTH_DIRECT?direct_used:token_used;}
void forth_report(void) {
 print("engine ");print(selected==FORTH_DIRECT?"DIRECT":selected==FORTH_TOKEN?"TOKEN":"C");
 print(" words ");decimal_number(count);print("/");decimal_number(WORDS_MAX);print(" token-bytes ");decimal_number((fcell)token_used);
 print(" direct-bytes ");decimal_number((fcell)direct_used);print(" data-bytes ");decimal_number((fcell)data_used);
 print(" source-bytes ");decimal_number((fcell)source_used);print(" data-peak ");decimal_number(fvm_ds_peak);
 print(" return-peak ");decimal_number(fvm_rs_peak);print("\r\n");
}
fcell forth_get_base(void) {return fvm_read_cell(base_address);}
void forth_set_base(fcell radix) {if(radix>=2&&radix<=36)fvm_write_cell(base_address,radix);}

uint32_t forth_catalog_revision(void) {return catalog_revision;}
unsigned forth_word_count(void) {return count;}
int forth_inspect_word(unsigned index,struct forth_word_info *out) {
 if(!out||index>=count)return 0;
 const struct word *w=&words[index];
 out->name=w->name;out->source=source_space+w->source;
 out->source_length=w->length;out->kind=w->kind;out->origin=w->origin;
 out->xt=(fcell)(index+1);out->token_address=w->token;out->direct_address=w->direct;
 out->token_bytes=w->token_length;out->direct_bytes=w->direct_length;
 out->module=out->module_version=out->target_name=NULL;out->module_crc32=0;
 out->value=out->target_xt=0;out->value_valid=0;
 if(w->origin)forth_module_origin(w->origin,&out->module,&out->module_version,&out->module_crc32);
 if(w->kind==WK_CONSTANT){out->value=w->aux;out->value_valid=1;}
 else if(w->kind==WK_VARIABLE||w->kind==WK_DEFER) {
  fcell saved_error=fvm_error,base=address(data_space);
  /* Only compiler-owned, fully allocated managed cells may be read. */
  if(w->aux>=base&&data_used>=3&&(size_t)(w->aux-base)<=data_used-3) {
   fvm_error=0;out->value=fvm_read_cell(w->aux);out->value_valid=!fvm_error;
  }
  fvm_error=saved_error;
  if(w->kind==WK_DEFER&&out->value_valid) {
   out->target_xt=out->value;
   if(out->target_xt&&out->target_xt<=count)out->target_name=words[out->target_xt-1].name;
  }
 }
 return 1;
}
void forth_inspect_resources(struct forth_resources *out) {
 if(!out)return;
 out->words=count;out->words_max=WORDS_MAX;
 out->token_bytes=token_used;out->token_capacity=TOKEN_BYTES;
 out->direct_bytes=direct_used;out->direct_capacity=DIRECT_BYTES;
 out->data_bytes=data_used;out->data_capacity=DATA_BYTES;
 out->source_bytes=source_used;out->source_capacity=SOURCE_BYTES;out->engine=selected;
}
static void observer_error(char *out,size_t capacity,const char *message) {
 if(!out||!capacity)return;
 size_t i=0;while(i+1<capacity&&message[i]){out[i]=message[i];++i;}out[i]=0;
}
int forth_observe(const char *name,fcell budget,char *error,size_t capacity) {
 if(observer_active||transaction_active||current>=0||fvm_rdepth||fvm_ldepth) {
  observer_error(error,capacity,"observer requires idle VM");return 0;
 }
 if(!budget||budget>FVM_MASK) {
  observer_error(error,capacity,"invalid observer budget");return 0;
 }
 for(unsigned i=0;i<FVM_DS;++i)observer_before.stack[i]=fvm_ds[i];
 observer_before.depth=fvm_depth;observer_before.ip=fvm_ip;
 observer_before.error=fvm_error;observer_before.budget=fvm_budget;
 observer_before.ds_peak=fvm_ds_peak;observer_before.rs_peak=fvm_rs_peak;
 observer_before.engine=selected;observer_before.base=fvm_read_cell(base_address);
 copy(observer_before.error_text,error_text);
 observer_active=1;observer_budget=budget;fvm_depth=0;error_text[0]=0;
 int ok=forth_execute(name);
 if(ok&&fvm_depth) {copy(error_text,"observer leaves stack values");ok=0;}
 observer_error(error,capacity,ok?"":error_text);
 fvm_write_cell(base_address,observer_before.base);
 for(unsigned i=0;i<FVM_DS;++i)fvm_ds[i]=observer_before.stack[i];
 fvm_depth=observer_before.depth;fvm_ip=observer_before.ip;
 fvm_error=observer_before.error;fvm_budget=observer_before.budget;
 fvm_ds_peak=observer_before.ds_peak;fvm_rs_peak=observer_before.rs_peak;
 fvm_rdepth=fvm_ldepth=0;selected=observer_before.engine;
 copy(error_text,observer_before.error_text);observer_active=0;return ok;
}

static void checkpoint_take(struct checkpoint *s) {
 s->token=token_used;s->direct=direct_used;s->data=data_used;s->source=source_used;s->count=count;s->map=map_used;
 for(unsigned i=0;i<HASH_BUCKETS;++i)s->buckets[i]=buckets[i];
 for(unsigned i=0;i<DATA_BYTES;++i)s->data_bytes[i]=data_space[i];
 s->depth=fvm_depth;s->ip=fvm_ip;s->error=fvm_error;s->budget=fvm_budget;
 s->ds_peak=fvm_ds_peak;s->rs_peak=fvm_rs_peak;s->engine=selected;
 s->base=fvm_read_cell(base_address);
 for(unsigned i=0;i<FVM_DS;++i)s->stack[i]=fvm_ds[i];
}
static void checkpoint_restore(const struct checkpoint *s,int dictionary,int data) {
 if(dictionary) {
  token_used=s->token;direct_used=s->direct;data_used=s->data;source_used=s->source;count=s->count;map_used=s->map;
  for(unsigned i=0;i<HASH_BUCKETS;++i)buckets[i]=s->buckets[i];
 }
 if(data)for(unsigned i=0;i<DATA_BYTES;++i)data_space[i]=s->data_bytes[i];
 current=-1;control_used=0;leave_used=0;fvm_rdepth=fvm_ldepth=0;
 fvm_depth=s->depth;fvm_ip=s->ip;fvm_error=s->error;fvm_budget=s->budget;
 fvm_ds_peak=s->ds_peak;fvm_rs_peak=s->rs_peak;selected=s->engine;
 for(unsigned i=0;i<FVM_DS;++i)fvm_ds[i]=s->stack[i];
}
static int transaction_bindings_valid(void) {
 fcell base=address(data_space);
 for(unsigned i=0;i<count;++i)if(words[i].kind==WK_DEFER) {
  fcell a=words[i].aux,n=fvm_read_cell(a);
  if(fvm_error||n>count)return fail("invalid deferred service binding");
  if(!n&&i<transaction_before.count) {
   if(a<base||a-base>DATA_BYTES-3)return fail("invalid deferred service address");
   size_t offset=(size_t)(a-base);
   if(transaction_before.data_bytes[offset]||transaction_before.data_bytes[offset+1]||transaction_before.data_bytes[offset+2])return fail("cannot unbind an active service during module activation");
  }
 }
 return 1;
}
int forth_transaction_eval(const char *source,const char *test_word,unsigned origin,int commit) {
 char saved_error[96];int ok=0;
 if(transaction_active||current>=0||fvm_rdepth||fvm_ldepth)return forth_transaction_error("module transaction requires idle VM");
 if(!source)return forth_transaction_error("missing module source");
 checkpoint_take(&transaction_before);
 if(!fvm_overlay_begin(address(data_space),DATA_BYTES,data_space))return fail("module overlay unavailable");
 transaction_active=1;transaction_budget=1000000UL;origin_id=origin;selected=FORTH_REFERENCE;
 fvm_reset();
 /* Source packages start in decimal; the caller's interactive BASE persists. */
 fvm_write_cell(base_address,10);
 if(!forth_eval(source))goto done;
 if(current>=0){fail("incomplete module definition");goto done;}
 if(fvm_depth){fail("module initialization leaves stack values");goto done;}
 fvm_overlay_copy(test_data);
 if(test_word&&*test_word) {
  if(!forth_execute(test_word))goto done;
  if(fvm_depth!=1||!fvm_ds[0]){fail("module test must return one true flag");goto done;}
 }
 /* Test writes are discarded; only initializer effects become live. */
 fvm_overlay_restore(test_data);fvm_write_cell(base_address,transaction_before.base);
 if(!transaction_bindings_valid())goto done;
 transaction_words=count-transaction_before.count;transaction_token=token_used-transaction_before.token;
 transaction_direct=direct_used-transaction_before.direct;transaction_data=data_used-transaction_before.data;ok=1;
done:
 copy(saved_error,error_text);transaction_active=0;origin_id=0;
 if(ok&&commit) {
  activation_before=transaction_before;have_activation=1;
  activation_count=count;activation_token=token_used;activation_direct=direct_used;
  activation_data=data_used;activation_source=source_used;
  fvm_overlay_commit();checkpoint_restore(&transaction_before,0,0);
  catalog_changed();
 }else {fvm_overlay_discard();checkpoint_restore(&transaction_before,1,0);}
 copy(error_text,saved_error);return ok;
}
void forth_transaction_forget_rollback(void){have_activation=0;}
void forth_transaction_sizes(unsigned *w,size_t *t,size_t *d,size_t *a){*w=transaction_words;*t=transaction_token;*d=transaction_direct;*a=transaction_data;}
int forth_transaction_rollback(void) {
 if(!have_activation)return forth_transaction_error("no activation checkpoint");
 if(tasks_live())return forth_transaction_error("rollback refused while background tasks are live");
 if(transaction_active||current>=0||fvm_rdepth||fvm_ldepth)return forth_transaction_error("rollback requires idle VM");
 if(count!=activation_count||token_used!=activation_token||direct_used!=activation_direct||data_used!=activation_data||source_used!=activation_source)return forth_transaction_error("rollback blocked by later dictionary definitions");
 checkpoint_restore(&activation_before,1,1);have_activation=0;catalog_changed();return 1;
}
void forth_repl(void) {
 char line[256];unsigned used=0;int overflow=0,cr=0,changed=1;
 print("\r\nVIOS Forth 24 / Agon Light 2\r\nvios> ");
 if(line_mode_hook)line_mode_hook(1);
 for(;;) {
  int c=platform_console_getc();
  if(c<0){if(!used&&current<0){if(idle_hook){idle_hook(changed);changed=0;}forth_tasks_run();}continue;}
  if(c=='\n'&&cr){cr=0;continue;}cr=0;
  if(c=='\r'||c=='\n') {
   cr=c=='\r';print("\r\n");line[used]=0;
   /* Host envelope is consumed before the compiler sees source. No queued
    * marker can become input to a running KEY primitive. */
   char nonce[9];int tagged=used>=10&&line[0]=='@'&&line[9]==' ';
   for(unsigned i=0;tagged&&i<8;++i) {
    char h=line[i+1];if(!((h>='0'&&h<='9')||(h>='A'&&h<='F')))tagged=0;
    nonce[i]=h;
   }
   nonce[8]=0;int success=0;
   const char *request=line+(tagged?10:0);int request_ok=1;
   if(tagged&&(transport_crc||line[10]=='!')) {
    if(used<20||line[10]!='!'||line[19]!=' ') {copy(error_text,"request CRC required");request_ok=0;}
    else {
     uint32_t crc=0;
     for(unsigned i=11;i<19;++i){char h=line[i];unsigned digit=h>='0'&&h<='9'?(unsigned)(h-'0'):h>='A'&&h<='F'?(unsigned)(h-'A'+10):99;
      if(digit>15)request_ok=0;crc=(crc<<4)|digit;
     }
     request=line+20;
     if(!request_ok||crc!=forth_module_crc32(request)){copy(error_text,"request CRC mismatch");request_ok=0;}
    }
   }
   if(overflow) {print("error: input line too long\r\n");rollback();}
   else if(request_ok&&line_mode_hook)line_mode_hook(0);
   if(!overflow&&(!request_ok||!(success=forth_eval(request)))){print("error: ");print(forth_last_error());print("\r\n");}
   if(line_mode_hook)line_mode_hook(1);
   if(tagged){print("\r\n@");print(nonce);print("\r\n");}
   if(success)print("ok\r\n");else if(tagged)print("error\r\n");
   used=0;overflow=0;changed=1;print(current>=0?"compile> ":"vios> ");
  }else if(c==8||c==127){if(used&&!overflow){--used;print("\b \b");}}
  else if(c==3){rollback();fvm_reset();used=0;overflow=0;changed=1;print("\r\naborted\r\nvios> ");}
  else if(c>=32&&c<127&&!overflow){if(used+1<sizeof line){line[used++]=(char)c;platform_console_putc((unsigned char)c);}else overflow=1;}
 }
}

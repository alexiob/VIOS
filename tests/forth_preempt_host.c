/* Tick preemption at VM safe points (tasks design B2) on the C reference VM.
 * Ticks are simulated through the same path the board's ISR uses: after k
 * more instructions fvm_budget reaches 0 with forth_tick_pending set and the
 * rest of the budget in forth_budget_stolen. A simulated tick fires only while
 * the planner has armed a delay, as the ISR would. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "forth/system.h"
static char output[8192];static size_t output_used;
void platform_console_putc(unsigned char c){if(output_used+1<sizeof output){output[output_used++]=(char)c;output[output_used]=0;}}
int platform_console_getc(void){return -1;}
void platform_console_init(void){}
const char *platform_identity(void){return "VIOS preempt host";}
void platform_restart(void){}
static uint32_t clock_ms;static uint32_t fake_clock(void){return clock_ms;}
static unsigned armed,arms,every,ticks;
static void arm(unsigned delay){armed=delay;++arms;}
/* Every `every` instructions (randomised when every is 0 and seeded). */
static unsigned seeded;
/* A steal can also land while C runs between VM runs (the gate's listen):
 * the gate steals the current budget as the ISR would. */
static int steal_in_gate;
static int gate(void){if(steal_in_gate&&armed&&fvm_budget){forth_budget_stolen=fvm_budget;fvm_budget=0;forth_tick_pending=1;}return 1;}
static void sim_tick(void){
 if(!armed||forth_tick_pending)return;
 unsigned k=every?every:1u+(unsigned)(rand()%(int)seeded);
 if(fvm_budget>k){forth_budget_stolen=fvm_budget-k;fvm_budget=k;forth_tick_pending=1;++ticks;}
}
static unsigned ctrl_c_after;
static int ctrl_c(void){return ctrl_c_after&&!--ctrl_c_after;}
static void eval(const char *s){output_used=0;output[0]=0;if(!forth_eval(s)){fprintf(stderr,"eval failed: %s: %s\n",s,forth_last_error());assert(0);}}
static fcell value(const char *s){eval(s);fcell v=fvm_pop();assert(!fvm_error);return v;}
static void fresh(void){forth_register_clock(fake_clock);forth_init();forth_register_preempt(arm);forth_register_tick_source(sim_tick);forth_register_task_gate(gate);}
int main(void){
 fresh();
 eval("VARIABLE N : BG BEGIN N @ 1+ N ! AGAIN ; : FG 0 4000 0 DO I + LOOP ;");
 /* Without a ready task the foreground is never interrupted (nothing armed). */
 every=50;ticks=0;assert(value("FG")==7998000u&&ticks==0&&armed==0);
 /* A ready task arms the foreground quantum; ticks give it time inside FG. */
 assert(value("' BG SPAWN")==1&&armed==4);
 eval("0 N !");ticks=0;assert(value("FG")==7998000u);
 fcell n=value("N @");assert(n>0&&ticks>0&&"the background task ran during the foreground word");
 eval("PREEMPT-STATS");assert(strstr(output,"preempt on switches "));
 /* PREEMPT off: the foreground runs alone again. */
 eval("0 PREEMPT 0 N !");ticks=0;assert(value("FG")==7998000u&&value("N @")==0&&ticks==0&&armed==0);
 eval("-1 PREEMPT 1 TASK-KILL");
 /* The same program gives the same result under any tick schedule. */
 eval(": SUMSQ 0 300 0 DO I I * + LOOP ; VARIABLE Q");
 fcell expected=value("SUMSQ");
 eval("' BG SPAWN DROP");
 for(unsigned s=1;s<200;s+=7){every=s;assert(value("SUMSQ")==expected);}
 eval("2 TASK-KILL");
 /* CRITICAL( ... )CRITICAL keeps a shared read-modify-write whole. */
 eval("VARIABLE C : INCS 0 DO CRITICAL( C @ 1+ C ! )CRITICAL LOOP ; : T1 500 INCS ; : T2 500 INCS ;");
 srand(7);seeded=40;every=0;
 for(unsigned round=0;round<5;++round){
  eval("0 C !");assert(value("' T1 SPAWN")>0&&value("' T2 SPAWN")>0);
  eval("700 INCS");
  for(unsigned guard=0;guard<10000&&forth_tasks_run();++guard){}
  assert(value("C @")==1700&&"no increment lost under random ticks");
 }
 /* Board finding (2026-10-20): a steal during the round's gate must not leave
  * the foreground with a zero budget ("execution budget exhausted"). */
 steal_in_gate=1;every=60;eval("0 N !");assert(value("' BG SPAWN")>0);
 for(unsigned r=0;r<5;++r)assert(value("FG")==7998000u);
 steal_in_gate=0;
 /* A background slice ends at a tick (more slices than budget alone gives). */
 every=37;eval(": SPIN BEGIN AGAIN ;");assert(value("' SPIN SPAWN")>0);
 for(unsigned i=0;i<20;++i)forth_tasks_run();
 eval("TASKS");assert(strstr(output,"SPIN")||strstr(output,"state ready"));
 eval("PREEMPT-STATS");assert(strstr(output,"preempt on"));
 /* Ctrl-C: with nothing to share, the foreground still gets a 50 ms check,
  * and a request fails the running command at the next safe point. */
 eval("2 TASK-KILL 3 TASK-KILL 4 TASK-KILL");for(unsigned i=0;i<10;++i)forth_tasks_run();
 forth_register_interrupt(ctrl_c);ctrl_c_after=5;every=200;
 output_used=0;assert(!forth_eval(": SPIN2 BEGIN AGAIN ; SPIN2"));assert(strstr(forth_last_error(),"interrupted"));
 assert(value("1 2 +")==3&&"the REPL keeps working after an interrupt");
 /* A foreground MS or FRAME-WAIT wait (C, not the VM) is interruptible too:
  * the clock does not move here, so only Ctrl-C can end these waits. */
 ctrl_c_after=3;assert(!forth_eval("100000 MS"));assert(strstr(forth_last_error(),"interrupted"));
 ctrl_c_after=3;assert(!forth_eval("FRAME-WAIT"));assert(strstr(forth_last_error(),"interrupted"));
 forth_register_interrupt(0);
 /* FRAME-WAIT: a task sleeps to the next 50/3 ms boundary. */
 clock_ms=1000;eval(": FRAMER FRAME-WAIT 1 Q ! ;");eval("0 Q !");assert(value("' FRAMER SPAWN")>0);
 forth_tasks_run();assert(value("Q @")==0);
 clock_ms=1016;forth_tasks_run();assert(value("Q @")==0);
 clock_ms=1017;forth_tasks_run();assert(value("Q @")==1&&"woke at the 61st frame boundary, 1016.67 ms");
 /* B5: chunked FILL/MOVE give the same bytes as memset/memmove, with ticks
  * landing between chunks; per-task BASE. */
 eval("CREATE BUF 2100 ALLOT");fcell buf=value("BUF");
 static const unsigned sizes[]={0,1,255,256,257,600,1000};
 static unsigned char mirror[2100];
 every=7;
 for(unsigned k=0;k<sizeof sizes/sizeof sizes[0];++k){
  unsigned u=sizes[k];char line[96];
  for(unsigned i=0;i<2100;++i){fvm_write8((buf+i)&FVM_MASK,(unsigned char)(i*7+k));mirror[i]=(unsigned char)(i*7+k);}
  snprintf(line,sizeof line,"BUF 50 + %u 165 FILL",u);eval(line);memset(mirror+50,165,u);
  snprintf(line,sizeof line,"BUF 40 + BUF 100 + %u MOVE",u);eval(line);memmove(mirror+100,mirror+40,u);   /* overlapping, backward */
  snprintf(line,sizeof line,"BUF 300 + BUF 260 + %u MOVE",u);eval(line);memmove(mirror+260,mirror+300,u);  /* overlapping, forward */
  for(unsigned i=0;i<2100;++i)assert(fvm_read8((buf+i)&FVM_MASK)==mirror[i]&&"chunked FILL/MOVE match memset/memmove");
 }
 eval("VARIABLE TB : T16 HEX 255 TB ! BEGIN PAUSE AGAIN ;");fcell id=value("' T16 SPAWN");
 for(unsigned i=0;i<4;++i)forth_tasks_run();
 assert(value("BASE @")==10&&value("TB @")==255&&"HEX in a task leaves the foreground decimal");
 eval("TASKS");assert(strstr(output,"base 16"));
 char kill[32];snprintf(kill,sizeof kill,"%u TASK-KILL",(unsigned)id);eval(kill);
 /* A console task preempting a command does not print into the middle of
  * the command's output; its held bytes come out after the command. */
 eval("VARIABLE CTN : CT BEGIN 66 EMIT CTN @ 1+ CTN ! PAUSE AGAIN ; : PR 40 0 DO 65 EMIT LOOP ;");
 fcell ct=value("' CT SPAWN-CON");every=3;
 eval("0 CTN ! PR");
 {const char *a=strchr(output,'A');assert(a&&!strncmp(a,"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",40)&&"the command's output is not interleaved");}
 assert(value("CTN @")>0&&"the console task did run during the command");
 output_used=0;output[0]=0;forth_tasks_run();assert(strchr(output,'B')&&"held console-task output follows");
 {char kill[32];snprintf(kill,sizeof kill,"%u TASK-KILL",(unsigned)ct);eval(kill);}
 printf("tick preemption: foreground rounds, determinism, CRITICAL and slice ends passed (%u arms)\n",arms);
 return 0;
}

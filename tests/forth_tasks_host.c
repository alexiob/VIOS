/* Background task scheduling on the C reference VM with a fake clock/console. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "forth/system.h"
static char output[8192];static size_t output_used;
static const char *input;static unsigned empty_polls;
static uint32_t clock_ms;
void platform_console_putc(unsigned char c){if(output_used+1<sizeof output){output[output_used++]=(char)c;output[output_used]=0;}}
/* Reports "no byte" a few times before each scripted byte, as a real UART does. */
int platform_console_getc(void){if(!input||!*input)return -1;if(empty_polls<3){++empty_polls;return -1;}empty_polls=0;return (unsigned char)*input++;}
void platform_console_init(void){}
const char *platform_identity(void){return "VIOS task host";}
void platform_restart(void){}
static uint32_t fake_clock(void){return clock_ms;}
static int unsafe_native(void){return 1;}
static void eval(const char *s){output_used=0;output[0]=0;if(!forth_eval(s)){fprintf(stderr,"eval failed: %s: %s\n",s,forth_last_error());assert(0);}}
static void fails(const char *s,const char *message){output_used=0;assert(!forth_eval(s));assert(strstr(forth_last_error(),message));}
static fcell value(const char *s){eval(s);fcell v=fvm_pop();assert(!fvm_error);return v;}
static int has(const char *needle){return strstr(output,needle)!=NULL;}
int main(void){
 forth_register_clock(fake_clock);forth_init();
 assert(forth_register_native("UNSAFE",unsafe_native,0));
 eval("VARIABLE A VARIABLE B VARIABLE C");
 eval(": CA BEGIN A @ 1+ A ! AGAIN ; : CB BEGIN B @ 1+ B ! PAUSE AGAIN ;");
 /* Foreground state survives every background slice. */
 eval("7 8");
 assert(value("' CA SPAWN")==1&&value("' CB SPAWN")==2);
 for(unsigned i=0;i<10;++i)assert(forth_tasks_run()==2);
 assert(fvm_depth==2&&fvm_ds[0]==7&&fvm_ds[1]==8);
 fcell a=value("A @"),b=value("B @");
 assert(b==10&&"cooperative task yields once per PAUSE");
 assert(a>1000&&"looping task is preempted by budget, not starved or stuck");
 eval("TASKS");assert(has("task 1 state ready")&&has("task 2 state ready")&&has("slices 10 steps 10240"));
 /* ENGINE switches change IP encodings and are refused while tasks live. */
 fails("ENGINE C","refused while background tasks");
 eval("1 TASK-KILL 2 TASK-KILL");assert(value("1 TASK-STATE")==5);
 assert(forth_tasks_run()==0);fcell frozen=value("A @");assert(forth_tasks_run()==0&&value("A @")==frozen);
 eval("ENGINE C");
 /* Output goes to the task ring, never to the shared console. */
 eval(": PR 65 EMIT 42 . CR ;");assert(value("' PR SPAWN")==1);
 output_used=0;output[0]=0;forth_tasks_run();assert(output_used==0);
 assert(value("1 TASK-STATE")==3);
 eval("TASK-OUT 1");assert(has("task-out 1 hex 413432200D0A dropped 0"));
 eval("TASK-OUT 1");assert(has("task-out 1 hex - dropped 0"));
 /* Faults are isolated: the task records them, the foreground keeps its own. */
 eval(": KK KEY ; : BADN UNSAFE ; : DEEP 70 0 DO I LOOP PAUSE ;");
 assert(value("' KK SPAWN")==1&&value("' BADN SPAWN")==2&&value("' DEEP SPAWN")==3);
 forth_tasks_run();eval("TASKS");
 assert(has("task 1 state failed")&&has("task 2 state failed")&&has("task 3 state failed"));
 assert(value("1 TASK-STATE")==4);
 eval("TASKS");
 /* Error text is hex encoded; spot-check "KEY unavailable" and capacity. */
 assert(has("4B455920756E617661696C61626C65"));assert(has("7461736B20737461636B2065786365656473"));
 assert(!strstr(forth_last_error(),"KEY"));
 /* MS sleeps by the registered clock and wakes after the deadline. */
 eval(": SLEEPER 100 MS C @ 1+ C ! ;");clock_ms=1000;assert(value("' SLEEPER SPAWN")==1);
 forth_tasks_run();assert(value("1 TASK-STATE")==2);
 clock_ms=1050;forth_tasks_run();assert(value("C @")==0);
 clock_ms=1100;forth_tasks_run();assert(value("C @")==1&&value("1 TASK-STATE")==3);
 /* A foreground KEY wait keeps background tasks running. */
 eval("0 B !");assert(value("' CB SPAWN")==1);
 input="Z";assert(value("KEY")=='Z');assert(value("B @")>=3);
 /* Foreground PAUSE runs one round. */
 fcell before=value("B @");eval("PAUSE PAUSE");assert(value("B @")==before+2);
 eval("1 TASK-KILL");
 /* No free slot is an error; slots are reusable after completion. */
 eval(": FOREVER BEGIN PAUSE AGAIN ;");
 for(unsigned i=1;i<=FORTH_TASKS_MAX;++i)assert(value("' FOREVER SPAWN")==i);
 fails("' FOREVER SPAWN","no free task slot");
 eval("1 TASK-KILL 2 TASK-KILL 3 TASK-KILL 4 TASK-KILL");fails("0 TASK-STATE","invalid task id");
 /* SPAWN-CON output goes to the shared console, not the ring. */
 eval(": HELLO 72 EMIT 105 EMIT ;");{fcell id=value("' HELLO SPAWN-CON");output_used=0;output[0]=0;forth_tasks_run();
  assert(has("Hi"));struct forth_task_info t;assert(forth_task_info((unsigned)id,&t)&&t.output_bytes==0&&t.state==3);}
 /* The slice length is a runtime policy. */
 eval("TASK-SLICE 256");eval("0 A !");assert(value("' CA SPAWN")>=1);forth_tasks_run();
 eval("TASKS");assert(has("task-slice 256")&&has("steps 256 "));fails("TASK-SLICE 10","64..65535");
 eval("TASK-SLICE 1024");for(unsigned i=1;i<=FORTH_TASKS_MAX;++i){char k[24];snprintf(k,sizeof k,"%u TASK-KILL",i);eval(k);}
 /* forth_init clears every slot. */
 assert(value("' FOREVER SPAWN")==1);forth_init();assert(forth_tasks_run()==0);
 puts("background tasks: preemption, yield, sleep, isolation, output rings and guards passed");return 0;
}

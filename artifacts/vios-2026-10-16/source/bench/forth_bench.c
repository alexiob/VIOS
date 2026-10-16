/* Hardware measurements; UART output is strictly outside timed intervals. */
#include "forth/system.h"
#include "platform/console.h"
#include "platform/timer.h"
#define SAMPLES 9
static int ready;
static fcell saved_stack[FVM_DS];
static void out(const char *p) {while(*p)platform_console_putc((unsigned char)*p++);}
static void num(unsigned long n) {char b[16];unsigned i=0;do {b[i++]=(char)('0'+n%10);n/=10;}while(n);while(i)platform_console_putc((unsigned char)b[--i]);}
static void field(const char *s,unsigned long n) {out(s);num(n);}
static fcell expected_arith(unsigned n) {fcell x=0;while(n--)x=((((x+3)&FVM_MASK)^7)-1)&FVM_MASK;return x;}
static unsigned short median(unsigned short *v) {
 for(unsigned i=1;i<SAMPLES;++i){unsigned short n=v[i];unsigned j=i;while(j&&v[j-1]>n){v[j]=v[j-1];--j;}v[j]=n;}return v[SAMPLES/2];
}
void forth_benchmark(void) {
 enum forth_engine previous=forth_engine();fcell depth=fvm_depth;
 fcell previous_base=forth_get_base();forth_set_base(10);
 for(fcell i=0;i<depth;++i)saved_stack[i]=fvm_ds[i];
 fvm_depth=0;
 if(!ready) {
  if(!forth_eval("DECIMAL : BM-STEP 3 + ; : BM-EMPTY ; : BM-ARITH 0 SWAP 0 DO 3 + 7 XOR 1 - LOOP ; : BM-CALL 0 SWAP 0 DO BM-STEP LOOP ; : BM-BRANCH 0 SWAP 0 DO I 1 AND IF 3 + ELSE 1 + THEN LOOP ; : BM-LOOP 0 SWAP 0 DO I + LOOP ; VARIABLE BM-CELL : BM-MEM 0 BM-CELL ! 0 DO BM-CELL @ 1 + BM-CELL ! LOOP BM-CELL @ ;")) {out("BENCH ERROR setup ");out(forth_last_error());out("\r\n");goto restore;}
  ready=1;
 }
 out("BENCH VIOS checked24+peaks timer_hz=288000 wrap_ticks=65536 samples=9\r\n");
 unsigned short overhead[SAMPLES];
 for(unsigned i=0;i<SAMPLES;++i){unsigned short a=platform_timer_read(),b=platform_timer_read();overhead[i]=(unsigned short)(a-b);}
 field("TIMER read-pair-ticks=",median(overhead));out("\r\n");
 static const char *const names[]={"BM-ARITH","BM-CALL","BM-BRANCH","BM-LOOP","BM-MEM"};
 for(unsigned engine=0;engine<3;++engine) {
  forth_set_engine((enum forth_engine)engine);
  unsigned short empty[SAMPLES];
  for(unsigned r=0;r<SAMPLES;++r) {unsigned short a=platform_timer_read();int ok=forth_execute("BM-EMPTY");unsigned short b=platform_timer_read();if(!ok)goto error;empty[r]=(unsigned short)(a-b);}
  field("EMPTY engine=",engine);field(" median=",median(empty));out("\r\n");
  for(unsigned test=0;test<5;++test)for(unsigned n=25;n<=100;n*=2) {
   unsigned short ticks[SAMPLES];
   fcell want=test==0?expected_arith(n):test==1?n*3:test==2?(n/2)*4+(n&1):test==3?n*(n-1)/2:n;
   for(unsigned r=0;r<SAMPLES;++r) {
    fvm_depth=0;fvm_push(n);
    unsigned short a=platform_timer_read();int ok=forth_execute(names[test]);unsigned short b=platform_timer_read();
    ticks[r]=(unsigned short)(a-b);
    if(!ok||fvm_depth!=1||fvm_ds[0]!=want)goto error;
   }
   field("SAMPLE engine=",engine);out(" word=");out(names[test]);field(" n=",n);field(" result=",want);
   out(" ticks=");for(unsigned r=0;r<SAMPLES;++r){if(r)out(",");num(ticks[r]);}
   field(" median=",median(ticks));out("\r\n");
  }
 }
 {
  unsigned short a=platform_timer_read();int ok=forth_eval(": BM-COMPILE DUP IF 3 + ELSE 7 + THEN ;");unsigned short b=platform_timer_read();
  if(!ok)goto error;
  field("COMPILE chars=",sizeof(": BM-COMPILE DUP IF 3 + ELSE 7 + THEN ;")-1);field(" ticks=",(unsigned short)(a-b));out("\r\n");
 }
 field("CODE token-bytes=",forth_code_bytes(FORTH_TOKEN));field(" direct-bytes=",forth_code_bytes(FORTH_DIRECT));out("\r\nBENCH DONE\r\n");goto restore;
error:out("BENCH ERROR ");out(forth_last_error());field(" depth=",fvm_depth);if(fvm_depth)field(" top=",fvm_ds[fvm_depth-1]);out("\r\n");
restore:
 forth_set_engine(previous);forth_set_base(previous_base);fvm_depth=depth;fvm_error=0;
 for(fcell i=0;i<depth;++i)fvm_ds[i]=saved_stack[i];
}

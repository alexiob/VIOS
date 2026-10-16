/* VIOS resident dual-emitting compiler. CPU execution stays behind vm.h. */
#include "forth/system.h"
#include "platform/console.h"
#include "platform/system.h"
#define WORDS_MAX 128
#define NAME_MAX 31
#define TOKEN_BYTES 16384
#define DIRECT_BYTES 32768
#define DATA_BYTES 8192
#define SOURCE_BYTES 8192
#define CONTROL_MAX 64
#define HASH_BUCKETS 32

enum { WK_PRIMITIVE,WK_COLON,WK_CONSTANT,WK_VARIABLE,WK_DEFER };
enum { CS_IF,CS_ELSE,CS_BEGIN,CS_WHILE,CS_DO };
struct word {
 char name[NAME_MAX+1]; fcell token,direct,aux;
 unsigned short link,source,length,token_length,direct_length; unsigned char kind,opcode;
};
struct control { unsigned char kind; size_t token,direct; };
static struct word words[WORDS_MAX];
static unsigned short buckets[HASH_BUCKETS];
static unsigned char token_code[TOKEN_BYTES],direct_code[DIRECT_BYTES],data_space[DATA_BYTES];
static char source_space[SOURCE_BYTES];
static struct control controls[CONTROL_MAX];
static size_t token_used,direct_used,data_used,source_used,control_used;
static size_t mark_token,mark_direct,mark_data,mark_source;
static unsigned count;
static int current;
static enum forth_engine selected;
static fcell base_address;
static char error_text[96];

static size_t length(const char *s) { size_t n=0;while(s[n])++n;return n; }
static int equal(const char *a,const char *b) { while(*a&&*a==*b){++a;++b;}return *a==*b; }
static char upper(char c) {return c>='a'&&c<='z'?(char)(c-'a'+'A'):c;}
static void copy(char *d,const char *s) {while((*d++=*s++)) {}}
static void print(const char *s) {while(*s)platform_console_putc((unsigned char)*s++);}
static void unsigned_number(fcell n) {
 char buf[24];unsigned i=0;fcell radix=fvm_read_cell(base_address);
 if(radix<2||radix>36)radix=10;
 do {unsigned d=(unsigned)(n%radix);buf[i++]=(char)(d<10?'0'+d:'A'+d-10);n/=radix;}while(n);
 while(i)platform_console_putc((unsigned char)buf[--i]);
}
static void number(fcell n) {
 if(n&0x800000UL) {platform_console_putc('-');n=((~n)+1)&FVM_MASK;}
 unsigned_number(n);platform_console_putc(' ');
}
static void decimal_number(fcell n) {
 char b[16];unsigned i=0;do {b[i++]=(char)('0'+n%10);n/=10;}while(n);
 while(i)platform_console_putc((unsigned char)b[--i]);
}
static void rollback(void) {
 if(current>=0) {token_used=mark_token;direct_used=mark_direct;data_used=mark_data;source_used=mark_source;}
 current=-1;control_used=0;
}
static int fail(const char *s) {
 copy(error_text,s);rollback();fvm_depth=fvm_rdepth=fvm_ldepth=0;return 0;
}
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
 mark_token=token_used;mark_direct=direct_used;mark_data=data_used;mark_source=source_used;
 current=(int)count;words[count].kind=(unsigned char)kind;copy(words[count].name,name);
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
static int publish(void) {
 unsigned h=hash(words[count].name);
 words[count].link=buckets[h];words[count].length=(unsigned short)(source_used-words[count].source);
 words[count].token_length=(unsigned short)(token_used-mark_token);
 words[count].direct_length=(unsigned short)(direct_used-mark_direct);
 buckets[h]=(unsigned short)(count+1);++count;current=-1;return 1;
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
static int execute_index(unsigned n) {
 fvm_error=0;fvm_rdepth=fvm_ldepth=0;fvm_budget=1000000UL;
 fvm_ip=selected==FORTH_DIRECT?words[n].direct+4:words[n].token;
 for(;;) {
  unsigned char result;
#ifdef __ez80__
  if(selected==FORTH_DIRECT)result=fvm_direct_run();
  else if(selected==FORTH_TOKEN)result=fvm_token_run();
  else result=fvm_reference_run();
#else
  result=fvm_reference_run();
#endif
  if(fvm_error)return fail(vm_message());
  if(!result)return 1;
  unsigned code=result-0x80u;
  if(code==F_EMIT) {fcell c=fvm_pop();if(!fvm_error)platform_console_putc((unsigned char)c);}
  else if(code==F_DOT) {fcell nvalue=fvm_pop();if(!fvm_error)number(nvalue);}
  else if(code==F_CR)print("\r\n");
  else if(code==F_KEY) {int c;do {c=platform_console_getc();}while(c<0);fvm_push((fcell)c);}
  else if(code==F_EXECUTE||code==F_DEFER) {fcell xt=fvm_pop();if(!fvm_error&&!call_xt(xt))return 0;}
  else if(!fvm_complex(code))return fail(vm_message());
  if(fvm_error)return fail(vm_message());
 }
}
int forth_execute(const char *name) {int n=find(name);return n<0?unknown(name):execute_index((unsigned)n);}
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
static int control_word(const char *s) {
 struct control c;
 if(current<0)return fail("compile-only word");
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
 if(equal(s,"LOOP")) {if(c.kind!=CS_DO)return fail("LOOP needs DO");return op(F_LOOP)&&operand(address(token_code+c.token),address(direct_code+c.direct));}
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
}
int forth_eval(const char *source) {
 const char *p=source;char s[NAME_MAX+1],name[NAME_MAX+1];int got;
 error_text[0]=0;fvm_error=0;
 while((got=next_token(&p,s))>0) {
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
  if(current>=0&&!source_add(s))return 0;
  if(equal(s,";")) {
   if(current<0)return fail("unexpected semicolon");
   if(control_used)return fail("unbalanced control flow");
   if(!op(F_EXIT)||!publish())return 0;continue;
  }
  if(equal(s,"VARIABLE")||equal(s,"CONSTANT")||equal(s,"DEFER")) {
   if(current>=0)return fail("defining word inside definition");
   if(!need_token(&p,name)||!defining(s,name))return 0;continue;
  }
  if(equal(s,"IF")||equal(s,"ELSE")||equal(s,"THEN")||equal(s,"BEGIN")||equal(s,"UNTIL")||equal(s,"AGAIN")||equal(s,"WHILE")||equal(s,"REPEAT")||equal(s,"DO")||equal(s,"LOOP")) {
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
  if(equal(s,"WORDS")||equal(s,".S")||equal(s,"SEE")||equal(s,"RESOURCES")||equal(s,"ENGINE")) {
   if(current>=0)return fail("interactive tool inside definition");
   if(equal(s,"WORDS")){for(unsigned i=0;i<count;++i){print(" ");print(words[i].name);}print(" \r\n");}
   else if(equal(s,".S"))show_stack();
   else if(equal(s,"SEE")){if(!need_token(&p,name))return 0;int n=find(name);if(n<0)return unknown(name);show_word(n);}
   else if(equal(s,"RESOURCES"))forth_report();
   else {
    if(!need_token(&p,name))return 0;
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
 token_used=direct_used=data_used=source_used=control_used=0;count=0;current=-1;
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
 forth_eval(": 1+ 1 + ; : 1- 1 - ; : NEGATE 0 SWAP - ; : 2DUP OVER OVER ; : 2DROP DROP DROP ; : CELLS 3 * ; : CELL+ 3 + ; : TRUE -1 ; : FALSE 0 ; : HEX 16 BASE ! ; : DECIMAL 10 BASE ! ;");
}
void forth_set_engine(enum forth_engine engine) {selected=engine;}
enum forth_engine forth_engine(void) {return selected;}
const char *forth_last_error(void) {return error_text;}
int forth_compiling(void) {return current>=0;}
size_t forth_code_bytes(enum forth_engine engine) {return engine==FORTH_DIRECT?direct_used:token_used;}
void forth_report(void) {
 print("engine ");print(selected==FORTH_DIRECT?"DIRECT":selected==FORTH_TOKEN?"TOKEN":"C");
 print(" words ");decimal_number(count);print("/128 token-bytes ");decimal_number((fcell)token_used);
 print(" direct-bytes ");decimal_number((fcell)direct_used);print(" data-bytes ");decimal_number((fcell)data_used);
 print(" source-bytes ");decimal_number((fcell)source_used);print(" data-peak ");decimal_number(fvm_ds_peak);
 print(" return-peak ");decimal_number(fvm_rs_peak);print("\r\n");
}
fcell forth_get_base(void) {return fvm_read_cell(base_address);}
void forth_set_base(fcell radix) {if(radix>=2&&radix<=36)fvm_write_cell(base_address,radix);}
void forth_repl(void) {
 char line[256];unsigned used=0;int overflow=0,cr=0;
 print("\r\nVIOS Forth 24 / Agon Light 2\r\nvios> ");
 for(;;) {
  int c=platform_console_getc();if(c<0)continue;
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
   if(overflow) {print("error: input line too long\r\n");rollback();}
   else if(!(success=forth_eval(line+(tagged?10:0)))){print("error: ");print(forth_last_error());print("\r\n");}
   if(tagged){print("\r\n@");print(nonce);print("\r\n");}
   if(success)print("ok\r\n");else if(tagged)print("error\r\n");
   used=0;overflow=0;print(current>=0?"compile> ":"vios> ");
  }else if(c==8||c==127){if(used&&!overflow){--used;print("\b \b");}}
  else if(c==3){rollback();fvm_reset();used=0;overflow=0;print("\r\naborted\r\nvios> ");}
  else if(c>=32&&c<127&&!overflow){if(used+1<sizeof line){line[used++]=(char)c;platform_console_putc((unsigned char)c);}else overflow=1;}
 }
}

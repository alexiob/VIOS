/* Handles over SD files, /dev endpoints and pipes. Non-blocking natives only;
 * blocking is Forth policy (READ-WAIT/WRITE-ALL). See
 * docs/superpowers/specs/2026-10-18-vios-handles.md. */
#include "vios/runtime.h"
#include "platform/storage.h"
#include "platform/vdp.h"
#define HANDLES 8
#define PIPES 4
#define PIPE_BYTES 128
#define SNAPSHOT 512
enum {H_FREE,H_FILE,H_PIPE,H_NULL,H_CONS,H_SNAPSHOT,H_HOST};
enum {E_AGAIN=1,E_BROKEN=2,E_BAD=3,E_FULL=4,E_PATH=5,E_IO=6,E_MODE=7};
static struct {unsigned char kind,mode,owner,slot;uint32_t moved;char path[24];} handles[HANDLES];
static struct {unsigned char data[PIPE_BYTES];unsigned short head,count;unsigned char readers,writers,used;} pipes[PIPES];
static char snapshot[SNAPSHOT];static unsigned short snapshot_length,snapshot_offset;static int snapshot_owner=-1;
static char last_error[48];
/* User-space devices: /dev/NAME served by a Forth word (Plan 9 style). */
#define DEVICES 4
static struct {char name[12];fcell xt;} devices[DEVICES];
static void note(const char *s){unsigned i=0;while(s[i]&&i+1<sizeof last_error){last_error[i]=s[i];++i;}last_error[i]=0;}
static fcell code(int n,const char *message){note(message);return (fcell)(-n)&FVM_MASK;}
static int push(fcell n){return fvm_push(n)?1:forth_error("data stack overflow");}
static int handle(fcell h){return h>=1&&h<=HANDLES&&handles[h-1].kind!=H_FREE?(int)h-1:-1;}
static int path_copy(fcell address,fcell length,char *out,size_t capacity){
 if(!length||length>=capacity)return 0;
 for(fcell i=0;i<length;++i){unsigned char c=fvm_read8(address+i);if(fvm_error||c<33||c>126)return 0;out[i]=(char)c;}
 out[length]=0;return 1;
}
static int same(const char *a,const char *b){return vios_same(a,b);}
/* Mac files over USB (/host/...): one bounded request/reply per operation.
 * Requests: O<mode r|w><path>, R<h><n>, W<h><bytes>, C<h>; replies K<data>
 * or E<message>. No harness serving -> the request times out (~1 s). */
static unsigned char host_reply[240];static unsigned host_reply_length;static unsigned char host_id;
/* Every request carries a 1-byte id the host echoes first, so a late reply
 * to an earlier (timed-out) request is never taken as the current answer.
 * On success host_reply holds K/E and its data, without the id. */
static int host_call(const unsigned char *request,size_t length){
 unsigned char framed[240];if(length+1>sizeof framed){note("host request too long");return 0;}
 framed[0]=++host_id;for(size_t i=0;i<length;++i)framed[1+i]=request[i];
 unsigned char raw[240];unsigned n=0;
 if(!platform_host_request(framed,length+1,raw,sizeof raw,&n)||n<2||raw[0]!=host_id){note("host service unavailable");return 0;}
 host_reply_length=n-1;for(unsigned i=0;i<host_reply_length;++i)host_reply[i]=raw[1+i];
 if(host_reply[0]=='K')return 1;
 unsigned m=host_reply_length-1<sizeof last_error-1?host_reply_length-1:sizeof last_error-1;
 for(unsigned i=0;i<m;++i)last_error[i]=(char)host_reply[1+i];last_error[m]=0;return 0;
}
static int host_close(unsigned remote){unsigned char r[2]={'C',(unsigned char)remote};return host_call(r,2);}
static int host_read(unsigned remote,unsigned char *out,unsigned want,unsigned *got){
 unsigned char r[3]={'R',(unsigned char)remote,(unsigned char)(want>200?200:want)};*got=0;
 if(!host_call(r,3))return 0;
 if(host_reply_length-1>r[2]){note("host reply longer than requested");return 0;} /* never overrun out[] */
 *got=host_reply_length-1;for(unsigned i=0;i<*got;++i)out[i]=host_reply[1+i];return 1;
}
static int host_write(unsigned remote,const unsigned char *data,unsigned n,unsigned *put){
 unsigned char r[202];r[0]='W';r[1]=(unsigned char)remote;if(n>200)n=200;for(unsigned i=0;i<n;++i)r[2+i]=data[i];*put=0;
 if(!host_call(r,n+2)||host_reply_length<2||host_reply[1]>n){if(host_reply_length>=2&&host_reply[1]>n)note("host write ack exceeds request");return 0;}
 *put=host_reply[1];return 1;
}
static void put(char c){if(snapshot_length<SNAPSHOT)snapshot[snapshot_length++]=c;}
static void put_text(const char *s){while(*s)put(*s++);}
static void put_number(uint32_t n){char b[11];unsigned i=0;do{b[i++]=(char)('0'+n%10);n/=10;}while(n);while(i)put(b[--i]);}
static void render_tasks(void){
 static const char *const states[]={"free","ready","sleeping","done","failed","killed"};
 snapshot_length=snapshot_offset=0;
 for(unsigned id=1;id<=FORTH_TASKS_MAX;++id){
  struct forth_task_info t;if(!forth_task_info(id,&t))continue;
  put_number(id);put(' ');put_text(t.state<6?states[t.state]:"?");put(' ');put_text(t.name[0]?t.name:"-");
  put(' ');put_number(t.slices);put(' ');put_number(t.steps);put(' ');put_text(t.error[0]?t.error:"-");put('\n');
 }
}
static void release(int i){
 if(handles[i].kind==H_FILE)storage_stream_close(handles[i].slot);
 else if(handles[i].kind==H_HOST)(void)host_close(handles[i].slot);
 else if(handles[i].kind==H_PIPE){if(handles[i].mode==1)--pipes[handles[i].slot].readers;else --pipes[handles[i].slot].writers;
  if(!pipes[handles[i].slot].readers&&!pipes[handles[i].slot].writers)pipes[handles[i].slot].used=0;}
 else if(handles[i].kind==H_SNAPSHOT)snapshot_owner=-1;
 handles[i].kind=H_FREE;
}
static int slot(void){for(int i=0;i<HANDLES;++i)if(handles[i].kind==H_FREE)return i;return -1;}
static void claim(int i,unsigned char kind,unsigned char mode,unsigned char s,const char *path){
 handles[i].kind=kind;handles[i].mode=mode;handles[i].slot=s;handles[i].owner=(unsigned char)forth_current_task();handles[i].moved=0;
 unsigned n=0;while(path[n]&&n+1<sizeof handles[i].path){handles[i].path[n]=path[n];++n;}handles[i].path[n]=0;
}
static int open_native(void){
 fcell mode=fvm_pop(),length=fvm_pop(),address=fvm_pop();if(fvm_error)return forth_error("data stack underflow");
 char path[128];if(mode<1||mode>2)return push(code(E_MODE,"mode must be 1 read or 2 write"));
 if(!path_copy(address,length,path,sizeof path)){fvm_error=0;return push(code(E_PATH,"invalid path"));}
 int i=slot();if(i<0)return push(code(E_FULL,"no free handle"));
 if(path[0]=='/'&&path[1]=='d'&&path[2]=='e'&&path[3]=='v'&&path[4]=='/'){
  const char *d=path+5;
  if(same(d,"null")){claim(i,H_NULL,(unsigned char)mode,0,path);return push((fcell)(i+1));}
  if(same(d,"cons")){if(mode!=2)return push(code(E_MODE,"/dev/cons is write only"));claim(i,H_CONS,2,0,path);return push((fcell)(i+1));}
  if(same(d,"tasks")){
   if(mode!=1)return push(code(E_MODE,"/dev/tasks is read only"));if(snapshot_owner>=0)return push(code(E_FULL,"snapshot device busy"));
   render_tasks();snapshot_owner=i;claim(i,H_SNAPSHOT,1,0,path);return push((fcell)(i+1));
  }
  for(unsigned k=0;k<DEVICES;++k)if(devices[k].xt&&same(d,devices[k].name)){
   /* Pipe between caller and a fresh server task; each end is owned by its user. */
   unsigned p=PIPES;for(unsigned q=0;q<PIPES;++q)if(!pipes[q].used){p=q;break;}
   handles[i].kind=H_PIPE;int other=slot();handles[i].kind=H_FREE;
   if(p==PIPES||other<0)return push(code(E_FULL,"no free pipe for device"));
   pipes[p].used=1;pipes[p].head=pipes[p].count=0;pipes[p].readers=pipes[p].writers=1;
   unsigned char near=(unsigned char)mode,far=(unsigned char)(mode==1?2:1);
   claim(i,H_PIPE,near,(unsigned char)p,path);claim(other,H_PIPE,far,(unsigned char)p,path);
   unsigned id=forth_task_start(devices[k].xt,far==1?(unsigned)other+1:0,far==2?(unsigned)other+1:0);
   if(!id){release(other);release(i);return push(code(E_FULL,"no free task for device"));}
   handles[other].owner=(unsigned char)id;return push((fcell)(i+1));
  }
  return push(code(E_PATH,"no such device"));
 }
 if(path[0]=='/'&&path[1]=='h'&&path[2]=='o'&&path[3]=='s'&&path[4]=='t'&&path[5]=='/'&&path[6]){
  unsigned char r[130];unsigned n=0;r[n++]='O';r[n++]=mode==1?'r':'w';for(const char *q=path+6;*q&&n<sizeof r;++q)r[n++]=(unsigned char)*q;
  if(!host_call(r,n)||host_reply_length<2)return push(code(E_PATH,last_error[0]?last_error:"host open failed"));
  claim(i,H_HOST,(unsigned char)mode,host_reply[1],path);return push((fcell)(i+1));
 }
 unsigned s=STORAGE_STREAMS;
 for(unsigned k=0;k<STORAGE_STREAMS;++k){int busy=0;for(int j=0;j<HANDLES;++j)if(handles[j].kind==H_FILE&&handles[j].slot==k)busy=1;if(!busy){s=k;break;}}
 if(s==STORAGE_STREAMS)return push(code(E_FULL,"no free file stream"));
 if(!storage_stream_open(s,path,mode==2))return push(code(E_PATH,storage_error()));
 claim(i,H_FILE,(unsigned char)mode,(unsigned char)s,path);return push((fcell)(i+1));
}
static int close_native(void){
 fcell h=fvm_pop();if(fvm_error)return forth_error("data stack underflow");
 int i=handle(h);if(i<0)return push(code(E_BAD,"bad handle"));
 int ok=1;
 if(handles[i].kind==H_FILE){ok=storage_stream_close(handles[i].slot);handles[i].kind=H_FREE;}
 else if(handles[i].kind==H_HOST){ok=host_close(handles[i].slot);handles[i].kind=H_FREE;}
 else release(i);
 return push(ok?0:code(E_IO,storage_error()));
}
static int transfer(int writing){
 fcell h=fvm_pop(),length=fvm_pop(),address=fvm_pop();if(fvm_error)return forth_error("data stack underflow");
 int i=handle(h);if(i<0)return push(code(E_BAD,"bad handle"));
 if((writing&&handles[i].mode!=2)||(!writing&&handles[i].mode!=1))return push(code(E_MODE,"wrong handle direction"));
 unsigned char kind=handles[i].kind;fcell done=0;
 if(kind==H_NULL)done=writing?length:0;
 else if(kind==H_CONS){for(;done<length;++done){unsigned char c=fvm_read8(address+done);if(fvm_error){fvm_error=0;return push(code(E_IO,"invalid memory"));}forth_emit(c);}}
 else if(kind==H_SNAPSHOT){for(;done<length&&snapshot_offset<snapshot_length;++done)fvm_write8(address+done,(unsigned char)snapshot[snapshot_offset++]);}
 else if(kind==H_PIPE){
  unsigned p=handles[i].slot;
  if(writing){
   if(!pipes[p].readers)return push(code(E_BROKEN,"broken pipe"));
   if(length&&pipes[p].count==PIPE_BYTES)return push(code(E_AGAIN,"pipe full"));
   for(;done<length&&pipes[p].count<PIPE_BYTES;++done){pipes[p].data[(pipes[p].head+pipes[p].count)%PIPE_BYTES]=fvm_read8(address+done);++pipes[p].count;}
  }else{
   if(length&&!pipes[p].count)return push(pipes[p].writers?code(E_AGAIN,"pipe empty"):0);
   for(;done<length&&pipes[p].count;++done){fvm_write8(address+done,pipes[p].data[pipes[p].head]);pipes[p].head=(unsigned short)((pipes[p].head+1)%PIPE_BYTES);--pipes[p].count;}
  }
 }
 else if(kind==H_HOST){
  unsigned char bounce[200];
  while(done<length){
   unsigned want=length-done>sizeof bounce?(unsigned)sizeof bounce:(unsigned)(length-done),moved=0;
   if(writing){for(unsigned k=0;k<want;++k)bounce[k]=fvm_read8(address+done+(fcell)k);if(!host_write(handles[i].slot,bounce,want,&moved))return push(done?done:code(E_IO,last_error));}
   else{if(!host_read(handles[i].slot,bounce,want,&moved))return push(done?done:code(E_IO,last_error));for(unsigned k=0;k<moved;++k)fvm_write8(address+done+(fcell)k,bounce[k]);}
   done+=moved;if(moved<want)break;
  }
 }
 else {
  unsigned char bounce[64];
  while(done<length){
   size_t want=length-done>sizeof bounce?sizeof bounce:(size_t)(length-done),moved=0;
   if(writing){for(size_t k=0;k<want;++k)bounce[k]=fvm_read8(address+done+(fcell)k);
    if(!storage_stream_write(handles[i].slot,bounce,want,&moved)){done+=(fcell)moved;handles[i].moved+=(uint32_t)done;return push(done?done:code(E_IO,storage_error()));}}
   else{if(!storage_stream_read(handles[i].slot,bounce,want,&moved))return push(done?done:code(E_IO,storage_error()));
    for(size_t k=0;k<moved;++k)fvm_write8(address+done+(fcell)k,bounce[k]);}
   done+=(fcell)moved;if(moved<want)break;
  }
 }
 if(fvm_error){fvm_error=0;return push(code(E_IO,"invalid memory"));}
 handles[i].moved+=(uint32_t)done;return push(done);
}
static int read_native(void){return transfer(0);}
static int write_native(void){return transfer(1);}
static int pipe_native(void){
 unsigned p=PIPES;for(unsigned k=0;k<PIPES;++k)if(!pipes[k].used){p=k;break;}
 int r=slot();if(p==PIPES||r<0)return push(code(E_FULL,"no free pipe"))&&push(0);
 handles[r].kind=H_PIPE; /* reserve before searching for the writer slot */
 int w=slot();if(w<0){handles[r].kind=H_FREE;return push(code(E_FULL,"no free handle"))&&push(0);}
 pipes[p].used=1;pipes[p].head=pipes[p].count=0;pipes[p].readers=pipes[p].writers=1;
 claim(r,H_PIPE,1,(unsigned char)p,"pipe");claim(w,H_PIPE,2,(unsigned char)p,"pipe");
 return push((fcell)(r+1))&&push((fcell)(w+1));
}
/* Byte I/O for redirected task stdin/stdout. */
static int io_write(unsigned h,unsigned char c){
 int i=handle(h);if(i<0||handles[i].mode!=2)return -1;
 if(handles[i].kind==H_NULL){++handles[i].moved;return 1;}
 if(handles[i].kind==H_CONS){forth_emit(c);++handles[i].moved;return 1;}
 if(handles[i].kind==H_PIPE){unsigned p=handles[i].slot;if(!pipes[p].readers)return -1;if(pipes[p].count==PIPE_BYTES)return 0;
  pipes[p].data[(pipes[p].head+pipes[p].count)%PIPE_BYTES]=c;++pipes[p].count;++handles[i].moved;return 1;}
 if(handles[i].kind==H_FILE){size_t put=0;if(!storage_stream_write(handles[i].slot,&c,1,&put)||put!=1)return -1;++handles[i].moved;return 1;}
 if(handles[i].kind==H_HOST){unsigned put=0;if(!host_write(handles[i].slot,&c,1,&put)||put!=1)return -1;++handles[i].moved;return 1;}
 return -1;
}
static int io_read(unsigned h,unsigned char *c){
 int i=handle(h);if(i<0||handles[i].mode!=1)return -1;
 if(handles[i].kind==H_PIPE){unsigned p=handles[i].slot;if(!pipes[p].count)return pipes[p].writers?0:-1;
  *c=pipes[p].data[pipes[p].head];pipes[p].head=(unsigned short)((pipes[p].head+1)%PIPE_BYTES);--pipes[p].count;++handles[i].moved;return 1;}
 if(handles[i].kind==H_SNAPSHOT){if(snapshot_offset>=snapshot_length)return -1;*c=(unsigned char)snapshot[snapshot_offset++];++handles[i].moved;return 1;}
 if(handles[i].kind==H_FILE){size_t got=0;if(!storage_stream_read(handles[i].slot,c,1,&got))return -1;if(!got)return -1;++handles[i].moved;return 1;}
 if(handles[i].kind==H_HOST){unsigned got=0;if(!host_read(handles[i].slot,c,1,&got)||!got)return -1;++handles[i].moved;return 1;}
 return -1; /* null: EOF */
}
static int io_give(unsigned h,unsigned task){int i=handle(h);if(i<0)return 0;handles[i].owner=(unsigned char)task;return 1;}
/* ( addr u xt -- ) register or replace /dev/NAME; xt 0 removes it. */
static int device_native(void){
 fcell xt=fvm_pop(),length=fvm_pop(),address=fvm_pop();if(fvm_error)return forth_error("data stack underflow");
 char name[12];if(!path_copy(address,length,name,sizeof name)){fvm_error=0;return forth_error("device name is 1..11 printable bytes");}
 int free_slot=-1;
 for(unsigned k=0;k<DEVICES;++k){if(devices[k].xt&&same(devices[k].name,name)){devices[k].xt=xt;if(!xt)devices[k].name[0]=0;return 1;}if(!devices[k].xt&&free_slot<0)free_slot=(int)k;}
 if(!xt)return 1;if(free_slot<0)return forth_error("device table full");
 unsigned n=0;while(name[n]){devices[free_slot].name[n]=name[n];++n;}devices[free_slot].name[n]=0;devices[free_slot].xt=xt;return 1;
}
static void task_exit(unsigned id){for(int i=0;i<HANDLES;++i)if(handles[i].kind!=H_FREE&&handles[i].owner==id)release(i);}
/* Whole-file read from the Mac for LOAD/INCLUDE /host/...; 1 ok, 0 failed. */
int vios_host_read_file(const char *path,char *buffer,size_t capacity,size_t *size){
 *size=0;if(!(path[0]=='/'&&path[1]=='h'&&path[2]=='o'&&path[3]=='s'&&path[4]=='t'&&path[5]=='/'&&path[6]))return 0;
 unsigned char r[130];unsigned n=0;r[n++]='O';r[n++]='r';for(const char *q=path+6;*q&&n<sizeof r;++q)r[n++]=(unsigned char)*q;
 if(!host_call(r,n)||host_reply_length<2)return 0;unsigned remote=host_reply[1];int ok=1;
 for(;;){unsigned got=0;if(*size==capacity){ok=0;note("host file exceeds buffer");break;}
  if(!host_read(remote,(unsigned char *)buffer+*size,(unsigned)(capacity-*size>200?200:capacity-*size),&got)){ok=0;break;}
  if(!got)break;*size+=got;}
 (void)host_close(remote);return ok;
}
const char *vios_host_error(void){return last_error;}
static void list_entry(const char *name,unsigned long size,int directory,void *ctx){
 (void)ctx;vios_out("entry ");vios_out(name);vios_out(" size ");vios_uint((uint32_t)size);vios_out(directory?" dir 1\r\n":" dir 0\r\n");
}
static int bad(const char *message){forth_error(message);return -1;}
static int hex_digit(char c){return c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:-1;}
/* Host file service, one line per call:
 *   DIR path            -> "entry NAME size N dir 0|1" lines
 *   HANDLE-PUT h HEX    -> "put N" bytes written (pipes may be short)
 *   HANDLE-GET h n      -> "get HEX|-" (n<=64; - means EOF) */
int vios_handles_command(const char *word,const char **rest){
 char a[128],b[236];
 if(vios_same(word,"DIR")){if(!vios_arg(rest,a,sizeof a))return -1;return storage_list(a,list_entry,0)?1:bad(storage_error());}
 if(vios_same(word,"HANDLE-PUT")||vios_same(word,"HANDLE-GET")){
  if(!vios_arg(rest,a,16)||!vios_arg(rest,b,sizeof b))return -1;
  unsigned h=0;for(const char *p=a;*p;++p){if(*p<'0'||*p>'9')return bad("handle number required");h=h*10+(unsigned)(*p-'0');}
  int i=handle(h);if(i<0)return bad("bad handle");
  if(vios_same(word,"HANDLE-PUT")){
   unsigned n=0;unsigned char c=0;size_t len=vios_length(b);if(len&1)return bad("hex must have even length");
   for(size_t k=0;k<len;k+=2){int x=hex_digit(b[k]),y=hex_digit(b[k+1]);if(x<0||y<0)return bad("invalid hex");c=(unsigned char)(x*16+y);
    int r=io_write(h,c);if(r<0)return bad("handle write failed");if(!r)break;++n;}
   vios_out("put ");vios_uint(n);vios_out("\r\n");return 1;
  }
  unsigned want=0;for(const char *p=b;*p;++p){if(*p<'0'||*p>'9')return bad("byte count required");want=want*10+(unsigned)(*p-'0');}
  if(!want||want>64)return bad("HANDLE-GET reads 1..64 bytes");
  vios_out("get ");unsigned n=0;unsigned char c;int r=0;
  while(n<want&&(r=io_read(h,&c))>0){vios_hex(c,2);++n;}
  if(!n)vios_out(r<0?"-":"");vios_out("\r\n");return 1;
 }
 return 0;
}
void vios_handles_report(void){
 static const char *const kinds[]={"free","file","pipe","null","cons","snapshot","host"};
 for(int i=0;i<HANDLES;++i){if(handles[i].kind==H_FREE)continue;
  vios_out("handle ");vios_uint((uint32_t)i+1);vios_out(" kind ");vios_out(kinds[handles[i].kind]);vios_out(handles[i].mode==1?" read":" write");
  vios_out(" owner ");vios_uint(handles[i].owner);vios_out(" bytes ");vios_uint(handles[i].moved);
  if(handles[i].kind==H_PIPE){vios_out(" buffered ");vios_uint(pipes[handles[i].slot].count);}
  vios_out(" path ");vios_out(handles[i].path);vios_out("\r\n");}
 for(unsigned k=0;k<DEVICES;++k)if(devices[k].xt){vios_out("device /dev/");vios_out(devices[k].name);vios_out(" xt ");vios_uint((uint32_t)devices[k].xt);vios_out("\r\n");}
 vios_out("io-error ");vios_out(last_error[0]?last_error:"-");vios_out("\r\n");
}
int vios_handles_init(void){
 for(int i=0;i<HANDLES;++i){if(handles[i].kind==H_FILE)storage_stream_close(handles[i].slot);handles[i].kind=H_FREE;}
 for(unsigned p=0;p<PIPES;++p)pipes[p].used=0;snapshot_owner=-1;last_error[0]=0;
 for(unsigned k=0;k<DEVICES;++k){devices[k].xt=0;devices[k].name[0]=0;}
 forth_register_task_exit(task_exit);forth_register_task_io(io_write,io_read,io_give);
 static const struct {const char *name;int (*fn)(void);} natives[]={{"OPEN",open_native},{"CLOSE",close_native},{"READ",read_native},{"WRITE",write_native},{"PIPE",pipe_native},{"DEVICE",device_native}};
 for(unsigned k=0;k<sizeof natives/sizeof natives[0];++k)if(!forth_register_native(natives[k].name,natives[k].fn,FORTH_NATIVE_TASK))return 0;
 /* Blocking is replaceable Forth policy over the non-blocking primitives. */
 return forth_eval(": READ-WAIT >R BEGIN 2DUP R@ READ DUP -1 = WHILE DROP PAUSE REPEAT R> DROP >R 2DROP R> ; "
  ": WRITE-ALL >R BEGIN DUP WHILE 2DUP R@ WRITE DUP -1 = IF DROP PAUSE ELSE DUP 0 < IF R> DROP >R 2DROP R> EXIT THEN "
  "DUP >R - SWAP R> + SWAP THEN REPEAT R> DROP 2DROP 0 ; "
  /* ( xt1 xt2 -- id1 id2 ) xt1's output feeds xt2's KEY; xt2 prints to the
   * console. Each end is owned by its task, so EOF follows the producer. */
  "VARIABLE PL-R VARIABLE PL-W "
  /* CRITICAL: no tick may run a task between its SPAWN and its wiring (with
   * preemption the producer could finish before TASK-OUTPUT: soak 2026-10-20). */
  ": PIPELINE CRITICAL( PIPE PL-W ! PL-R ! SPAWN-CON DUP PL-R @ TASK-INPUT SWAP SPAWN DUP PL-W @ TASK-OUTPUT SWAP )CRITICAL ;");
}

/* Handles over SD files, /dev endpoints and pipes. Non-blocking natives only;
 * blocking is Forth policy (READ-WAIT/WRITE-ALL). See
 * docs/superpowers/specs/2026-10-18-vios-handles.md. */
#include "vios/runtime.h"
#include "platform/storage.h"
#define HANDLES 8
#define PIPES 4
#define PIPE_BYTES 128
#define SNAPSHOT 512
enum {H_FREE,H_FILE,H_PIPE,H_NULL,H_CONS,H_SNAPSHOT};
enum {E_AGAIN=1,E_BROKEN=2,E_BAD=3,E_FULL=4,E_PATH=5,E_IO=6,E_MODE=7};
static struct {unsigned char kind,mode,owner,slot;uint32_t moved;char path[24];} handles[HANDLES];
static struct {unsigned char data[PIPE_BYTES];unsigned short head,count;unsigned char readers,writers,used;} pipes[PIPES];
static char snapshot[SNAPSHOT];static unsigned short snapshot_length,snapshot_offset;static int snapshot_owner=-1;
static char last_error[48];
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
  return push(code(E_PATH,"no such device"));
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
 if(handles[i].kind==H_FILE){ok=storage_stream_close(handles[i].slot);handles[i].kind=H_FREE;}else release(i);
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
 return -1;
}
static int io_read(unsigned h,unsigned char *c){
 int i=handle(h);if(i<0||handles[i].mode!=1)return -1;
 if(handles[i].kind==H_PIPE){unsigned p=handles[i].slot;if(!pipes[p].count)return pipes[p].writers?0:-1;
  *c=pipes[p].data[pipes[p].head];pipes[p].head=(unsigned short)((pipes[p].head+1)%PIPE_BYTES);--pipes[p].count;++handles[i].moved;return 1;}
 if(handles[i].kind==H_SNAPSHOT){if(snapshot_offset>=snapshot_length)return -1;*c=(unsigned char)snapshot[snapshot_offset++];++handles[i].moved;return 1;}
 if(handles[i].kind==H_FILE){size_t got=0;if(!storage_stream_read(handles[i].slot,c,1,&got))return -1;if(!got)return -1;++handles[i].moved;return 1;}
 return -1; /* null: EOF */
}
static int io_give(unsigned h,unsigned task){int i=handle(h);if(i<0)return 0;handles[i].owner=(unsigned char)task;return 1;}
static void task_exit(unsigned id){for(int i=0;i<HANDLES;++i)if(handles[i].kind!=H_FREE&&handles[i].owner==id)release(i);}
void vios_handles_report(void){
 static const char *const kinds[]={"free","file","pipe","null","cons","snapshot"};
 for(int i=0;i<HANDLES;++i){if(handles[i].kind==H_FREE)continue;
  vios_out("handle ");vios_uint((uint32_t)i+1);vios_out(" kind ");vios_out(kinds[handles[i].kind]);vios_out(handles[i].mode==1?" read":" write");
  vios_out(" owner ");vios_uint(handles[i].owner);vios_out(" bytes ");vios_uint(handles[i].moved);
  if(handles[i].kind==H_PIPE){vios_out(" buffered ");vios_uint(pipes[handles[i].slot].count);}
  vios_out(" path ");vios_out(handles[i].path);vios_out("\r\n");}
 vios_out("io-error ");vios_out(last_error[0]?last_error:"-");vios_out("\r\n");
}
int vios_handles_init(void){
 for(int i=0;i<HANDLES;++i){if(handles[i].kind==H_FILE)storage_stream_close(handles[i].slot);handles[i].kind=H_FREE;}
 for(unsigned p=0;p<PIPES;++p)pipes[p].used=0;snapshot_owner=-1;last_error[0]=0;
 forth_register_task_exit(task_exit);forth_register_task_io(io_write,io_read,io_give);
 static const struct {const char *name;int (*fn)(void);} natives[]={{"OPEN",open_native},{"CLOSE",close_native},{"READ",read_native},{"WRITE",write_native},{"PIPE",pipe_native}};
 for(unsigned k=0;k<sizeof natives/sizeof natives[0];++k)if(!forth_register_native(natives[k].name,natives[k].fn,FORTH_NATIVE_TASK))return 0;
 /* Blocking is replaceable Forth policy over the non-blocking primitives. */
 return forth_eval(": READ-WAIT >R BEGIN 2DUP R@ READ DUP -1 = WHILE DROP PAUSE REPEAT R> DROP >R 2DROP R> ; "
  ": WRITE-ALL >R BEGIN DUP WHILE 2DUP R@ WRITE DUP -1 = IF DROP PAUSE ELSE DUP 0 < IF R> DROP >R 2DROP R> EXIT THEN "
  "DUP >R - SWAP R> + SWAP THEN REPEAT R> DROP 2DROP 0 ; "
  /* ( xt1 xt2 -- id1 id2 ) xt1's output feeds xt2's KEY; xt2 prints to the
   * console. Each end is owned by its task, so EOF follows the producer. */
  "VARIABLE PL-R VARIABLE PL-W "
  ": PIPELINE PIPE PL-W ! PL-R ! SPAWN-CON DUP PL-R @ TASK-INPUT SWAP SPAWN DUP PL-W @ TASK-OUTPUT SWAP ;");
}

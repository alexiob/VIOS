/* Trial/confirm/revert source bundles. CRC records are journaled in two files.
 * Both slots share FAT32: this is update rollback, not media fault tolerance. */
#include "vios/runtime.h"
#include "platform/storage.h"
#include "platform/vdp.h"
#define NONE 255u
#define CLEAN 0u
#define PENDING 1u
#define TRIED 2u
#define ENTRY_SIZE 164u
#define BUNDLE_MAX (16+ENTRY_SIZE*FORTH_MODULES_MAX+4)
struct boot_state {unsigned confirmed,previous,candidate,state;uint32_t generation,crc[2];};
static struct boot_state boot,committed;
/* Where startup time goes (BOOT-TIMES): ESP clock stamps (ms, one VDP round
 * trip each) at each step of vios_boot_startup, and the slowest module. */
enum {MARK_START,MARK_STORAGE,MARK_STATE,MARK_MODULES,MARK_HEALTH,MARK_READY,MARKS};
static uint32_t marks[MARKS],module_slowest,module_loads;static char module_slowest_name[32];
static void mark(unsigned at){marks[at]=platform_millis();}
extern uint32_t vios_module_read_ms,vios_module_check_ms,vios_module_compile_ms;
static unsigned running=NONE;
static unsigned char bundle[BUNDLE_MAX],checked[BUNDLE_MAX];
static const char *slot_path(unsigned slot){return slot?"/VIOS/BOOT_B.VIB":"/VIOS/BOOT_A.VIB";}
static const char *slot_name(unsigned slot){return slot==0?"A":slot==1?"B":"NONE";}
static void clear_state(void){boot.confirmed=boot.previous=boot.candidate=NONE;boot.state=CLEAN;boot.generation=boot.crc[0]=boot.crc[1]=0;}
static int valid_slot(unsigned n){return n==0||n==1||n==NONE;}
static int read_record(const char *path,unsigned char *record){
 size_t size=0;if(!storage_read_file(path,record,32,&size)||size!=32)return 0;
 if(record[0]!='V'||record[1]!='I'||record[2]!='R'||record[3]!='1'||record[4]!=1||!valid_slot(record[5])||!valid_slot(record[6])||!valid_slot(record[7])||record[8]>TRIED)return 0;
 return vios_get32(record+28)==vios_crc32(record,28);
}
static void read_state(void){
 unsigned char a[32],b[32];int av=read_record("/VIOS/BOOT0.REC",a),bv=read_record("/VIOS/BOOT1.REC",b);clear_state();
 if(!av&&!bv){committed=boot;return;}
 unsigned char *r=!av?b:!bv?a:vios_get32(a+12)>=vios_get32(b+12)?a:b;
 boot.confirmed=r[5];boot.previous=r[6];boot.candidate=r[7];boot.state=r[8];boot.generation=vios_get32(r+12);boot.crc[0]=vios_get32(r+16);boot.crc[1]=vios_get32(r+20);committed=boot;
}
static int record_error(const char *message){boot=committed;return forth_error(message);}
static int write_state(void){
 if(boot.generation==UINT32_MAX)return record_error("boot record generation exhausted");
 unsigned char r[32],verify[32];for(unsigned i=0;i<32;++i)r[i]=0;
 r[0]='V';r[1]='I';r[2]='R';r[3]='1';r[4]=1;r[5]=(unsigned char)boot.confirmed;r[6]=(unsigned char)boot.previous;r[7]=(unsigned char)boot.candidate;r[8]=(unsigned char)boot.state;
 uint32_t generation=boot.generation+1;vios_put32(r+12,generation);vios_put32(r+16,boot.crc[0]);vios_put32(r+20,boot.crc[1]);vios_put32(r+28,vios_crc32(r,28));
 const char *path=(generation&1)?"/VIOS/BOOT1.REC":"/VIOS/BOOT0.REC";
 if(!storage_write_file(path,r,sizeof r)||!storage_sync()||!read_record(path,verify))return record_error("boot record write/readback failed");
 for(unsigned i=0;i<sizeof r;++i)if(r[i]!=verify[i])return record_error("boot record readback mismatch");
 boot.generation=generation;committed=boot;return 1;
}
static int field_terminated(const unsigned char *p,unsigned capacity){for(unsigned i=0;i<capacity;++i)if(!p[i])return 1;return 0;}
/* Verify a slot bundle into b without side effects; NULL or why it is unusable. */
static const char *read_slot(unsigned slot,unsigned char *b){
 size_t size=0;if(!storage_read_file(slot_path(slot),b,BUNDLE_MAX,&size))return storage_error();
 if(size<20||b[0]!='V'||b[1]!='I'||b[2]!='B'||b[3]!='1'||b[4]!=1||b[5]!=24||b[6]>FORTH_MODULES_MAX)return "invalid/incompatible boot bundle";
 if(size!=16+(size_t)ENTRY_SIZE*b[6]+4)return "boot bundle length mismatch";
 uint32_t crc=vios_get32(b+size-4);
 if(crc!=vios_crc32(b,size-4)||crc!=boot.crc[slot])return "boot bundle CRC mismatch";
 for(unsigned i=0;i<b[6];++i){const unsigned char *e=b+16+(size_t)i*ENTRY_SIZE;if(!field_terminated(e,32)||!field_terminated(e+32,128))return "unterminated boot bundle entry";}
 return 0;
}
static int bundle_identity(void);
/* Load the modules of the bundle already read (and checked) into `bundle`. */
static int load_modules(unsigned slot){
 for(unsigned i=0;i<bundle[6];++i){const unsigned char *e=bundle+16+(size_t)i*ENTRY_SIZE;
  uint32_t before=platform_millis();
  if(!vios_module_load((const char *)e+32))return 0;
  uint32_t spent=platform_millis()-before;++module_loads;
  if(spent>module_slowest){module_slowest=spent;size_t n=0;while(n<31&&e[n]){module_slowest_name[n]=(char)e[n];++n;}module_slowest_name[n]=0;}
 }
 if(!bundle_identity())return 0;
 running=slot;return 1;
}
/* Boot-module cache (docs/superpowers/specs/2026-10-19-vios-boot-speed.md,
 * Phase 3): a snapshot of what the slot's module loads added, keyed by the
 * resident build stamp, the slot's bundle CRC and the base dictionary state.
 * File: 'VIC1', stamp, bundle CRC, base (6 x u32), the core snapshot, the
 * module snapshot, CRC-32 of everything before it. Sources stay the truth:
 * any mismatch loads the slot from its packages and rewrites the cache. */
/* Resident build stamp: tools/vios_stamp.py writes the CRC-32 of the binary
 * (taken with this word zero) after the 'STMP' marker. 0 = unstamped (an ELF
 * load, a host build): never cached. */
volatile uint32_t vios_build_stamp[2]={0x504D5453UL,0};   /* not const: host tests stamp it */
#define CACHE_STREAM (STORAGE_STREAMS-1)   /* no handles are open during boot */
static uint32_t cache_crc,cache_hits,cache_misses,cache_saves;static const char *cache_note="none";
static const char *cache_path(unsigned slot,int temporary){
 return temporary?(slot?"/VIOS/CACHE_B.TMP":"/VIOS/CACHE_A.TMP"):(slot?"/VIOS/CACHE_B.VIC":"/VIOS/CACHE_A.VIC");
}
static int cache_put(void *data,size_t size,void *ctx){
 (void)ctx;cache_crc=forth_crc32_update(cache_crc,(const unsigned char *)data,size);
 while(size){size_t put=0;if(!storage_stream_write(CACHE_STREAM,data,size,&put)||!put)return 0;data=(unsigned char *)data+put;size-=put;}
 return 1;
}
static int cache_get(void *data,size_t size,void *ctx){
 (void)ctx;unsigned char *p=(unsigned char *)data;size_t want=size;
 while(want){size_t got=0;if(!storage_stream_read(CACHE_STREAM,p,want,&got)||!got)return 0;p+=got;want-=got;}
 cache_crc=forth_crc32_update(cache_crc,(const unsigned char *)data,size);return 1;
}
static void cache_header(unsigned char *h,unsigned slot,const struct forth_snapshot_base *b){
 h[0]='V';h[1]='I';h[2]='C';h[3]='1';vios_put32(h+4,vios_build_stamp[1]);vios_put32(h+8,boot.crc[slot]);
 const uint32_t v[6]={b->token,b->direct,b->data,b->source,b->count,b->map};
 for(unsigned i=0;i<6;++i)vios_put32(h+12+4*i,v[i]);
}
/* 1: the slot's state is restored from its cache. 0 with *clobbered set:
 * the read failed part way and the runtime must be reinitialised. */
static int cache_load(unsigned slot,const struct forth_snapshot_base *base,int *clobbered){
 *clobbered=0;if(!vios_build_stamp[1]){cache_note="unstamped";return 0;}
 if(!storage_stream_open(CACHE_STREAM,cache_path(slot,0),0)){cache_note="absent";return 0;}
 unsigned char want[36],have[36];cache_header(want,slot,base);cache_crc=0;
 int ok=cache_get(have,sizeof have,0);
 for(unsigned i=0;ok&&i<sizeof have;++i)if(have[i]!=want[i])ok=0;
 if(!ok){(void)storage_stream_close(CACHE_STREAM);cache_note="stale";return 0;}
 *clobbered=1;
 ok=forth_snapshot_read(base,cache_get,0)&&forth_modules_snapshot_read(cache_get,0);
 uint32_t expected=cache_crc;unsigned char tail[4];
 ok=ok&&cache_get(tail,4,0)&&vios_get32(tail)==expected;
 unsigned char extra;size_t got=0;if(ok&&storage_stream_read(CACHE_STREAM,&extra,1,&got)&&got)ok=0;   /* trailing bytes */
 (void)storage_stream_close(CACHE_STREAM);
 if(!ok){cache_note="corrupt";return 0;}
 *clobbered=0;cache_note="hit";++cache_hits;return 1;
}
static void cache_save(unsigned slot,const struct forth_snapshot_base *base){
 if(!vios_build_stamp[1])return;
 const char *temporary=cache_path(slot,1),*path=cache_path(slot,0);
 unsigned char h[36];cache_header(h,slot,base);cache_crc=0;
 if(!storage_stream_open(CACHE_STREAM,temporary,1)){cache_note="save failed";return;}
 int ok=cache_put(h,sizeof h,0)&&forth_snapshot_write(base,cache_put,0)&&forth_modules_snapshot_write(cache_put,0);
 unsigned char tail[4];vios_put32(tail,cache_crc);ok=ok&&cache_put(tail,4,0);
 ok=storage_stream_close(CACHE_STREAM)&&ok;
 (void)storage_remove(path);
 if(!ok||!storage_rename(temporary,path)){(void)storage_remove(temporary);cache_note="save failed";return;}
 ++cache_saves;cache_note="saved";
}
static int bundle_identity(void){
 for(unsigned i=0;i<bundle[6];++i){const unsigned char *e=bundle+16+(size_t)i*ENTRY_SIZE;
  struct forth_module_info info;int found=0;for(unsigned n=0;n<forth_module_count();++n)if(forth_module_info(n,&info)&&vios_same(info.name,(const char *)e)&&info.source_crc32==vios_get32(e+160)){found=1;break;}
  if(!found)return forth_error("boot package identity mismatch");
 }
 return 1;
}
/* load_slot through the cache: the bundle is still read and checked against
 * the boot record first, so a slot is accepted exactly when it is today. */
static int fresh_runtime(void);
static int load_slot_cached(unsigned slot){
 if(slot>1)return 0;
 struct forth_snapshot_base base;forth_snapshot_base(&base);
 const char *why=read_slot(slot,bundle);if(why)return forth_error(why);
 int clobbered=0;
 if(cache_load(slot,&base,&clobbered)){
  if(bundle_identity()){running=slot;return 1;}
  clobbered=1;cache_note="identity";
 }
 ++cache_misses;
 if(clobbered){if(!fresh_runtime())return 0;forth_snapshot_base(&base);}
 if(!load_modules(slot))return 0;
 cache_save(slot,&base);return 1;
}
static int make_bundle(unsigned slot){
 unsigned count=forth_module_count();unsigned char order[FORTH_MODULES_MAX],used[FORTH_MODULES_MAX];
 if(!count)return forth_error("no active source modules to persist");
 for(unsigned i=0;i<count;++i)used[i]=0;
 for(unsigned next=0;next<count;++next){int chosen=-1;
  for(unsigned i=0;i<count&&chosen<0;++i)if(!used[i]){
   struct forth_module_info info;forth_module_info(i,&info);int ready=1;
   for(unsigned d=0;d<info.dependency_count;++d){struct forth_module_dependency dep;forth_module_dependency_at(info.name,0,d,&dep);int found=-1;
    for(unsigned j=0;j<count;++j){struct forth_module_info target;forth_module_info(j,&target);if(vios_same(dep.name,target.name)&&dep.source_crc32==target.source_crc32){found=(int)j;break;}}
    if(found<0)return forth_error("boot bundle has missing/stale dependency");if(!used[found])ready=0;
   }
   if(ready)chosen=(int)i;
  }
  if(chosen<0)return forth_error("boot bundle dependency cycle");order[next]=(unsigned char)chosen;used[chosen]=1;
 }
 size_t size=16+(size_t)ENTRY_SIZE*count+4;for(size_t i=0;i<size;++i)bundle[i]=0;
 bundle[0]='V';bundle[1]='I';bundle[2]='B';bundle[3]='1';bundle[4]=1;bundle[5]=24;bundle[6]=(unsigned char)count;
 for(unsigned i=0;i<count;++i){struct forth_module_info info;forth_module_info(order[i],&info);
  if(!vios_module_save(info.name,NULL))return 0;
  unsigned char *e=bundle+16+(size_t)i*ENTRY_SIZE;
  size_t n=vios_length(info.name);for(size_t j=0;j<n;++j)e[j]=(unsigned char)info.name[j];
  if(!vios_module_path(info.name,(char *)e+32,128))return 0;vios_put32(e+160,info.source_crc32);
 }
 uint32_t crc=vios_crc32(bundle,size-4);vios_put32(bundle+size-4,crc);
 size_t actual=0;
 if(!storage_write_file(slot_path(slot),bundle,size)||!storage_sync()||!storage_read_file(slot_path(slot),checked,sizeof checked,&actual))return forth_error(storage_error());
 if(actual!=size)return forth_error("boot bundle readback length mismatch");for(size_t i=0;i<size;++i)if(bundle[i]!=checked[i])return forth_error("boot bundle readback mismatch");
 boot.crc[slot]=crc;return 1;
}
int vios_boot_trial(void){
 read_state();unsigned slot=boot.confirmed==0?1:0;
 if(!storage_mkdir("/VIOS"))return forth_error(storage_error());
 /* Persist loss of the old fallback identity before reusing its storage. */
 if(boot.previous==slot){boot.previous=NONE;if(!write_state())return 0;}
 if(!make_bundle(slot))return 0;
 boot.candidate=slot;boot.state=PENDING;return write_state();
}
int vios_boot_confirm(void){
 if(boot.state==CLEAN&&running==boot.confirmed&&running!=NONE)return 1;
 if(boot.state!=TRIED||running!=boot.candidate||running==NONE)return forth_error("no running trial boot to confirm");
 boot.previous=boot.confirmed;boot.confirmed=running;boot.candidate=NONE;boot.state=CLEAN;return write_state();
}
int vios_boot_revert(void){
 read_state();if(boot.state!=CLEAN){boot.candidate=NONE;boot.state=CLEAN;return write_state();}
 if(boot.previous==NONE)return forth_error("no previous confirmed boot slot");
 unsigned previous=boot.previous;boot.previous=boot.confirmed;boot.confirmed=previous;return write_state();
}
void vios_boot_status(void){
 read_state();vios_out("boot confirmed ");vios_out(slot_name(boot.confirmed));vios_out(" candidate ");vios_out(slot_name(boot.candidate));
 vios_out(" state ");vios_out(boot.state==CLEAN?"CLEAN":boot.state==PENDING?"PENDING":"TRIED");vios_out(" current ");vios_out(slot_name(running));vios_out(" generation ");vios_uint(boot.generation);vios_out("\r\n");
}
/* Read-only: what a power cycle would load. Problems are reported, never raised. */
static void manifest_slot(unsigned slot,const char *role){
 vios_out("manifest ");vios_out(slot_name(slot));vios_out(" ");vios_out(role);
 const char *why=slot<2?read_slot(slot,checked):0;
 if(slot>1)vios_out(" none\r\n");else if(why){vios_out(" error ");vios_out(why);vios_out("\r\n");}
 else{vios_out(" modules ");vios_uint(checked[6]);vios_out(" crc32 ");vios_uint(boot.crc[slot]);vios_out("\r\n");}
 if(why||slot>1)return;
 for(unsigned i=0;i<checked[6];++i){const unsigned char *e=checked+16+(size_t)i*ENTRY_SIZE;char version[32];uint32_t crc=vios_get32(e+160);
  int package=vios_module_package_version((const char *)e+32,crc,version);
  vios_out("manifest-module ");vios_out(slot_name(slot));vios_out(" ");vios_out((const char *)e);vios_out(" version ");vios_out(package>0?version:"-");
  vios_out(" crc32 ");vios_uint(crc);vios_out(" package ");vios_out(package>0?"ok":package?"bad":"unreadable");vios_out("\r\n");
 }
}
void vios_boot_manifest(void){read_state();manifest_slot(boot.confirmed,"confirmed");manifest_slot(boot.candidate,"candidate");}
static int fresh_runtime(void){forth_init();if(!vios_runtime_init())return 0;
#ifdef __ez80__
 forth_set_engine(FORTH_DIRECT);
#endif
 return 1;
}
static int health(void){
 if(!forth_eval("1 2 + 3 =")||fvm_depth!=1||!fvm_pop())return forth_error("boot VM self-test failed");
 unsigned char command[]={23,0,0x86},data[8];unsigned n;
 if(!platform_vdp_request(command,sizeof command,0x86,data,sizeof data,&n)||n!=8)return forth_error("boot VDP health timeout");
 return storage_sync()?1:forth_error(storage_error());
}
/* Healthy: the MOS boot stub's unfinished-boot counter (/vios/boot.cnt,
 * arch/ez80/viosboot.s) starts over. No file, no card: nothing to clear. */
static void ready(void){(void)storage_remove("/vios/boot.cnt");mark(MARK_READY);platform_boot_ready();}
int vios_boot_startup(void){
 running=NONE;platform_boot_trial();mark(MARK_START);
 if(!storage_init()){vios_out("boot recovery: ");vios_out(storage_error());vios_out("\r\n");ready();return 1;}
 mark(MARK_STORAGE);read_state();mark(MARK_STATE);
 if(boot.state==PENDING&&boot.candidate<2){
  unsigned candidate=boot.candidate;boot.state=TRIED;
  if(write_state()&&load_slot_cached(candidate)&&(mark(MARK_MODULES),health())&&(mark(MARK_HEALTH),vios_boot_confirm())){
   vios_out("boot trial confirmed ");vios_out(slot_name(candidate));vios_out("\r\n");ready();return 1;
  }
  vios_out("boot candidate rejected: ");vios_out(forth_last_error());vios_out("\r\n");
 }
 if(boot.state!=CLEAN){boot.state=CLEAN;boot.candidate=NONE;(void)write_state();}
 unsigned choices[2]={boot.confirmed,boot.previous};
 for(unsigned i=0;i<2;++i)if(choices[i]<2){
  if(!fresh_runtime())return 0;
  if(load_slot_cached(choices[i])&&(mark(MARK_MODULES),health())){
   mark(MARK_HEALTH);
   if(i){boot.previous=boot.confirmed;boot.confirmed=choices[i];(void)write_state();}
   vios_out("boot confirmed ");vios_out(slot_name(running));vios_out("\r\n");ready();return 1;
  }
  vios_out("boot slot rejected: ");vios_out(forth_last_error());vios_out("\r\n");
 }
 if(!fresh_runtime())return 0;running=NONE;vios_out("boot recovery: built-in kernel, no valid source bundle\r\n");ready();return 1;
}
/* "boot-times storage S state T modules M (N loads, slowest NAME X) health H
 * finish F total ms": each step's duration, from the stamps above. */
static void boot_times(void){
 static const char *const names[]={"storage","state","modules","health","finish"};
 vios_out("boot-times");
 for(unsigned i=1;i<MARKS;++i){vios_out(" ");vios_out(names[i-1]);vios_out(" ");vios_uint(marks[i]&&marks[i-1]?marks[i]-marks[i-1]:0);
  if(i==MARK_MODULES){vios_out(" (");vios_uint(module_loads);vios_out(" loads, slowest ");vios_out(module_loads?module_slowest_name:"-");vios_out(" ");vios_uint(module_slowest);
   vios_out("; cache ");vios_out(cache_note);vios_out(" hits ");vios_uint(cache_hits);vios_out(" misses ");vios_uint(cache_misses);vios_out(" saves ");vios_uint(cache_saves);
   vios_out("; read ");vios_uint(vios_module_read_ms);vios_out(" check ");vios_uint(vios_module_check_ms);vios_out(" compile ");vios_uint(vios_module_compile_ms);vios_out(")");}}
 vios_out(" total ");vios_uint(marks[MARK_READY]-marks[MARK_START]);vios_out(" ms\r\n");
}
int vios_boot_command(const char *word,const char **rest){
 (void)rest;
 if(vios_same(word,"BOOT-TIMES")){boot_times();return 1;}
 if(vios_same(word,"BOOT-STATUS")){vios_boot_status();return 1;}
 if(vios_same(word,"BOOT-MANIFEST")){vios_boot_manifest();return 1;}
 if(vios_same(word,"BOOT-TRIAL"))return vios_boot_trial()?1:-1;
 if(vios_same(word,"BOOT-CONFIRM"))return vios_boot_confirm()?1:-1;
 if(vios_same(word,"BOOT-REVERT"))return vios_boot_revert()?1:-1;
 return 0;
}

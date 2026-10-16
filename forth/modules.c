/* Bounded resident source modules. No filesystem or hardware access here. */
#include "forth/modules.h"
struct dependency {char name[32];uint32_t crc;};
struct generation {
 char version[32],test[32];
 unsigned short offset,length,id;
 uint32_t crc;
 unsigned dep_count,present;
 unsigned words;size_t token_bytes,direct_bytes,data_bytes;
 struct dependency deps[FORTH_MODULE_DEPS_MAX];
};
struct module {char name[32];struct generation current,previous;};
static struct module modules[FORTH_MODULES_MAX],rollback_module;
static struct {char name[32],source[FORTH_MODULE_SOURCE_MAX+1];struct generation gen;int present;} pending;
static char source_pool[FORTH_MODULE_SOURCE_POOL];
static unsigned module_count,source_used,next_id,rollback_slot,rollback_count,rollback_source;
static int rollback_available;
static size_t len(const char *s){size_t n=0;while(s[n])++n;return n;}
static int same(const char *a,const char *b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
static void copy(char *d,const char *s){while((*d++=*s++)){}}
static int identifier(const char *s){unsigned n=0;if(!s||!*s)return 0;while(*s){if(++n>31||*s<33||*s>126)return 0;++s;}return 1;}
static int find(const char *name){for(unsigned i=0;i<module_count;++i)if(same(name,modules[i].name))return (int)i;return -1;}
static int error(const char *s){return forth_transaction_error(s);}
void forth_modules_reset(void){module_count=source_used=0;next_id=1;pending.present=rollback_available=0;}
uint32_t forth_module_crc32(const char *source){
 uint32_t crc=0xffffffffUL;if(!source)return 0;
 while(*source){crc^=(unsigned char)*source++;for(unsigned i=0;i<8;++i)crc=(crc>>1)^((crc&1)?0xedb88320UL:0);}
 return ~crc;
}
static int dependencies_valid(const struct generation *g){
 for(unsigned i=0;i<g->dep_count;++i){int n=find(g->deps[i].name);
  if(n<0||modules[n].current.crc!=g->deps[i].crc)return error("missing or stale module dependency");}
 return 1;
}
int forth_module_stage(const char *name,const char *version,
 const struct forth_module_dependency *deps,unsigned dep_count,const char *source,const char *test_word){
 if(forth_transaction_active())return error("nested module transaction");
 if(pending.present)return error("discard or activate the pending module first");
 if(!identifier(name)||!identifier(version)||(test_word&&*test_word&&!identifier(test_word)))return error("invalid module name/version/test");
 if(!source||len(source)>FORTH_MODULE_SOURCE_MAX)return error("module source exceeds 4096 bytes");
 for(const char *p=source;*p;++p)if((*p<32&&*p!='\n'&&*p!='\r'&&*p!='\t')||*p>126)return error("module source must be ASCII");
 if(dep_count>FORTH_MODULE_DEPS_MAX||(dep_count&&!deps))return error("module dependency capacity exceeded");
 int n=find(name);
 if(n<0&&module_count==FORTH_MODULES_MAX)return error("module inventory full (8 modules)");
 if(n>=0&&modules[n].previous.present)return error("two module versions retained; rollback or restart before a third");
 if(next_id>65535u)return error("module generation IDs exhausted");
 if(len(source)+1>FORTH_MODULE_SOURCE_POOL-source_used)return error("module source pool full (32768 bytes)");
 copy(pending.name,name);copy(pending.source,source);copy(pending.gen.version,version);
 pending.gen.test[0]=0;if(test_word)copy(pending.gen.test,test_word);
 pending.gen.crc=forth_module_crc32(source);pending.gen.length=(unsigned short)len(source);
 pending.gen.dep_count=dep_count;pending.gen.id=(unsigned short)next_id;pending.gen.present=1;
 for(unsigned i=0;i<dep_count;++i){
  if(!identifier(deps[i].name))return error("invalid dependency name");
  for(unsigned j=0;j<i;++j)if(same(deps[j].name,deps[i].name))return error("duplicate module dependency");
  copy(pending.gen.deps[i].name,deps[i].name);pending.gen.deps[i].crc=deps[i].source_crc32;
 }
 if(!dependencies_valid(&pending.gen))return 0;
 if(!forth_transaction_eval(pending.source,pending.gen.test,pending.gen.id,0))return 0;
 forth_transaction_sizes(&pending.gen.words,&pending.gen.token_bytes,&pending.gen.direct_bytes,&pending.gen.data_bytes);
 ++next_id;pending.present=1;return 1;
}
int forth_module_activate(const char *name){
 if(!pending.present||!name||!same(name,pending.name))return error("no matching staged module");
 if(!dependencies_valid(&pending.gen))return 0;
 int n=find(name);
 if(n<0&&module_count==FORTH_MODULES_MAX)return error("module inventory full");
 if(n>=0&&modules[n].previous.present)return error("two module versions already retained");
 if(pending.gen.length+1u>FORTH_MODULE_SOURCE_POOL-source_used)return error("module source pool full");
 /* No metadata/source mutation occurs until the complete transaction passes. */
 if(!forth_transaction_eval(pending.source,pending.gen.test,pending.gen.id,1))return 0;
 forth_transaction_sizes(&pending.gen.words,&pending.gen.token_bytes,&pending.gen.direct_bytes,&pending.gen.data_bytes);
 rollback_slot=n<0?module_count:(unsigned)n;rollback_count=module_count;rollback_source=source_used;
 if(n<0){copy(modules[module_count].name,name);modules[module_count].current.present=0;modules[module_count].previous.present=0;++module_count;}
 rollback_module=modules[rollback_slot];
 struct module *m=&modules[rollback_slot];m->previous=m->current;m->current=pending.gen;
 m->current.offset=(unsigned short)source_used;copy(source_pool+source_used,pending.source);source_used+=pending.gen.length+1u;
 pending.present=0;rollback_available=1;return 1;
}
int forth_module_discard(void){pending.present=0;return 1;}
int forth_module_rollback(const char *name){
 if(pending.present)return error("discard staged module before rollback");
 if(!rollback_available||!name||!same(name,modules[rollback_slot].name))return error("only the latest activation has a rollback checkpoint");
 if(!forth_transaction_rollback())return 0;
 modules[rollback_slot]=rollback_module;module_count=rollback_count;source_used=rollback_source;
 rollback_available=0;return 1;
}
unsigned forth_module_count(void){return module_count;}
static int info(const char *name,const struct generation *g,int active,int previous,struct forth_module_info *out){
 if(!g->present||!out)return 0;
 out->name=name;out->version=g->version;out->test_word=g->test;
 out->source_crc32=g->crc;out->source_length=g->length;out->dependency_count=g->dep_count;
 out->active=active;out->previous=previous;out->words=g->words;
 out->token_bytes=g->token_bytes;out->direct_bytes=g->direct_bytes;out->data_bytes=g->data_bytes;return 1;
}
int forth_module_info(unsigned index,struct forth_module_info *out){return index<module_count?info(modules[index].name,&modules[index].current,1,(int)modules[index].previous.present,out):0;}
int forth_module_previous_info(const char *name,struct forth_module_info *out){int n=name?find(name):-1;return n<0?0:info(modules[n].name,&modules[n].previous,0,1,out);}
int forth_module_pending_info(struct forth_module_info *out){return pending.present?info(pending.name,&pending.gen,0,0,out):0;}
int forth_module_pending_dependency_at(unsigned index,struct forth_module_dependency *out){
 if(!pending.present||!out||index>=pending.gen.dep_count)return 0;
 out->name=pending.gen.deps[index].name;out->source_crc32=pending.gen.deps[index].crc;return 1;
}
const char *forth_module_pending_source(void){return pending.present?pending.source:NULL;}
const char *forth_module_source(const char *name,int previous){
 int n=name?find(name):-1;if(n<0)return NULL;
 const struct generation *g=previous?&modules[n].previous:&modules[n].current;
 return g->present?source_pool+g->offset:NULL;
}
int forth_module_dependency_at(const char *name,int previous,unsigned index,struct forth_module_dependency *out){
 int n=name?find(name):-1;if(n<0||!out)return 0;
 const struct generation *g=previous?&modules[n].previous:&modules[n].current;
 if(!g->present||index>=g->dep_count)return 0;
 out->name=g->deps[index].name;out->source_crc32=g->deps[index].crc;return 1;
}
int forth_module_origin(unsigned id,const char **name,const char **version,uint32_t *crc){
 for(unsigned i=0;i<module_count;++i)for(unsigned j=0;j<2;++j){
  const struct generation *g=j?&modules[i].previous:&modules[i].current;
  if(g->present&&g->id==id){*name=modules[i].name;*version=g->version;*crc=g->crc;return 1;}}
 if(pending.gen.id==id){*name=pending.name;*version=pending.gen.version;*crc=pending.gen.crc;return 1;}
 return 0;
}

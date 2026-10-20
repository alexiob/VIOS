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
/* CRC-32 (zlib) by table: one lookup per byte instead of eight 32-bit shifts
 * (the bitwise loop cost ~0.8 s of module checks at boot on the eZ80). The
 * 1 KB table is shared with vios_crc32. */
static const uint32_t crc_table[256]={
 0x00000000UL,0x77073096UL,0xee0e612cUL,0x990951baUL,0x076dc419UL,0x706af48fUL,
 0xe963a535UL,0x9e6495a3UL,0x0edb8832UL,0x79dcb8a4UL,0xe0d5e91eUL,0x97d2d988UL,
 0x09b64c2bUL,0x7eb17cbdUL,0xe7b82d07UL,0x90bf1d91UL,0x1db71064UL,0x6ab020f2UL,
 0xf3b97148UL,0x84be41deUL,0x1adad47dUL,0x6ddde4ebUL,0xf4d4b551UL,0x83d385c7UL,
 0x136c9856UL,0x646ba8c0UL,0xfd62f97aUL,0x8a65c9ecUL,0x14015c4fUL,0x63066cd9UL,
 0xfa0f3d63UL,0x8d080df5UL,0x3b6e20c8UL,0x4c69105eUL,0xd56041e4UL,0xa2677172UL,
 0x3c03e4d1UL,0x4b04d447UL,0xd20d85fdUL,0xa50ab56bUL,0x35b5a8faUL,0x42b2986cUL,
 0xdbbbc9d6UL,0xacbcf940UL,0x32d86ce3UL,0x45df5c75UL,0xdcd60dcfUL,0xabd13d59UL,
 0x26d930acUL,0x51de003aUL,0xc8d75180UL,0xbfd06116UL,0x21b4f4b5UL,0x56b3c423UL,
 0xcfba9599UL,0xb8bda50fUL,0x2802b89eUL,0x5f058808UL,0xc60cd9b2UL,0xb10be924UL,
 0x2f6f7c87UL,0x58684c11UL,0xc1611dabUL,0xb6662d3dUL,0x76dc4190UL,0x01db7106UL,
 0x98d220bcUL,0xefd5102aUL,0x71b18589UL,0x06b6b51fUL,0x9fbfe4a5UL,0xe8b8d433UL,
 0x7807c9a2UL,0x0f00f934UL,0x9609a88eUL,0xe10e9818UL,0x7f6a0dbbUL,0x086d3d2dUL,
 0x91646c97UL,0xe6635c01UL,0x6b6b51f4UL,0x1c6c6162UL,0x856530d8UL,0xf262004eUL,
 0x6c0695edUL,0x1b01a57bUL,0x8208f4c1UL,0xf50fc457UL,0x65b0d9c6UL,0x12b7e950UL,
 0x8bbeb8eaUL,0xfcb9887cUL,0x62dd1ddfUL,0x15da2d49UL,0x8cd37cf3UL,0xfbd44c65UL,
 0x4db26158UL,0x3ab551ceUL,0xa3bc0074UL,0xd4bb30e2UL,0x4adfa541UL,0x3dd895d7UL,
 0xa4d1c46dUL,0xd3d6f4fbUL,0x4369e96aUL,0x346ed9fcUL,0xad678846UL,0xda60b8d0UL,
 0x44042d73UL,0x33031de5UL,0xaa0a4c5fUL,0xdd0d7cc9UL,0x5005713cUL,0x270241aaUL,
 0xbe0b1010UL,0xc90c2086UL,0x5768b525UL,0x206f85b3UL,0xb966d409UL,0xce61e49fUL,
 0x5edef90eUL,0x29d9c998UL,0xb0d09822UL,0xc7d7a8b4UL,0x59b33d17UL,0x2eb40d81UL,
 0xb7bd5c3bUL,0xc0ba6cadUL,0xedb88320UL,0x9abfb3b6UL,0x03b6e20cUL,0x74b1d29aUL,
 0xead54739UL,0x9dd277afUL,0x04db2615UL,0x73dc1683UL,0xe3630b12UL,0x94643b84UL,
 0x0d6d6a3eUL,0x7a6a5aa8UL,0xe40ecf0bUL,0x9309ff9dUL,0x0a00ae27UL,0x7d079eb1UL,
 0xf00f9344UL,0x8708a3d2UL,0x1e01f268UL,0x6906c2feUL,0xf762575dUL,0x806567cbUL,
 0x196c3671UL,0x6e6b06e7UL,0xfed41b76UL,0x89d32be0UL,0x10da7a5aUL,0x67dd4accUL,
 0xf9b9df6fUL,0x8ebeeff9UL,0x17b7be43UL,0x60b08ed5UL,0xd6d6a3e8UL,0xa1d1937eUL,
 0x38d8c2c4UL,0x4fdff252UL,0xd1bb67f1UL,0xa6bc5767UL,0x3fb506ddUL,0x48b2364bUL,
 0xd80d2bdaUL,0xaf0a1b4cUL,0x36034af6UL,0x41047a60UL,0xdf60efc3UL,0xa867df55UL,
 0x316e8eefUL,0x4669be79UL,0xcb61b38cUL,0xbc66831aUL,0x256fd2a0UL,0x5268e236UL,
 0xcc0c7795UL,0xbb0b4703UL,0x220216b9UL,0x5505262fUL,0xc5ba3bbeUL,0xb2bd0b28UL,
 0x2bb45a92UL,0x5cb36a04UL,0xc2d7ffa7UL,0xb5d0cf31UL,0x2cd99e8bUL,0x5bdeae1dUL,
 0x9b64c2b0UL,0xec63f226UL,0x756aa39cUL,0x026d930aUL,0x9c0906a9UL,0xeb0e363fUL,
 0x72076785UL,0x05005713UL,0x95bf4a82UL,0xe2b87a14UL,0x7bb12baeUL,0x0cb61b38UL,
 0x92d28e9bUL,0xe5d5be0dUL,0x7cdcefb7UL,0x0bdbdf21UL,0x86d3d2d4UL,0xf1d4e242UL,
 0x68ddb3f8UL,0x1fda836eUL,0x81be16cdUL,0xf6b9265bUL,0x6fb077e1UL,0x18b74777UL,
 0x88085ae6UL,0xff0f6a70UL,0x66063bcaUL,0x11010b5cUL,0x8f659effUL,0xf862ae69UL,
 0x616bffd3UL,0x166ccf45UL,0xa00ae278UL,0xd70dd2eeUL,0x4e048354UL,0x3903b3c2UL,
 0xa7672661UL,0xd06016f7UL,0x4969474dUL,0x3e6e77dbUL,0xaed16a4aUL,0xd9d65adcUL,
 0x40df0b66UL,0x37d83bf0UL,0xa9bcae53UL,0xdebb9ec5UL,0x47b2cf7fUL,0x30b5ffe9UL,
 0xbdbdf21cUL,0xcabac28aUL,0x53b39330UL,0x24b4a3a6UL,0xbad03605UL,0xcdd70693UL,
 0x54de5729UL,0x23d967bfUL,0xb3667a2eUL,0xc4614ab8UL,0x5d681b02UL,0x2a6f2b94UL,
 0xb40bbe37UL,0xc30c8ea1UL,0x5a05df1bUL,0x2d02ef8dUL
};
uint32_t forth_crc32_update(uint32_t crc,const unsigned char *p,size_t length){
 while(length--)crc=crc_table[(unsigned char)(crc^*p++)]^(crc>>8);
 return crc;
}
uint32_t forth_module_crc32(const char *source){
 uint32_t crc=0xffffffffUL;if(!source)return 0;
 while(*source)crc=crc_table[(unsigned char)(crc^(unsigned char)*source++)]^(crc>>8);
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
 if(pending.gen.length+1u>FORTH_MODULE_SOURCE_POOL-source_used)return error("module source pool full");
 /* No metadata/source mutation occurs until the complete transaction passes. */
 if(!forth_transaction_eval(pending.source,pending.gen.test,pending.gen.id,1))return 0;
 forth_transaction_sizes(&pending.gen.words,&pending.gen.token_bytes,&pending.gen.direct_bytes,&pending.gen.data_bytes);
 rollback_slot=n<0?module_count:(unsigned)n;rollback_count=module_count;rollback_source=source_used;
 if(n<0){copy(modules[module_count].name,name);modules[module_count].current.present=0;modules[module_count].previous.present=0;++module_count;}
 rollback_module=modules[rollback_slot];
 /* The previous generation is retired: its code and source stay allocated
  * (unnamed) until restart; rollback still restores the one before this. */
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

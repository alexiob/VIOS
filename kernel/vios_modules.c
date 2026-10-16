/* Exact-source packages and outer module commands. Files are immutable by hash. */
#include "vios/runtime.h"
#include "platform/storage.h"
#include "platform/console.h"
#define PACKAGE_HEADER 260
#define PACKAGE_MAX (PACKAGE_HEADER+FORTH_MODULE_SOURCE_MAX+4)
static unsigned char package[PACKAGE_MAX],verify_package[PACKAGE_MAX];
static struct {
 char name[32],version[32],test[32],source[FORTH_MODULE_SOURCE_MAX+1],names[4][32];
 struct forth_module_dependency deps[4];unsigned count;size_t used;int present;
} builder;
static unsigned save_depth;
static void copy(char *d,const char *s){while((*d++=*s++)){}}
static int error(const char *message){return forth_error(message);}
static int lookup(const char *name,int previous,struct forth_module_info *info){
 if(previous)return forth_module_previous_info(name,info);
 for(unsigned i=0;i<forth_module_count();++i)if(forth_module_info(i,info)&&vios_same(info->name,name))return 1;
 return error("module not found");
}
static int field(unsigned offset,const char *s){size_t n=vios_length(s);if(n>31)return 0;for(size_t i=0;i<n;++i)package[offset+i]=(unsigned char)s[i];return 1;}
static int encode_package(const char *name,int previous,size_t *size){
 struct forth_module_info info;if(!lookup(name,previous,&info))return 0;
 const char *source=forth_module_source(name,previous);if(!source)return error("module source unavailable");
 for(unsigned i=0;i<PACKAGE_HEADER;++i)package[i]=0;
 copy((char *)package,"VIM1");package[4]=1;package[5]=24;
 if(!field(8,info.name)||!field(40,info.version)||!field(72,info.test_word))return error("invalid package field");
 package[104]=(unsigned char)info.dependency_count;
 vios_put32(package+108,info.source_length);vios_put32(package+112,info.source_crc32);
 for(unsigned i=0;i<info.dependency_count;++i){struct forth_module_dependency dep;if(!forth_module_dependency_at(name,previous,i,&dep))return error("module dependency unavailable");field(116+i*36,dep.name);vios_put32(package+148+i*36,dep.source_crc32);}
 for(unsigned i=0;i<info.source_length;++i)package[PACKAGE_HEADER+i]=(unsigned char)source[i];
 *size=PACKAGE_HEADER+info.source_length+4;vios_put32(package+*size-4,vios_crc32(package,*size-4));return 1;
}
static void hex8(char *out,uint32_t value){static const char digits[]="0123456789ABCDEF";for(unsigned i=0;i<8;++i)out[i]=digits[(value>>((7-i)*4))&15];out[8]=0;}
static int content_path(const char *name,int previous,char *out,size_t capacity){
 size_t size;if(capacity<29||!encode_package(name,previous,&size))return 0;
 copy(out,"/VIOS/");hex8(out+6,forth_module_crc32(name));out[14]='/';hex8(out+15,vios_get32(package+size-4));copy(out+23,".VIM");return 1;
}
int vios_module_path(const char *name,char *out,size_t capacity){return content_path(name,0,out,capacity);}
static int immutable_write(const char *path,const void *data,size_t size){
 size_t old=0;if(storage_read_file(path,verify_package,sizeof verify_package,&old)){
  if(old!=size)return error("immutable package path already contains different data");
  const unsigned char *p=data;for(size_t i=0;i<size;++i)if(p[i]!=verify_package[i])return error("immutable package collision");return 1;
 }
 if(!vios_same(storage_error(),"file not found"))return error(storage_error());
 char temporary[128];size_t path_length=vios_length(path);
 if(path_length<4||path_length>=sizeof temporary||path[path_length-4]!='.')return error("package path needs an 8.3 extension");
 for(size_t i=0;i<=path_length;++i)temporary[i]=path[i];copy(temporary+path_length-3,"TMP");
 /* The temporary name is in the reserved VIOS package namespace. A failed
  * write can be retried without poisoning the final immutable identity. */
 if(!storage_write_file(temporary,data,size)||!storage_sync()||!storage_read_file(temporary,verify_package,sizeof verify_package,&old))return error(storage_error());
 if(old!=size)return error("package readback length mismatch");
 const unsigned char *p=data;for(size_t i=0;i<size;++i)if(p[i]!=verify_package[i])return error("package readback mismatch");
 if(!storage_rename(temporary,path)||!storage_sync())return error(storage_error());return 1;
}
static int save_generation(const char *name,int previous){
 if(save_depth==16)return error("module dependency cycle");
 ++save_depth;struct forth_module_info info;int ok=lookup(name,previous,&info);
 for(unsigned i=0;ok&&i<info.dependency_count;++i){
  struct forth_module_dependency dep;struct forth_module_info target;
  if(!forth_module_dependency_at(name,previous,i,&dep)){ok=error("missing package dependency");break;}
  int old=0;if(!lookup(dep.name,0,&target)){ok=0;break;}
  if(target.source_crc32!=dep.source_crc32){old=1;if(!lookup(dep.name,1,&target)||target.source_crc32!=dep.source_crc32){ok=error("dependency source no longer retained");break;}}
  ok=save_generation(dep.name,old);
 }
 if(ok){char path[128],directory[16];size_t size;
  ok=content_path(name,previous,path,sizeof path);
  if(ok){for(unsigned i=0;i<14;++i)directory[i]=path[i];directory[14]=0;
   ok=storage_mkdir("/VIOS")&&storage_mkdir(directory);
   if(!ok)error(storage_error());
  }
  if(ok)ok=encode_package(name,previous,&size)&&immutable_write(path,package,size);
 }
 --save_depth;return ok;
}
int vios_module_save(const char *name,const char *path){
 if(save_depth)return error("nested package save");
 if(!save_generation(name,0))return 0;
 if(path){size_t size;if(!encode_package(name,0,&size)||!immutable_write(path,package,size))return 0;}
 return 1;
}
static int terminated(unsigned offset){for(unsigned i=0;i<32;++i)if(!package[offset+i])return 1;return 0;}
int vios_module_load(const char *path){
 size_t size=0;if(!storage_read_file(path,package,sizeof package,&size))return error(storage_error());
 if(size<PACKAGE_HEADER+4||package[0]!='V'||package[1]!='I'||package[2]!='M'||package[3]!='1'||package[4]!=1||package[5]!=24)return error("incompatible module package");
 uint32_t length=vios_get32(package+108);
 if(length>FORTH_MODULE_SOURCE_MAX||size!=PACKAGE_HEADER+(size_t)length+4||package[104]>4)return error("invalid module package length");
 if(vios_get32(package+size-4)!=vios_crc32(package,size-4))return error("module package CRC mismatch");
 if(!terminated(8)||!terminated(40)||!terminated(72))return error("unterminated package metadata");
 struct forth_module_dependency deps[4];
 for(unsigned i=0;i<package[104];++i){unsigned offset=116+i*36;if(!terminated(offset))return error("unterminated package dependency");deps[i].name=(char *)package+offset;deps[i].source_crc32=vios_get32(package+offset+32);}
 for(uint32_t i=0;i<length;++i)if(!package[PACKAGE_HEADER+(size_t)i])return error("NUL in package source");
 package[PACKAGE_HEADER+(size_t)length]=0;
 if(forth_module_crc32((char *)package+PACKAGE_HEADER)!=vios_get32(package+112))return error("module source CRC mismatch");
 if(!forth_module_stage((char *)package+8,(char *)package+40,deps,package[104],(char *)package+PACKAGE_HEADER,(char *)package+72))return 0;
 return forth_module_activate((char *)package+8);
}
static void inventory(void){
 for(unsigned i=0;i<forth_module_count();++i){struct forth_module_info info;if(!forth_module_info(i,&info))continue;
  vios_out("module ");vios_out(info.name);vios_out(" version ");vios_out(info.version);vios_out(" crc32 ");vios_uint(info.source_crc32);
  vios_out(" bytes ");vios_uint(info.source_length);vios_out(" deps ");vios_uint(info.dependency_count);vios_out(" active 1 previous ");
  struct forth_module_info previous;vios_uint(forth_module_previous_info(info.name,&previous)?1:0);vios_out("\r\n");
  for(unsigned d=0;d<info.dependency_count;++d){struct forth_module_dependency dep;forth_module_dependency_at(info.name,0,d,&dep);vios_out("dependency ");vios_out(info.name);vios_out(" module ");vios_out(dep.name);vios_out(" crc32 ");vios_uint(dep.source_crc32);vios_out("\r\n");}
 }
}
static int hex_value(char c){return c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:-1;}
int vios_module_command(const char *word,const char **rest){
 char name[32],arg[233];
 if(vios_same(word,"MODULE-BEGIN")){
  if(!vios_arg(rest,builder.name,sizeof builder.name)||!vios_arg(rest,builder.version,sizeof builder.version)||!vios_arg(rest,builder.test,sizeof builder.test))return -1;
  if(vios_same(builder.test,"-"))builder.test[0]=0;builder.used=builder.count=0;builder.source[0]=0;builder.present=1;return 1;
 }
 if(vios_same(word,"MODULE-DEP")){
  if(!builder.present||builder.count==4)return error("module builder/dependency capacity unavailable")?-1:-1;
  unsigned n=builder.count;if(!vios_arg(rest,builder.names[n],32)||!vios_arg(rest,arg,sizeof arg))return -1;
  if(vios_length(arg)!=8){error("dependency CRC must be eight hex digits");return -1;}
  uint32_t crc=0;for(unsigned i=0;i<8;++i){int digit=hex_value(arg[i]);if(digit<0){error("invalid CRC hex");return -1;}crc=(crc<<4)|(unsigned)digit;}
  builder.deps[n].name=builder.names[n];builder.deps[n].source_crc32=crc;++builder.count;return 1;
 }
 if(vios_same(word,"MODULE-DATA")){
  if(!builder.present){error("no module builder");return -1;}
  if(!vios_arg(rest,arg,sizeof arg))return -1;size_t n=vios_length(arg);
  if((n&1)||n/2>FORTH_MODULE_SOURCE_MAX-builder.used){error("module source chunk/size invalid");return -1;}
  for(size_t i=0;i<n;i+=2){int a=hex_value(arg[i]),b=hex_value(arg[i+1]);if(a<0||b<0||!(a|b)){error("invalid source hex/NUL");return -1;}}
  for(size_t i=0;i<n;i+=2)builder.source[builder.used++]=(char)(hex_value(arg[i])*16+hex_value(arg[i+1]));builder.source[builder.used]=0;return 1;
 }
 if(vios_same(word,"MODULE-STAGE")){
  if(!builder.present){error("no module builder");return -1;}
  if(!forth_module_stage(builder.name,builder.version,builder.deps,builder.count,builder.source,builder.test))return -1;
  builder.present=0;vios_out("staged ");vios_out(builder.name);vios_out(" crc32 ");vios_uint(forth_module_crc32(builder.source));vios_out("\r\n");return 1;
 }
 if(vios_same(word,"MODULE-ACTIVATE")||vios_same(word,"MODULE-ROLLBACK")){
  if(!vios_arg(rest,name,sizeof name))return -1;
  int ok=vios_same(word,"MODULE-ACTIVATE")?forth_module_activate(name):forth_module_rollback(name);return ok?1:-1;
 }
 if(vios_same(word,"MODULE-DISCARD")){builder.present=0;return forth_module_discard()?1:-1;}
 if(vios_same(word,"MODULES")){inventory();return 1;}
 if(vios_same(word,"MODULE-SOURCE")){
  if(!vios_arg(rest,name,sizeof name))return -1;
  const char *saved=*rest;int previous=0;
  while(**rest==' '||**rest=='\t')++*rest;
  if(**rest){if(!vios_arg(rest,arg,sizeof arg))return -1;if(vios_same(arg,"PREVIOUS"))previous=1;else *rest=saved;}
  const char *source=forth_module_source(name,previous);if(!source){error("module source not found");return -1;}
  size_t n=vios_length(source);if(!n)vios_out("source-hex\r\n");
  for(size_t offset=0;offset<n;offset+=64){vios_out("source-hex ");size_t end=offset+64<n?offset+64:n;for(size_t i=offset;i<end;++i)vios_hex((unsigned char)source[i],2);vios_out("\r\n");}return 1;
 }
 if(vios_same(word,"SAVE")){
  char path[128];if(!vios_arg(rest,name,sizeof name))return -1;
  while(**rest==' '||**rest=='\t')++*rest;
  const char *alias=NULL;if(**rest=='/'){if(!vios_arg(rest,path,sizeof path))return -1;alias=path;}
  if(!vios_module_save(name,alias))return -1;
  if(!alias&&!vios_module_path(name,path,sizeof path))return -1;
  vios_out("saved ");vios_out(path);vios_out("\r\n");return 1;
 }
 if(vios_same(word,"MODULE-LOAD")){char path[128];if(!vios_arg(rest,path,sizeof path)||!vios_module_load(path))return -1;return 1;}
 return 0;
}

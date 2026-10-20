/* Portable, bounded workbench model. All entry points run on the idle VM owner. */
#include <string.h>
#include "vios/workbench.h"
#include "vios/text_screen.h"
static struct {
 char source[VIOS_WB_SOURCE_MAX+1],undo[VIOS_WB_SOURCE_MAX+1];
 char module[32],version[32],test[32],error[128],selection[32],filter[32];
 char dep_names[FORTH_MODULE_DEPS_MAX][32];
 struct forth_module_dependency deps[FORTH_MODULE_DEPS_MAX];
 unsigned length,cursor,undo_length,undo_cursor,dep_count,selected,scroll;
 uint32_t revision,catalog,staged_revision;
 int open,has_draft,dirty,staged,activated,saved,undo_valid;
 int undo_dirty,undo_activated,undo_saved,zoom;
 enum vios_workbench_browser browser;
 enum vios_workbench_focus focus;
} wb;
/* Retained composition state. Offsets are 16 bit because drafts are bounded
 * to 4096 bytes. Ordinary edits adjust offsets, without scanning other lines. */
static struct {
 uint16_t lines[VIOS_WB_SOURCE_MAX+1];
 unsigned line_count,damage_first,damage_last;
 unsigned shift_old,shift_new,shift_damage;
 int shift_valid;
 uint32_t generation,catalog;
 unsigned selected,scroll,top,left,cursor;
 enum vios_workbench_browser browser;
 enum vios_workbench_focus focus;
 char filter[32],editor[38][80];
 char rows[15][80];
 unsigned char attrs[15][80];
 unsigned count,count_browser,measure_cursor,measure_start,measure_column;
 uint32_t count_catalog;
 char count_filter[32];
 int valid,editor_valid,browser_valid,damage,count_valid,measure_valid;
} view;
static unsigned source_line(unsigned offset){
 unsigned lo=0,hi=view.line_count;
 while(lo+1<hi){unsigned mid=lo+(hi-lo)/2;if(view.lines[mid]<=offset)lo=mid;else hi=mid;}
 return lo;
}
static void source_damage(unsigned first,unsigned last){
 if(!view.damage){view.damage_first=first;view.damage_last=last;view.damage=1;}
 else {if(first<view.damage_first)view.damage_first=first;if(last>view.damage_last)view.damage_last=last;}
}
static void source_index(unsigned first){
 unsigned start=view.lines[first];view.line_count=first+1;
 for(unsigned i=start;i<wb.length;++i){
  if(wb.source[i]=='\r'){if(i+1<wb.length&&wb.source[i+1]=='\n')++i;view.lines[view.line_count++]=(uint16_t)(i+1);}
  else if(wb.source[i]=='\n')view.lines[view.line_count++]=(uint16_t)(i+1);
 }
 source_damage(first,VIOS_WB_SOURCE_MAX);
}
static void source_reset(void){view.shift_valid=0;view.measure_valid=0;view.lines[0]=0;source_index(0);}
static void bounded_copy(char *out,const char *s,size_t max){size_t n=0;if(s)while(s[n]&&n+1<max){out[n]=s[n];++n;}out[n]=0;}
static int fail(const char *s){bounded_copy(wb.error,s,sizeof wb.error);return 0;}
static int okay(void){wb.error[0]=0;return 1;}
static int identifier(const char *s,int empty){size_t n=0;if(!s)return empty;if(!*s)return empty;for(;s[n];++n)if(n>=31||(unsigned char)s[n]<33||(unsigned char)s[n]>126)return 0;return 1;}
static int module_info(const char *name,struct forth_module_info *out){for(unsigned i=0;i<forth_module_count();++i)if(forth_module_info(i,out)&&!strcmp(name,out->name))return 1;return 0;}
static int matches(const struct forth_module_info *m,const char *source){
 if(!source||strcmp(m->name,wb.module)||strcmp(m->version,wb.version)||strcmp(m->test_word,wb.test)||strcmp(source,wb.source)||m->dependency_count!=wb.dep_count)return 0;
 for(unsigned i=0;i<wb.dep_count;++i){struct forth_module_dependency d;
  if(!forth_module_dependency_at(wb.module,0,i,&d)||strcmp(d.name,wb.deps[i].name)||d.source_crc32!=wb.deps[i].source_crc32)return 0;}
 return 1;
}
static int exact_active(void){struct forth_module_info m;return wb.activated&&module_info(wb.module,&m)&&matches(&m,forth_module_source(wb.module,0));}
static int pending_matches(void){struct forth_module_info m;
 if(!forth_module_pending_info(&m)||strcmp(m.name,wb.module)||strcmp(m.version,wb.version)||strcmp(m.test_word,wb.test)||m.dependency_count!=wb.dep_count)return 0;
 for(unsigned i=0;i<wb.dep_count;++i){struct forth_module_dependency d;
  if(!forth_module_pending_dependency_at(i,&d)||strcmp(d.name,wb.deps[i].name)||d.source_crc32!=wb.deps[i].source_crc32)return 0;}
 const char *s=forth_module_pending_source();return s&&!strcmp(s,wb.source);
}
static void invalidate(void){if(wb.staged&&pending_matches())forth_module_discard();wb.staged=0;wb.activated=0;wb.saved=0;}
static int revision(uint32_t expected){if(!wb.has_draft)return fail("no module draft");if(expected!=wb.revision)return fail("stale draft revision");if(wb.revision==UINT32_MAX)return fail("draft revision exhausted");return 1;}
static void snapshot(void){memcpy(wb.undo,wb.source,wb.length+1);wb.undo_length=wb.length;wb.undo_cursor=wb.cursor;wb.undo_dirty=wb.dirty;wb.undo_activated=wb.activated;wb.undo_saved=wb.saved;wb.undo_valid=1;}
static unsigned browser_count(void){
 uint32_t catalog=forth_catalog_revision();
 if(view.count_valid&&view.count_catalog==catalog&&view.count_browser==(unsigned)wb.browser&&!strcmp(view.count_filter,wb.filter))return view.count;
 unsigned count=0;
 if(wb.browser==VIOS_WB_MODULES)count=forth_module_count();
 else if(!wb.filter[0])count=forth_word_count();
 else for(unsigned i=0;i<forth_word_count();++i){struct forth_word_info w;if(forth_inspect_word(i,&w)&&w.module&&!strcmp(w.module,wb.filter))++count;}
 view.count=count;view.count_valid=1;view.count_catalog=catalog;view.count_browser=(unsigned)wb.browser;bounded_copy(view.count_filter,wb.filter,sizeof view.count_filter);return count;
}
static int browser_item(unsigned row,unsigned *absolute,const char **name){
 if(wb.browser==VIOS_WB_MODULES){struct forth_module_info m;if(!forth_module_info(row,&m))return 0;*absolute=row;*name=m.name;return 1;}
 if(!wb.filter[0]){struct forth_word_info w;if(!forth_inspect_word(row,&w))return 0;*absolute=row;*name=w.name;return 1;}
 unsigned count=0;for(unsigned i=0;i<forth_word_count();++i){struct forth_word_info w;if(!forth_inspect_word(i,&w)||(wb.filter[0]&&(!w.module||strcmp(w.module,wb.filter))))continue;
  if(count++==row){*absolute=i;*name=w.name;return 1;}}
 return 0;
}
static unsigned browser_visible(void){return wb.zoom?16:53;}
static unsigned editor_width(void){return wb.zoom?80:54;}
static unsigned editor_x(void){return wb.zoom?0:26;}
static void refresh_selection(void){unsigned n=browser_count(),absolute;const char *name;
 if(wb.selected>=n)wb.selected=n?n-1:0;
 if(browser_item(wb.selected,&absolute,&name))bounded_copy(wb.selection,name,sizeof wb.selection);else wb.selection[0]=0;
 if(wb.selected<wb.scroll)wb.scroll=wb.selected;if(wb.selected>=wb.scroll+browser_visible())wb.scroll=wb.selected-browser_visible()+1;
}
void vios_workbench_init(void){memset(&wb,0,sizeof wb);wb.revision=1;wb.catalog=forth_catalog_revision();memset(&view,0,sizeof view);source_reset();}
void vios_workbench_open(void){wb.open=1;view.valid=0;refresh_selection();}
void vios_workbench_close(void){wb.open=0;view.valid=0;}
int vios_workbench_active(void){return wb.open;}
void vios_workbench_zoom(int enabled){
 enabled=enabled!=0;if(wb.zoom==enabled)return;
 wb.zoom=enabled;view.valid=0;view.shift_valid=0;refresh_selection();
}
int vios_workbench_zoomed(void){return wb.zoom;}
const char *vios_workbench_error(void){return wb.error;}
void vios_workbench_note_error(const char *message){bounded_copy(wb.error,message,sizeof wb.error);}
const char *vios_workbench_source(void){return wb.source;}
void vios_workbench_status(struct vios_workbench_status *out){if(!out)return;refresh_selection();
 out->module=wb.module;out->version=wb.version;out->test_word=wb.test;out->error=wb.error;out->selection=wb.selection;out->filter=wb.filter;out->dependencies=wb.deps;out->dependency_count=wb.dep_count;
 out->length=wb.length;out->cursor=wb.cursor;out->selection_index=wb.selected;out->browser_count=browser_count();out->revision=wb.revision;out->catalog_revision=forth_catalog_revision();
 out->open=wb.open;out->has_draft=wb.has_draft;out->dirty=wb.dirty;out->staged=wb.staged&&wb.staged_revision==wb.revision&&pending_matches();out->activated=exact_active();out->saved=wb.saved&&out->activated;out->undo_available=wb.undo_valid;out->browser=wb.browser;out->focus=wb.focus;
}
int vios_workbench_browse(enum vios_workbench_browser browser,const char *filter){if(browser!=VIOS_WB_MODULES&&browser!=VIOS_WB_WORDS)return fail("invalid browser");if(!identifier(filter,1))return fail("invalid module filter");wb.browser=browser;bounded_copy(wb.filter,filter,sizeof wb.filter);wb.selected=wb.scroll=0;refresh_selection();return okay();}
int vios_workbench_select_word(unsigned index){struct forth_word_info w;if(!forth_inspect_word(index,&w))return fail("word index out of range");wb.browser=VIOS_WB_WORDS;wb.filter[0]=0;wb.selected=index;refresh_selection();return okay();}
int vios_workbench_select_module(const char *name){struct forth_module_info m;if(!name)return fail("missing module");for(unsigned i=0;i<forth_module_count();++i)if(forth_module_info(i,&m)&&!strcmp(name,m.name)){wb.browser=VIOS_WB_MODULES;wb.selected=i;refresh_selection();return okay();}return fail("module not found");}
int vios_workbench_selected_word(struct forth_word_info *out){unsigned absolute;const char *name;refresh_selection();return wb.browser==VIOS_WB_WORDS&&browser_item(wb.selected,&absolute,&name)&&forth_inspect_word(absolute,out);}
static int replace_ready(int discard){if(wb.has_draft&&wb.dirty&&!discard)return fail("dirty draft requires explicit discard");if(wb.revision==UINT32_MAX)return fail("draft revision exhausted");return 1;}
int vios_workbench_edit(const char *name,int discard){struct forth_module_info m;if(!name||!module_info(name,&m))return fail("module not found");if(!replace_ready(discard))return 0;
 const char *source=forth_module_source(name,0);if(!source||m.source_length>VIOS_WB_SOURCE_MAX)return fail("module source unavailable or too large");
 invalidate();bounded_copy(wb.module,m.name,sizeof wb.module);bounded_copy(wb.version,m.version,sizeof wb.version);bounded_copy(wb.test,m.test_word,sizeof wb.test);memcpy(wb.source,source,m.source_length+1);wb.length=m.source_length;wb.cursor=0;wb.dep_count=m.dependency_count;
 for(unsigned i=0;i<wb.dep_count;++i){struct forth_module_dependency d;forth_module_dependency_at(name,0,i,&d);bounded_copy(wb.dep_names[i],d.name,32);wb.deps[i].name=wb.dep_names[i];wb.deps[i].source_crc32=d.source_crc32;}
 wb.has_draft=1;wb.dirty=0;wb.activated=1;wb.saved=0;wb.undo_valid=0;++wb.revision;wb.catalog=forth_catalog_revision();source_reset();return okay();
}
int vios_workbench_new(const char *name,const char *version_name,int discard){if(!identifier(name,0)||!identifier(version_name,0))return fail("invalid module name/version");if(!replace_ready(discard))return 0;char saved_name[32],saved_version[32];bounded_copy(saved_name,name,32);bounded_copy(saved_version,version_name,32);invalidate();bounded_copy(wb.module,saved_name,32);bounded_copy(wb.version,saved_version,32);wb.source[0]=wb.test[0]=0;wb.length=wb.cursor=wb.dep_count=0;wb.has_draft=wb.dirty=1;wb.undo_valid=0;++wb.revision;wb.catalog=forth_catalog_revision();source_reset();return okay();}
int vios_workbench_patch(uint32_t expected,size_t offset,size_t remove,const char *bytes,size_t length){
 if(!revision(expected))return 0;if(offset>wb.length||remove>wb.length-offset)return fail("patch range out of bounds");if(length>VIOS_WB_SOURCE_MAX-(wb.length-remove))return fail("draft exceeds 4096 bytes");if(length&&!bytes)return fail("missing patch bytes");
 for(size_t i=0;i<length;++i){unsigned char c=(unsigned char)bytes[i];if((c<32&&c!='\t'&&c!='\r'&&c!='\n')||c>126)return fail("draft source must be ASCII without control bytes");}
 /* Reject aliasing draft slices rather than expose memmove-order surprises. */
 if(length){for(unsigned i=0;i<=wb.length;++i)if(bytes==wb.source+i)return fail("patch bytes alias draft");}
 unsigned line=source_line((unsigned)offset),suffix=source_line((unsigned)(offset+remove))+1;
 unsigned suffix_offset=suffix<view.line_count?view.lines[suffix]:0;
 int can_shift=view.valid&&view.editor_valid&&!view.damage&&suffix<view.line_count;
 view.shift_valid=0;
 int structural=offset&&wb.source[offset-1]=='\r';
 for(size_t i=0;i<remove;++i)if(wb.source[offset+i]=='\r'||wb.source[offset+i]=='\n')structural=1;
 for(size_t i=0;i<length;++i)if(bytes[i]=='\r'||bytes[i]=='\n')structural=1;
 if(!structural&&view.measure_valid&&offset+remove==view.measure_cursor){
  for(size_t i=0;i<remove;++i)if(wb.source[offset+i]=='\t')view.measure_valid=0;
  if(view.measure_valid){view.measure_column-=(unsigned)remove;
   for(size_t i=0;i<length;++i)view.measure_column=bytes[i]=='\t'?(view.measure_column+4)&~3u:view.measure_column+1;
   view.measure_cursor=(unsigned)(offset+length);}
 }else view.measure_valid=0;
 snapshot();invalidate();memmove(wb.source+offset+length,wb.source+offset+remove,wb.length-offset-remove+1);if(length)memcpy(wb.source+offset,bytes,length);wb.length=(unsigned)(wb.length-remove+length);wb.cursor=(unsigned)(offset+length);wb.dirty=1;++wb.revision;
 if(structural){
  source_index(line?line-1:0);
  if(can_shift){
   unsigned adjusted=(unsigned)(suffix_offset+length-remove),next=source_line(adjusted);
   if(view.lines[next]==adjusted&&next!=suffix){
    view.shift_valid=1;view.shift_old=suffix;view.shift_new=next;view.shift_damage=line?line-1:0;
   }
  }
 }
 else {for(unsigned i=line+1;i<view.line_count;++i)view.lines[i]=(uint16_t)(view.lines[i]+length-remove);source_damage(line,line);}
 return okay();
}
int vios_workbench_undo(uint32_t expected){if(!revision(expected))return 0;if(!wb.undo_valid)return fail("no undo snapshot");invalidate();memcpy(wb.source,wb.undo,wb.undo_length+1);wb.length=wb.undo_length;wb.cursor=wb.undo_cursor;wb.dirty=wb.undo_dirty;wb.activated=wb.undo_activated;wb.saved=wb.undo_saved;wb.undo_valid=0;++wb.revision;source_reset();return okay();}
int vios_workbench_set_metadata(uint32_t expected,const char *version_name,const char *test,const struct forth_module_dependency *deps,unsigned count){
 if(!revision(expected))return 0;if(!identifier(version_name,0)||!identifier(test,1))return fail("invalid version/test word");if(count>FORTH_MODULE_DEPS_MAX||(count&&!deps))return fail("dependency capacity exceeded");
 for(unsigned i=0;i<count;++i){if(!identifier(deps[i].name,0))return fail("invalid dependency name");for(unsigned j=0;j<i;++j)if(!strcmp(deps[i].name,deps[j].name))return fail("duplicate module dependency");}
 char saved_version[32],saved_test[32],saved_names[FORTH_MODULE_DEPS_MAX][32];uint32_t saved_crcs[FORTH_MODULE_DEPS_MAX];
 bounded_copy(saved_version,version_name,32);bounded_copy(saved_test,test,32);for(unsigned i=0;i<count;++i){bounded_copy(saved_names[i],deps[i].name,32);saved_crcs[i]=deps[i].source_crc32;}
 invalidate();bounded_copy(wb.version,saved_version,32);bounded_copy(wb.test,saved_test,32);wb.dep_count=count;for(unsigned i=0;i<count;++i){bounded_copy(wb.dep_names[i],saved_names[i],32);wb.deps[i].name=wb.dep_names[i];wb.deps[i].source_crc32=saved_crcs[i];}
 wb.undo_valid=0;wb.dirty=1;++wb.revision;return okay();
}
static int catalog_valid(void){return wb.catalog==forth_catalog_revision()?1:fail("live catalog changed; reload draft before stage/activate");}
int vios_workbench_stage(uint32_t expected){struct forth_module_info pending;if(!revision(expected)||!catalog_valid())return 0;if(wb.staged&&wb.staged_revision==expected&&pending_matches())return okay();
 if(forth_module_pending_info(&pending))return fail("another module is pending; discard or activate it first");
 if(!forth_module_stage(wb.module,wb.version,wb.deps,wb.dep_count,wb.source,wb.test)){bounded_copy(wb.error,forth_last_error(),sizeof wb.error);forth_module_discard();wb.staged=0;return 0;}
 wb.staged=1;wb.staged_revision=wb.revision;return okay();
}
int vios_workbench_activate(uint32_t expected){if(!revision(expected)||!catalog_valid())return 0;if(!wb.staged||wb.staged_revision!=expected||!pending_matches())return fail("draft must be staged at this revision");
 if(!forth_module_activate(wb.module))return fail(forth_last_error());wb.staged=0;wb.activated=1;wb.saved=0;wb.catalog=forth_catalog_revision();return okay();
}
int vios_workbench_can_save(uint32_t expected){struct forth_module_info m;if(!revision(expected))return 0;if(!wb.activated||!module_info(wb.module,&m)||!matches(&m,forth_module_source(wb.module,0)))return fail("save requires the exact active draft");return okay();}
int vios_workbench_mark_saved(uint32_t expected){if(!vios_workbench_can_save(expected))return 0;wb.saved=1;wb.dirty=0;return okay();}
static unsigned line_start(unsigned cursor){while(cursor&&wb.source[cursor-1]!='\n'&&wb.source[cursor-1]!='\r')--cursor;return cursor;}
static unsigned line_end(unsigned cursor){while(cursor<wb.length&&wb.source[cursor]!='\n'&&wb.source[cursor]!='\r')++cursor;return cursor;}
enum vios_workbench_request vios_workbench_key(unsigned code,unsigned modifiers){
 if(!wb.open)return VIOS_WB_NONE;if(code==VIOS_WB_F10){vios_workbench_close();return VIOS_WB_CLOSED;}if(code==VIOS_WB_F5)return VIOS_WB_STAGE;if(code==VIOS_WB_F6)return VIOS_WB_ACTIVATE;if(code==VIOS_WB_F7)return VIOS_WB_SAVE;
 if(code==' '&&(modifiers&VIOS_WB_MOD_CTRL)&&!(modifiers&(VIOS_WB_MOD_LALT|VIOS_WB_MOD_RALT|VIOS_WB_MOD_GUI))){vios_workbench_zoom(!wb.zoom);return VIOS_WB_NONE;}
 if(code==VIOS_WB_F8){vios_workbench_undo(wb.revision);return VIOS_WB_NONE;}if(code==9){wb.focus=wb.focus==VIOS_WB_BROWSER?VIOS_WB_EDITOR:VIOS_WB_BROWSER;return VIOS_WB_NONE;}if(modifiers&(VIOS_WB_MOD_CTRL|VIOS_WB_MOD_LALT|VIOS_WB_MOD_RALT|VIOS_WB_MOD_GUI))return VIOS_WB_NONE;
 if(wb.focus==VIOS_WB_BROWSER){refresh_selection();
  if(code==VIOS_WB_RIGHT&&wb.browser==VIOS_WB_MODULES){char filter[32];bounded_copy(filter,wb.selection,32);vios_workbench_browse(VIOS_WB_WORDS,filter);return VIOS_WB_NONE;}
  if(code==VIOS_WB_LEFT&&wb.browser==VIOS_WB_WORDS){char module[32];bounded_copy(module,wb.filter,32);vios_workbench_browse(VIOS_WB_MODULES,NULL);if(module[0])vios_workbench_select_module(module);return VIOS_WB_NONE;}
  if(code==VIOS_WB_UP&&wb.selected)--wb.selected;if(code==VIOS_WB_DOWN&&wb.selected+1<browser_count())++wb.selected;if(code==VIOS_WB_HOME)wb.selected=0;if(code==VIOS_WB_END&&browser_count())wb.selected=browser_count()-1;
  refresh_selection();if(code==13||code==10){if(wb.browser==VIOS_WB_MODULES)vios_workbench_edit(wb.selection,0);else {struct forth_word_info w;if(vios_workbench_selected_word(&w)){if(w.module)vios_workbench_edit(w.module,0);else fail("kernel words are read-only");}}}return VIOS_WB_NONE;}
 if(!wb.has_draft)return VIOS_WB_NONE;
 if(code==VIOS_WB_LEFT){if(wb.cursor)--wb.cursor;}else if(code==VIOS_WB_RIGHT){if(wb.cursor<wb.length)++wb.cursor;}
 else if(code==VIOS_WB_HOME)wb.cursor=line_start(wb.cursor);else if(code==VIOS_WB_END)wb.cursor=line_end(wb.cursor);
 else if(code==VIOS_WB_UP){unsigned start=line_start(wb.cursor),column=wb.cursor-start;if(start){unsigned end=start-1;if(end&&wb.source[end]=='\n'&&wb.source[end-1]=='\r')--end;unsigned previous=line_start(end);wb.cursor=previous+column;if(wb.cursor>end)wb.cursor=end;}}
 else if(code==VIOS_WB_DOWN){unsigned column=wb.cursor-line_start(wb.cursor),end=line_end(wb.cursor);if(end<wb.length){unsigned next=end+1;if(wb.source[end]=='\r'&&next<wb.length&&wb.source[next]=='\n')++next;wb.cursor=next+column;unsigned stop=line_end(next);if(wb.cursor>stop)wb.cursor=stop;}}
 else if(code==8||code==127){if(wb.cursor)vios_workbench_patch(wb.revision,wb.cursor-1,1,NULL,0);}
 else if(code==VIOS_WB_DELETE){if(wb.cursor<wb.length)vios_workbench_patch(wb.revision,wb.cursor,1,NULL,0);}
 else if(code==13||code==10){vios_workbench_patch(wb.revision,wb.cursor,0,"\n",1);}
 else if(code>=32&&code<=126){char c=(char)code;vios_workbench_patch(wb.revision,wb.cursor,0,&c,1);}return VIOS_WB_NONE;
}
/* Small dynamic rows are rebuilt to observe arbitrary CLI changes to the VM.
 * Only changed rows reach the screen; source rows use explicit edit damage. */
static char row_text[80];
static unsigned char row_attr[80];
static int row_active;
static void text(unsigned x,unsigned y,const char *s,unsigned width,unsigned fg){
 size_t n=strlen(s);if(n>width)n=width;
 if(row_active){memcpy(row_text+x,s,n);memset(row_attr+x,(int)fg,n);}
 else vios_text_put(x,y,s,n,fg,0);
}
static void number(unsigned x,unsigned y,unsigned value,unsigned width){
 char buf[16],out[16];unsigned n=0;
 do{buf[n++]=(char)('0'+value%10);value/=10;}while(value&&n<sizeof buf);
 for(unsigned i=0;i<n;++i)out[i]=buf[n-1-i];out[n]=0;text(x,y,out,width,3);
}
static void row_begin(void){memset(row_text,' ',sizeof row_text);memset(row_attr,3,sizeof row_attr);row_active=1;}
static void row_end(unsigned slot,unsigned x,unsigned y,unsigned width,int force){
 row_active=0;
 if(!force&&!memcmp(view.rows[slot]+x,row_text+x,width)&&!memcmp(view.attrs[slot]+x,row_attr+x,width))return;
 memcpy(view.rows[slot]+x,row_text+x,width);memcpy(view.attrs[slot]+x,row_attr+x,width);
 if(!force)vios_text_clear(x,y,width,1);
 unsigned end=x+width;while(end>x&&row_text[end-1]==' '&&row_attr[end-1]==3)--end;
 for(unsigned start=x;start<end;){unsigned stop=start+1;while(stop<end&&row_attr[stop]==row_attr[start])++stop;
  vios_text_put(start,y,row_text+start,stop-start,row_attr[start],0);start=stop;}
}
static void browser_row(unsigned row){
 unsigned absolute;const char *name;
 vios_text_clear(0,row+2,25,1);
 if(browser_item(wb.scroll+row,&absolute,&name))text(0,row+2,name,25,wb.scroll+row==wb.selected?2:3);
}
static void browser_compose(int full,uint32_t catalog){
 int pane=full||!view.browser_valid||view.catalog!=catalog||view.browser!=wb.browser||view.scroll!=wb.scroll||strcmp(view.filter,wb.filter);
 if(pane){
  if(!full)vios_text_clear(0,1,25,browser_visible()+1);text(0,1,wb.browser==VIOS_WB_MODULES?"MODULES":"WORDS",25,2);
  if(wb.browser==VIOS_WB_WORDS&&wb.filter[0]){
   unsigned index=0,row=0;
   for(unsigned i=0;i<forth_word_count()&&row<browser_visible();++i){struct forth_word_info w;
    if(!forth_inspect_word(i,&w)||!w.module||strcmp(w.module,wb.filter))continue;
    if(index>=wb.scroll){text(0,row+2,w.name,25,index==wb.selected?2:3);++row;}++index;}
  }else for(unsigned row=0;row<browser_visible();++row){unsigned absolute;const char *name;
   if(browser_item(wb.scroll+row,&absolute,&name))text(0,row+2,name,25,wb.scroll+row==wb.selected?2:3);}
 }else if(view.selected!=wb.selected){
  if(view.selected>=wb.scroll&&view.selected<wb.scroll+browser_visible())browser_row(view.selected-wb.scroll);
  if(wb.selected>=wb.scroll&&wb.selected<wb.scroll+browser_visible())browser_row(wb.selected-wb.scroll);
 }
 view.browser_valid=1;view.browser=wb.browser;view.scroll=wb.scroll;view.selected=wb.selected;bounded_copy(view.filter,wb.filter,sizeof view.filter);
}
static unsigned char editor_cursors[38];
static unsigned editor_cursor_row=38;
/* First and last columns where two rows differ. Callers guarantee at least
 * one difference, so neither scan needs a bound. On eZ80 a C byte walk spills
 * its pointers to the stack frame (about 86 cycles a byte); cpi and cpd keep
 * them in DE and HL (about 8). IY is scratch here, as in the library memcmp. */
#if defined(__ez80__)
unsigned editor_first_difference(const char *a,const char *b);
unsigned editor_last_difference(const char *a,const char *b,unsigned width);
__asm__(
 ".assume ADL = 1\n"
 ".section .text\n"
 ".global _editor_first_difference, _editor_last_difference\n"
 "_editor_first_difference:\n"
 " ld iy,0\n add iy,sp\n ld de,(iy+3)\n ld hl,(iy+6)\n"
 ".Lfirst:\n ld a,(de)\n inc de\n cpi\n jr z,.Lfirst\n"
 " ld de,(iy+6)\n scf\n sbc hl,de\n ret\n"
 "_editor_last_difference:\n"
 " ld iy,0\n add iy,sp\n ld bc,(iy+9)\n dec bc\n"
 " ld hl,(iy+3)\n add hl,bc\n ex de,hl\n ld hl,(iy+6)\n add hl,bc\n"
 ".Llast:\n ld a,(de)\n dec de\n cpd\n jr z,.Llast\n"
 " inc hl\n ld de,(iy+6)\n or a,a\n sbc hl,de\n ret\n");
#else
static unsigned editor_first_difference(const char *a,const char *b){unsigned i=0;while(a[i]==b[i])++i;return i;}
static unsigned editor_last_difference(const char *a,const char *b,unsigned width){while(a[width-1]==b[width-1])--width;return width-1;}
#endif
/* On eZ80, inlining this row loop into compose combines their register
 * pressure and spills per-character state into deep stack-frame accesses. */
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void editor_row(unsigned row,unsigned line,unsigned left,int focused){
 unsigned width=editor_width(),origin=editor_x();
 char shown[80];memset(shown,' ',width);unsigned cursor=width,x=0;
 if(line<view.line_count){
  unsigned start=view.lines[line],end=line+1<view.line_count?view.lines[line+1]:wb.length;
  unsigned stop=end;
  if(stop>start&&wb.source[stop-1]=='\n')--stop;
  if(stop>start&&wb.source[stop-1]=='\r')--stop;
  unsigned scan=stop-start;if(scan>left+width)scan=left+width;
  /* ASCII source without visible-prefix tabs maps byte columns directly.
   * Use the target's block-copy routines instead of projecting each byte. */
  if(!memchr(wb.source+start,'\t',scan)){
   if(stop-start>left){unsigned n=stop-start-left;if(n>width)n=width;memcpy(shown,wb.source+start+left,n);}
   if(focused&&wb.cursor>=start&&wb.cursor<=stop&&wb.cursor-start>=left&&wb.cursor-start-left<width)cursor=wb.cursor-start-left;
   /* A cursor on the LF of CRLF is drawn at the preceding line's end. */
   if(focused&&stop<end&&wb.source[stop]=='\r'&&stop+1<end&&wb.source[stop+1]=='\n'&&wb.cursor==stop+1&&stop-start>=left&&stop-start-left<width)cursor=stop-start-left;
  }else for(unsigned i=start;i<=end&&i<=wb.length;++i){unsigned char c=(unsigned char)wb.source[i];
   if(i==wb.cursor&&focused&&x>=left&&x-left<width)cursor=x-left;
   if(c=='\r'||c=='\n'||!c){
    if(c=='\r'&&i+1<wb.length&&wb.source[i+1]=='\n'&&wb.cursor==i+1&&focused&&x>=left&&x-left<width)cursor=x-left;
    break;
   }
   if(c>=32&&c<=126&&x>=left&&x-left<width)shown[x-left]=(char)c;
   if(c=='\t')x=(x+4)&~3u;else ++x;
   if(x>=left+width)break;
  }
 }
 /* The cache holds the glyphs this row last stored, in style 3 except at the
  * cursor, which is repaired before and restyled after the span. Re-storing
  * unchanged glyphs between the first and last difference leaves the planes
  * and dirty counts as they were, so one span replaces the per-run diff. */
 char *cached=view.editor[row];
 unsigned old_cursor=view.editor_valid?editor_cursors[row]:width;
 if(!view.editor_valid)memset(cached,' ',width);
 int changed=memcmp(cached,shown,width)!=0;
 if(changed||old_cursor!=cursor){
  if(old_cursor<width&&old_cursor!=cursor)vios_text_put(origin+old_cursor,row+18,shown+old_cursor,1,3,0);
  if(changed){
   unsigned first=editor_first_difference(shown,cached),last=editor_last_difference(shown,cached,width);
   vios_text_put(origin+first,row+18,shown+first,last-first+1,3,0);memcpy(cached,shown,width);
  }
  if(cursor<width)vios_text_put(origin+cursor,row+18,shown+cursor,1,0,2);
 }
 editor_cursors[row]=(unsigned char)cursor;
}
/* A successful renderer scroll moves styled cells. Move both caches with
 * them so the old cursor is repaired at its new location. */
static int editor_scroll(unsigned first,int rows){
 unsigned height=38-first,distance=(unsigned)(rows<0?-rows:rows);
 if(!distance||distance>=height||distance>31||!vios_text_scroll(editor_x(),18+first,editor_width(),height,rows))return 0;
 if(rows>0){
  memmove(view.editor[first],view.editor[first+distance],(height-distance)*sizeof view.editor[0]);
  memmove(editor_cursors+first,editor_cursors+first+distance,height-distance);
 }else {
  memmove(view.editor[first+distance],view.editor[first],(height-distance)*sizeof view.editor[0]);
  memmove(editor_cursors+first+distance,editor_cursors+first,height-distance);
 }
 unsigned blank=rows>0?38-distance:first;
 memset(view.editor[blank],' ',distance*sizeof view.editor[0]);memset(editor_cursors+blank,80,distance);
 return 1;
}
static void editor_compose(int full){
 unsigned current_line=source_line(wb.cursor),start=view.lines[current_line],column=0;
 /* line_start historically treats the LF of CRLF as column zero. */
 if(wb.cursor&&wb.source[wb.cursor-1]=='\r')start=wb.cursor;
 unsigned from=start;
 if(view.measure_valid&&view.measure_start==start){
  column=view.measure_column;from=view.measure_cursor;
  if(from>wb.cursor){
   for(unsigned i=wb.cursor;i<from;++i)if(wb.source[i]=='\t'){column=0;from=start;break;}
   if(from>wb.cursor){column-=from-wb.cursor;from=wb.cursor;}
  }
 }
 for(unsigned i=from;i<wb.cursor;++i)column=wb.source[i]=='\t'?(column+4)&~3u:column+1;
 view.measure_valid=1;view.measure_cursor=wb.cursor;view.measure_start=start;view.measure_column=column;
 unsigned width=editor_width(),left=column>=width?column-width+1:0,top=current_line>=38?current_line-37:0;
 int all=full||!view.editor_valid||view.top!=top||view.left!=left;
 unsigned exposed_first=38,exposed_last=0;
 if(!full&&view.editor_valid&&view.left==left){
  /* Damage is in source lines, so the row loop still repaints it after a
   * scroll; only skip the scroll when the damage covers the whole view. */
  if(view.top!=top&&(!view.damage||view.damage_first>top)){
   int rows=(int)top-(int)view.top;
   if(editor_scroll(0,rows)){
    all=0;if(rows>0){exposed_first=38-(unsigned)rows;exposed_last=37;}
    else {exposed_first=0;exposed_last=(unsigned)(-rows)-1;}
   }
  }else if(view.top==top&&view.shift_valid){
   unsigned first=view.shift_old<view.shift_new?view.shift_old:view.shift_new;
   int rows=(int)view.shift_old-(int)view.shift_new;
   if(first>=top&&first<top+38&&editor_scroll(first-top,rows)){
    view.damage_first=view.shift_damage;view.damage_last=view.shift_new-1;
    if(rows>0){exposed_first=38-(unsigned)rows;exposed_last=37;}
    else {exposed_first=first-top;exposed_last=exposed_first+(unsigned)(-rows)-1;}
   }
  }
 }
 unsigned cursor_row=current_line-top;
 for(unsigned row=0;row<38;++row){unsigned line=top+row;
  if(all||(row>=exposed_first&&row<=exposed_last)||editor_cursors[row]<width||(view.damage&&line>=view.damage_first&&line<=view.damage_last)||((view.damage||view.cursor!=wb.cursor||view.focus!=wb.focus)&&(row==cursor_row||row==editor_cursor_row)))
   editor_row(row,line,left,wb.focus==VIOS_WB_EDITOR);
 }
 view.editor_valid=1;view.top=top;view.left=left;view.cursor=wb.cursor;view.focus=wb.focus;editor_cursor_row=cursor_row;view.damage=0;view.shift_valid=0;
}
static struct {
 fcell value,target,stack[3];
 unsigned depth,base,words;
 size_t data_bytes;
 uint32_t revision;
 unsigned length;
 int value_valid,has_draft,dirty,staged,active,saved;
 enum vios_workbench_focus focus;
 char module[32],error[128];
} sampled;
void vios_workbench_compose(void){
 uint32_t generation=vios_text_generation(),catalog=forth_catalog_revision();
 int full=!view.valid||view.generation!=generation;
 if(full){vios_text_begin_frame();view.editor_valid=0;view.browser_valid=0;}
 if(!wb.open){view.valid=0;return;}
 refresh_selection();
 if(full){text(0,0,"VIOS  TEXT WORKBENCH",80,2);for(unsigned y=1;y<(wb.zoom?18u:56u);++y)text(25,y,"|",1,2);
  text(26,1,"INSPECTOR",54,2);text(0,56,"F5 Stage/Test  F6 Activate  F7 Save  F8 Undo  F10 Close",79,2);
  text(0,59,"TAB focus  Left/Right modules/words  Enter edit  Serial CLI available",79,3);}
 int selection_changed=full||view.catalog!=catalog||view.browser!=wb.browser||view.selected!=wb.selected||strcmp(view.filter,wb.filter);
 browser_compose(full,catalog);
 struct forth_word_info word;struct forth_module_info module;
 int has_word=vios_workbench_selected_word(&word),has_module=!has_word&&wb.selection[0]&&module_info(wb.selection,&module);
 for(unsigned row=2;row<=9;++row){
  if(!selection_changed&&(row<8||!has_word||(row==8&&sampled.target==word.target_xt)||(row==9&&sampled.value_valid==word.value_valid&&sampled.value==word.value)))continue;
  row_begin();
  if(has_word){switch(row){
   case 2:text(26,row,word.name,54,3);break;
   case 3:text(26,row,word.module?word.module:"kernel / read-only",54,3);break;
   case 4:text(26,row,word.module_version?word.module_version:"",54,3);break;
   case 5:text(26,row,"XT:",4,3);number(30,row,(unsigned)word.xt,16);break;
   case 6:text(26,row,"Token bytes:",16,3);number(42,row,word.token_bytes,12);break;
   case 7:text(26,row,"Direct bytes:",16,3);number(42,row,word.direct_bytes,12);break;
   case 8:if(word.target_name){text(26,row,"DEFER ->",9,3);text(35,row,word.target_name,45,3);}break;
   case 9:if(word.value_valid){text(26,row,"Value:",8,3);number(34,row,(unsigned)word.value,16);}break;
  }}else if(has_module){switch(row){
   case 2:text(26,row,module.name,54,3);break;
   case 3:text(26,row,module.version,54,3);break;
   case 4:text(26,row,"Source bytes:",16,3);number(42,row,module.source_length,12);break;
   case 5:text(26,row,"Dependencies:",16,3);number(42,row,module.dependency_count,12);break;
  }}row_end(row-2,26,row,54,full);
 }
 unsigned depth=(unsigned)fvm_depth,base=(unsigned)forth_get_base();
 if(full||sampled.depth!=depth||sampled.base!=base){row_begin();text(26,11,"DS:",4,3);number(30,11,(unsigned)fvm_depth,10);text(42,11,"BASE:",6,3);number(48,11,(unsigned)forth_get_base(),10);row_end(8,26,11,54,full);}
 int stack_changed=full||sampled.depth!=depth;
 for(unsigned i=0;i<3;++i){fcell value=i<depth?fvm_ds[depth-1-i]:0;if(sampled.stack[i]!=value)stack_changed=1;sampled.stack[i]=value;}
 if(stack_changed){row_begin();for(unsigned i=0;i<3&&i<fvm_depth;++i)number(26+i*17,12,(unsigned)fvm_ds[fvm_depth-1-i],16);row_end(9,26,12,54,full);}
 struct forth_resources resources;forth_inspect_resources(&resources);
 if(full||sampled.words!=resources.words||sampled.data_bytes!=resources.data_bytes){row_begin();text(26,14,"Words:",8,3);number(34,14,resources.words,8);text(44,14,"Data:",7,3);number(51,14,(unsigned)resources.data_bytes,9);row_end(10,26,14,54,full);}
 if(full||sampled.focus!=wb.focus){row_begin();text(26,16,wb.focus==VIOS_WB_EDITOR?"SOURCE [focused]":"SOURCE",54,2);row_end(11,26,16,54,full);}
 if(full||sampled.has_draft!=wb.has_draft||strcmp(sampled.module,wb.module)){row_begin();text(26,17,wb.has_draft?wb.module:"Select module and Enter to edit",54,3);row_end(12,26,17,54,full);bounded_copy(sampled.module,wb.module,sizeof sampled.module);}
 editor_compose(full);
 int staged=wb.staged&&pending_matches(),active=exact_active();
 if(full||sampled.dirty!=wb.dirty||sampled.staged!=staged||sampled.active!=active||sampled.saved!=wb.saved||sampled.length!=wb.length||sampled.revision!=wb.revision){row_begin();text(0,57,wb.dirty?"MODIFIED":"CLEAN",10,wb.dirty?1:3);if(staged)text(12,57,"STAGED",8,2);if(active)text(22,57,"ACTIVE",8,2);if(wb.saved&&active)text(32,57,"SAVED",8,2);text(44,57,"Bytes:",7,3);number(51,57,wb.length,5);text(58,57,"/4096 Rev:",11,3);number(69,57,(unsigned)wb.revision,10);row_end(13,0,57,80,full);}
 if(full||strcmp(sampled.error,wb.error)){row_begin();text(0,58,wb.error,79,1);row_end(14,0,58,80,full);bounded_copy(sampled.error,wb.error,sizeof sampled.error);}
 sampled.depth=depth;sampled.base=base;sampled.words=resources.words;sampled.data_bytes=resources.data_bytes;
 sampled.has_draft=wb.has_draft;sampled.focus=wb.focus;sampled.dirty=wb.dirty;sampled.staged=staged;sampled.active=active;sampled.saved=wb.saved;sampled.length=wb.length;sampled.revision=wb.revision;
 if(has_word){sampled.value=word.value;sampled.value_valid=word.value_valid;sampled.target=word.target_xt;}
 view.generation=vios_text_generation();view.catalog=catalog;view.valid=1;
}

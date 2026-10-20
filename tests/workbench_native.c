#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vios/workbench.h"
static char cells[60][80];
static unsigned char attributes[60][80];
static unsigned frame_calls,put_cells,clear_cells,source_rows;
static uint32_t screen_generation;
static unsigned scroll_calls,scroll_x,scroll_y,scroll_width,scroll_height;
static int scroll_rows,scroll_refuse;
int vios_text_scroll(unsigned x,unsigned y,unsigned width,unsigned height,int rows){
 ++scroll_calls;scroll_x=x;scroll_y=y;scroll_width=width;scroll_height=height;scroll_rows=rows;
 if(scroll_refuse)return 0;
 assert(x+width<=80&&y+height<=60&&rows&&rows<32&&rows>-32);
 unsigned distance=(unsigned)(rows<0?-rows:rows);assert(distance<height);
 if(rows>0){for(unsigned r=0;r<height-distance;++r){memmove(cells[y+r]+x,cells[y+r+distance]+x,width);memmove(attributes[y+r]+x,attributes[y+r+distance]+x,width);}}
 else {for(unsigned r=height;r-->distance;){memmove(cells[y+r]+x,cells[y+r-distance]+x,width);memmove(attributes[y+r]+x,attributes[y+r-distance]+x,width);}}
 unsigned blank=rows>0?y+height-distance:y;
 for(unsigned r=blank;r<blank+distance;++r){memset(cells[r]+x,' ',width);memset(attributes[r]+x,3,width);}
 return 1;
}
uint32_t vios_text_generation(void){return screen_generation;}
void vios_text_begin_frame(void){++screen_generation;++frame_calls;memset(cells,' ',sizeof cells);memset(attributes,3,sizeof attributes);}
void vios_text_clear(unsigned x,unsigned y,unsigned w,unsigned h){assert(x+w<=80&&y+h<=60);clear_cells+=w*h;for(unsigned row=y;row<y+h;++row){memset(cells[row]+x,' ',w);memset(attributes[row]+x,3,w);if(x>=26&&row>=18&&row<56)source_rows|=1u<<((row-18)%32);}}
void vios_text_put(unsigned x,unsigned y,const char *s,size_t n,unsigned fg,unsigned bg){assert(y<60&&x+n<=80);put_cells+=(unsigned)n;if(x>=26&&y>=18&&y<56)source_rows|=1u<<((y-18)%32);memcpy(cells[y]+x,s,n);memset(attributes[y]+x,(int)(fg|(bg<<4)),n);}
void platform_console_putc(unsigned char c){(void)c;}
int platform_console_getc(void){return -1;}
void platform_console_init(void){}
const char *platform_identity(void){return "test";}
void platform_restart(void){}
static struct vios_workbench_status status(void){struct vios_workbench_status s;vios_workbench_status(&s);return s;}
static void fresh(void){scroll_calls=0;scroll_refuse=0;forth_init();vios_workbench_init();vios_workbench_open();}
static void install(const char *name,const char *src){assert(forth_module_stage(name,"1",NULL,0,src,NULL));assert(forth_module_activate(name));}
static fcell result(const char *src){assert(forth_eval(src));return fvm_pop();}
static void editor(void){
 fresh();const char *src="\\ exact\r\n\t: VALUE 22 ;\n";install("sample",src);
 assert(vios_workbench_edit("sample",0));assert(strcmp(vios_workbench_source(),src)==0);
 struct vios_workbench_status s=status();assert(s.activated&&!s.dirty&&s.length==strlen(src));
 assert(vios_workbench_patch(s.revision,0,0," ",1));assert(status().dirty);
 assert(!vios_workbench_patch(s.revision,0,0,"x",1));assert(strstr(vios_workbench_error(),"stale"));
 char old[4097];strcpy(old,vios_workbench_source());
 assert(!vios_workbench_patch(status().revision,9000,0,"x",1));assert(!strcmp(old,vios_workbench_source()));
 assert(!vios_workbench_patch(status().revision,0,0,"\033",1));assert(!strcmp(old,vios_workbench_source()));
 assert(!vios_workbench_patch(status().revision,0,0,"\0",1));
 assert(!vios_workbench_new("other","1",0));assert(!strcmp(old,vios_workbench_source()));
 assert(vios_workbench_undo(status().revision));assert(!strcmp(src,vios_workbench_source()));assert(!status().dirty);
 assert(!vios_workbench_undo(status().revision));
 vios_workbench_close();assert(!vios_workbench_active());vios_workbench_open();assert(!strcmp(src,vios_workbench_source()));
 assert(vios_workbench_new("other","1",1));char full[4096];memset(full,' ',sizeof full);
 assert(vios_workbench_patch(status().revision,0,0,full,sizeof full));
 assert(!vios_workbench_patch(status().revision,4096,0,"x",1));assert(status().length==4096);
 assert(vios_workbench_undo(status().revision));assert(status().length==0);
 assert(vios_workbench_patch(status().revision,0,0,"abc\ndef",7));
 assert(vios_workbench_key(9,0)==VIOS_WB_NONE); /* editor focus */
 vios_workbench_key(VIOS_WB_HOME,0);assert(status().cursor==4);
 vios_workbench_key(8,0);assert(!strcmp(vios_workbench_source(),"abcdef"));
 vios_workbench_key(VIOS_WB_HOME,0);vios_workbench_key(VIOS_WB_DELETE,0);assert(!strcmp(vios_workbench_source(),"bcdef"));
 vios_workbench_key('A',2|16|32);assert(!strcmp(vios_workbench_source(),"Abcdef"));
 vios_workbench_key('X',1);assert(!strcmp(vios_workbench_source(),"Abcdef"));
 assert(vios_workbench_key(VIOS_WB_F5,0)==VIOS_WB_STAGE);
 assert(vios_workbench_key(VIOS_WB_F6,0)==VIOS_WB_ACTIVATE);
 assert(vios_workbench_key(VIOS_WB_F7,0)==VIOS_WB_SAVE);
 assert(vios_workbench_key(VIOS_WB_F10,0)==VIOS_WB_CLOSED);
}
static void viewport(void){
 fresh();assert(vios_workbench_new("long","1",0));char line[101];memset(line,'a',100);line[100]='Z';
 assert(vios_workbench_patch(status().revision,0,0,line,sizeof line));
 vios_workbench_key(9,0);vios_workbench_compose();
 assert(cells[18][78]=='Z'&&attributes[18][79]==32);
 assert(vios_workbench_key(VIOS_WB_HOME,0)==VIOS_WB_NONE);vios_workbench_compose();
 assert(cells[18][26]=='a'&&attributes[18][26]==32);
}
static void transaction(void){
 fresh();assert(forth_eval("DEFER SERVICE : OLD 11 ; ' OLD IS SERVICE VARIABLE X 7 X !"));
 install("provider",": VALUE 22 ; ' VALUE IS SERVICE");
 assert(vios_workbench_edit("provider",0));assert(vios_workbench_can_save(status().revision));
 assert(vios_workbench_mark_saved(status().revision));assert(status().saved);
 assert(vios_workbench_set_metadata(status().revision,"2","",NULL,0));
 const char *bad=": VALUE 33 ; 99 X ! ' VALUE IS SERVICE MISSING";
 assert(vios_workbench_patch(status().revision,0,status().length,bad,strlen(bad)));
 assert(forth_eval("HEX"));assert(fvm_push(123));
 assert(!vios_workbench_stage(status().revision));assert(fvm_depth==1&&fvm_pop()==123&&forth_get_base()==16);
 assert(result("SERVICE")==22&&result("X @")==7);assert(!strcmp(vios_workbench_source(),bad));
 const char *good=": VALUE 33 ; ' VALUE IS SERVICE";
 assert(vios_workbench_patch(status().revision,0,status().length,good,strlen(good)));
 assert(vios_workbench_stage(status().revision));assert(status().staged);
 assert(result("SERVICE")==22);assert(!vios_workbench_can_save(status().revision));
 assert(vios_workbench_activate(status().revision));assert(result("SERVICE")==33);
 assert(vios_workbench_can_save(status().revision));assert(vios_workbench_mark_saved(status().revision));
 assert(!status().dirty&&status().saved);
 assert(vios_workbench_set_metadata(status().revision,"3","",NULL,0));
 /* A third generation retires the first (forth/modules.c). */
 assert(vios_workbench_stage(status().revision));assert(vios_workbench_activate(status().revision));
 assert(result("SERVICE")==33);
}
static void aliased_metadata(void){
 fresh();assert(vios_workbench_new("APP","1",0));
 assert(vios_workbench_set_metadata(status().revision,"V2","CHECK",NULL,0));
 struct vios_workbench_status before=status();
 assert(vios_workbench_set_metadata(before.revision,before.test_word,before.version,NULL,0));
 assert(!strcmp(status().version,"CHECK")&&!strcmp(status().test_word,"V2"));
 before=status();assert(vios_workbench_new(before.version,before.module,1));
 assert(!strcmp(status().module,"CHECK")&&!strcmp(status().version,"APP"));
}
static void activation_failure(void){
 fresh();assert(forth_eval("VARIABLE X 7 X ! DEFER SERVICE : OLD 11 ; ' OLD IS SERVICE"));
 assert(vios_workbench_new("guarded","1",0));
 const char *src=": VALUE 22 ; ' VALUE IS SERVICE : CHECK X @ 7 = ;";
 assert(vios_workbench_patch(status().revision,0,0,src,strlen(src)));
 assert(vios_workbench_set_metadata(status().revision,"1","CHECK",NULL,0));
 assert(vios_workbench_stage(status().revision));
 assert(forth_eval("9 X ! HEX"));assert(fvm_push(123));
 assert(!vios_workbench_activate(status().revision));
 assert(fvm_depth==1&&fvm_pop()==123&&forth_get_base()==16);
 assert(result("SERVICE")==11&&result("X @")==9);
 assert(status().staged&&!status().activated&&!strcmp(vios_workbench_source(),src));
 char error[128];strcpy(error,vios_workbench_error());
 assert(!forth_eval("MISSING"));assert(!strcmp(vios_workbench_error(),error));
 assert(forth_eval("7 X !"));assert(vios_workbench_activate(status().revision));
 assert(result("SERVICE")==22);
}
static void pending_identity(void){
 fresh();install("base",": BASEWORD 9 ;");install("other",": OTHERWORD 8 ;");
 struct forth_module_dependency first={"base",forth_module_crc32(": BASEWORD 9 ;")};
 struct forth_module_dependency second={"other",forth_module_crc32(": OTHERWORD 8 ;")};
 assert(vios_workbench_new("client","1",0));const char *src=": CLIENT 7 ;";
 assert(vios_workbench_patch(status().revision,0,0,src,strlen(src)));
 assert(vios_workbench_set_metadata(status().revision,"1","",&first,1));
 assert(vios_workbench_stage(status().revision));struct forth_module_dependency pending;
 assert(forth_module_pending_dependency_at(0,&pending));assert(!strcmp(pending.name,"base"));
 assert(!forth_module_pending_dependency_at(1,&pending));assert(!forth_module_pending_dependency_at(0,NULL));
 assert(forth_module_discard());assert(!forth_module_pending_dependency_at(0,&pending));
 assert(forth_module_stage("client","1",&second,1,src,NULL));
 assert(!status().staged);assert(!vios_workbench_activate(status().revision));
 assert(!forth_find_xt("CLIENT"));assert(vios_workbench_patch(status().revision,0,0," ",1));
 assert(forth_module_pending_dependency_at(0,&pending));assert(!strcmp(pending.name,"other"));
 assert(forth_module_discard());assert(vios_workbench_stage(status().revision));assert(vios_workbench_activate(status().revision));
}
static void copied_dependencies(void){
 fresh();install("base",": BASEWORD 9 ;");
 struct forth_module_dependency dep={"base",forth_module_crc32(": BASEWORD 9 ;")};
 assert(forth_module_stage("client","1",&dep,1,": CLIENT BASEWORD ;",NULL));assert(forth_module_activate("client"));
 assert(vios_workbench_edit("client",0));assert(status().dependency_count==1);
 assert(!strcmp(status().dependencies[0].name,"base"));
 char name[32]="base",version[32]="2",test[32]="";dep.name=name;
 assert(vios_workbench_set_metadata(status().revision,version,test,&dep,1));
 strcpy(name,"damaged");strcpy(version,"bad");
 assert(!strcmp(status().version,"2")&&!strcmp(status().dependencies[0].name,"base"));
 assert(vios_workbench_stage(status().revision));assert(vios_workbench_activate(status().revision));
 assert(vios_workbench_can_save(status().revision));
 assert(forth_module_rollback("client"));assert(!status().activated&&!status().saved);
 assert(!vios_workbench_can_save(status().revision));
}
static void browsing(void){
 fresh();install("alpha",": APP 42 ;");assert(vios_workbench_browse(VIOS_WB_MODULES,NULL));
 assert(status().browser_count==1);assert(vios_workbench_select_module("alpha"));
 vios_workbench_key(VIOS_WB_RIGHT,0);assert(status().browser==VIOS_WB_WORDS&&status().browser_count==1);
 vios_workbench_key(VIOS_WB_LEFT,0);assert(status().browser==VIOS_WB_MODULES);
 assert(vios_workbench_browse(VIOS_WB_WORDS,"alpha"));assert(status().browser_count==1);
 struct forth_word_info word;assert(vios_workbench_selected_word(&word));assert(!strcmp(word.name,"APP"));
 assert(vios_workbench_edit("alpha",0));assert(fvm_push(888));assert(forth_eval("HEX"));
 vios_workbench_compose();assert(fvm_depth==1&&fvm_ds[0]==888&&forth_get_base()==16);
 assert(!memcmp(cells[0],"VIOS",4));
 assert(vios_workbench_set_metadata(status().revision,"2","",NULL,0));
 assert(forth_eval(": OUTSIDE 1 ;"));assert(!vios_workbench_stage(status().revision));
 assert(strstr(vios_workbench_error(),"catalog"));
}
static void equivalent_full(void){
 char saved[60][80];unsigned char attrs[60][80];
 vios_workbench_compose();memcpy(saved,cells,sizeof saved);memcpy(attrs,attributes,sizeof attrs);
 vios_text_begin_frame();vios_workbench_compose();
 assert(!memcmp(saved,cells,sizeof saved));assert(!memcmp(attrs,attributes,sizeof attrs));
}
static void incremental(void){
 fresh();assert(vios_workbench_new("lines","1",0));
 const char *src="abc\r\n\tdef\rghi\njkl\n";
 assert(vios_workbench_patch(status().revision,0,0,src,strlen(src)));vios_workbench_key(9,0);
 vios_workbench_compose();frame_calls=put_cells=clear_cells=source_rows=0;
 vios_workbench_compose();assert(!frame_calls&&!put_cells&&!clear_cells);
 vios_workbench_key(VIOS_WB_LEFT,0);vios_workbench_compose();
 assert(!frame_calls&&put_cells+clear_cells<400);equivalent_full();
 assert(vios_workbench_patch(status().revision,1,1,"Z",1));
 frame_calls=put_cells=clear_cells=source_rows=0;vios_workbench_compose();
 assert(!frame_calls&&put_cells+clear_cells<600);assert(cells[18][27]=='Z');equivalent_full();
 assert(forth_eval("VARIABLE IV 123 IV ! DEFER ID : IT 7 ; ' IT IS ID"));
 unsigned iv=0;struct forth_word_info w;for(;iv<forth_word_count();++iv){assert(forth_inspect_word(iv,&w));if(!strcmp(w.name,"IV"))break;}
 assert(vios_workbench_select_word(iv));equivalent_full();assert(!memcmp(cells[9]+34,"123",3));
 assert(forth_eval("9 IV ! HEX 99"));vios_workbench_compose();assert(cells[9][34]=='9'&&cells[9][35]==' ');assert(!memcmp(cells[11]+48,"16",2));equivalent_full();
 assert(fvm_pop()==0x99);assert(forth_eval("DECIMAL"));
 /* Multiple edits before compose exercise damage merging, CRLF boundaries,
  * tabs, shrinking lines, newlines and sources exceeding one viewport. */
 unsigned seed=9317;const char *inserts[]={"x","\n","\r","\t","\r\n","","longer"};
 for(unsigned i=0;i<360;++i){
  seed=seed*1664525u+1013904223u;struct vios_workbench_status s=status();
  unsigned offset=seed%(s.length+1),remove=offset<s.length?(seed>>13)%((s.length-offset<3?s.length-offset:3)+1):0;
  const char *insert=inserts[(seed>>20)%7];assert(vios_workbench_patch(s.revision,offset,remove,insert,strlen(insert)));
  if(i%3==0){vios_workbench_key(VIOS_WB_LEFT,0);vios_workbench_key(VIOS_WB_UP,0);}
  if(i%5==0)vios_workbench_key(9,0);
  if(i%7==0)assert(vios_workbench_undo(status().revision));
  if(i%2)equivalent_full();
 }
 assert(vios_workbench_new("many","1",1));char many[2000];unsigned n=0;
 for(unsigned i=0;i<65;++i){memcpy(many+n,"line\twith text\r\n",16);n+=16;}
 assert(vios_workbench_patch(status().revision,0,0,many,n));equivalent_full();
 for(unsigned i=0;i<80;++i){vios_workbench_key(VIOS_WB_UP,0);equivalent_full();}
 /* The maximum number of lines includes the trailing empty line. */
 assert(vios_workbench_new("maxlines","1",1));char maxlines[4096];memset(maxlines,'\n',sizeof maxlines);
 assert(vios_workbench_patch(status().revision,0,0,maxlines,sizeof maxlines));equivalent_full();
 assert(vios_workbench_patch(status().revision,4095,1,"\r",1));equivalent_full();
 assert(vios_workbench_undo(status().revision));equivalent_full();
 vios_workbench_close();equivalent_full();vios_workbench_open();equivalent_full();
}
static void cursor_line_shift(void){
 fresh();assert(vios_workbench_new("shift","1",0));
 assert(vios_workbench_patch(status().revision,0,0,"a\n\n",3));vios_workbench_key(9,0);vios_workbench_compose();
 assert(attributes[20][26]==32);
 assert(vios_workbench_patch(status().revision,1,0,"\t",1));vios_workbench_key(VIOS_WB_DOWN,0);
 assert(status().cursor==3);vios_workbench_compose();
 assert(attributes[19][26]==32&&attributes[20][26]==3);equivalent_full();
 assert(vios_workbench_new("shift","1",1));assert(vios_workbench_patch(status().revision,0,0,"a\nb",3));vios_workbench_compose();
 assert(attributes[19][27]==32);
 assert(vios_workbench_patch(status().revision,0,0,"x",1));vios_workbench_key(VIOS_WB_DOWN,0);vios_workbench_key(VIOS_WB_LEFT,0);
 assert(status().cursor==3);vios_workbench_compose();assert(attributes[19][26]==32&&attributes[19][27]==3);equivalent_full();
}
static void sparse_line_changes(void){
 fresh();assert(vios_workbench_new("scroll","1",0));char src[2000];unsigned n=0;
 for(unsigned i=0;i<50;++i)n+=(unsigned)snprintf(src+n,sizeof src-n,"\\ line row %02u content\n",i);
 assert(vios_workbench_patch(status().revision,0,0,src,n));vios_workbench_key(9,0);vios_workbench_compose();
 put_cells=clear_cells=frame_calls=0;vios_workbench_key(VIOS_WB_UP,0);vios_workbench_compose();
 /* Every visible line moves, but most columns are identical. Composition
  * should touch the changed digits/cursor, rather than 38 complete rows. */
 assert(!frame_calls&&put_cells+clear_cells<200);equivalent_full();
 /* A full-frame clear already establishes blank cells; pane composition
  * must not spend another pass clearing those same rectangles. */
 vios_text_begin_frame();clear_cells=0;vios_workbench_compose();assert(!clear_cells);
}
static void source_projection_edges(void){
 fresh();assert(vios_workbench_new("projection","1",0));
 assert(vios_workbench_patch(status().revision,0,0,"ab\r\n\tcd\nZ",9));vios_workbench_key(9,0);
 assert(vios_workbench_patch(status().revision,3,0,NULL,0));vios_workbench_compose();
 assert(cells[18][26]=='a'&&cells[18][27]=='b'&&cells[18][28]==' '&&attributes[18][28]==32);
 assert(cells[19][30]=='c'&&cells[19][31]=='d');
 assert(vios_workbench_patch(status().revision,4,0,NULL,0));vios_workbench_compose();
 assert(attributes[18][28]==3&&attributes[19][26]==32);
 assert(vios_workbench_patch(status().revision,7,0,NULL,0));vios_workbench_compose();
 assert(attributes[19][26]==3&&attributes[19][32]==32);equivalent_full();
 char longline[81];memset(longline,'A',sizeof longline);longline[70]='\t';
 assert(vios_workbench_patch(status().revision,0,status().length,longline,sizeof longline));
 assert(vios_workbench_patch(status().revision,0,0,NULL,0));vios_workbench_compose();
 assert(cells[18][79]=='A'&&attributes[18][26]==32); /* tab after visible prefix */
 longline[53]='\t';assert(vios_workbench_patch(status().revision,0,status().length,longline,sizeof longline));
 assert(vios_workbench_patch(status().revision,53,0,NULL,0));vios_workbench_compose();
 assert(cells[18][78]=='A'&&cells[18][79]==' '&&attributes[18][79]==32);equivalent_full();
 assert(vios_workbench_patch(status().revision,0,status().length,NULL,0));vios_workbench_compose();
 assert(cells[18][26]==' '&&attributes[18][26]==32);for(unsigned x=27;x<80;++x)assert(cells[18][x]==' '&&attributes[18][x]==3);
}
/* A missing semantic scroll must fail the output-work bound as well as
 * these literal fixtures; the compositor, not the screen stub, chooses it. */
static void semantic_scroll(void){
 fresh();assert(vios_workbench_new("scroll","1",0));char src[3000];unsigned n=0;
 for(unsigned i=0;i<65;++i)n+=(unsigned)snprintf(src+n,sizeof src-n,"row %02u\tcontent\r\n",i);
 assert(vios_workbench_patch(status().revision,0,0,src,n));vios_workbench_key(9,0);vios_workbench_compose();
 scroll_calls=put_cells=clear_cells=0;vios_workbench_key(VIOS_WB_UP,0);vios_workbench_compose();
 assert(scroll_calls==1&&scroll_rows==-1&&scroll_x==26&&scroll_y==18&&scroll_width==54&&scroll_height==38);
 assert(!memcmp(cells[18]+26,"row 27  content",15));assert(attributes[55][26]==32);
 assert(put_cells+clear_cells<100);equivalent_full();
 scroll_calls=0;for(unsigned i=0;i<4;++i)vios_workbench_key(VIOS_WB_UP,0);vios_workbench_compose();
 assert(scroll_calls==1&&scroll_rows==-4);assert(!memcmp(cells[18]+26,"row 23  content",15));equivalent_full();
 scroll_refuse=1;scroll_calls=0;vios_workbench_key(VIOS_WB_DOWN,0);vios_workbench_compose();
 assert(scroll_calls==1);assert(!memcmp(cells[18]+26,"row 24  content",15));equivalent_full();scroll_refuse=0;
 /* Move cursor without moving the viewport, then split and rejoin a line. */
 for(unsigned i=0;i<80;++i)vios_workbench_key(VIOS_WB_UP,0);vios_workbench_compose();
 vios_workbench_key(VIOS_WB_HOME,0);vios_workbench_compose();scroll_calls=0;
 vios_workbench_key(13,0);vios_workbench_compose();assert(scroll_calls==1&&scroll_rows==-1);
 assert(cells[18][26]==' '&&!memcmp(cells[19]+26,"row 00  content",15));assert(attributes[19][26]==32);equivalent_full();
 scroll_calls=0;vios_workbench_key(8,0);vios_workbench_compose();assert(scroll_calls==1&&scroll_rows==1);
 assert(!memcmp(cells[18]+26,"row 00  content",15)&&attributes[18][26]==32);equivalent_full();
 /* Two queued edits cannot reuse an intermediate suffix blindly. */
 vios_workbench_key(13,0);vios_workbench_key(13,0);vios_workbench_compose();
 assert(cells[18][26]==' '&&cells[19][26]==' '&&!memcmp(cells[20]+26,"row 00  content",15));equivalent_full();
}
static void full_width_editor(void){
 fresh();assert(vios_workbench_new("zoom","1",0));char src[100];
 memcpy(src,"A\tBC\r\n",6);memset(src+6,'a',92);src[98]='Z';
 assert(vios_workbench_patch(status().revision,0,0,src,99));
 assert(vios_workbench_patch(status().revision,0,0,NULL,0));vios_workbench_key(9,0);vios_workbench_compose();
 char normal[60][80];unsigned char style[60][80];memcpy(normal,cells,sizeof normal);memcpy(style,attributes,sizeof style);
 uint32_t revision=status().revision;unsigned cursor=status().cursor;
 vios_workbench_key(' ',VIOS_WB_MOD_CTRL);vios_workbench_compose();
 assert(cells[18][0]=='A'&&cells[18][4]=='B'&&cells[18][5]=='C'&&attributes[18][0]==32);
 assert(cells[19][0]=='a'&&cells[19][79]=='a'&&cells[20][0]==' ');
 assert(status().revision==revision&&status().cursor==cursor);equivalent_full();
 vios_workbench_key(' ',VIOS_WB_MOD_CTRL);vios_workbench_compose();
 assert(!memcmp(cells,normal,sizeof normal)&&!memcmp(attributes,style,sizeof style));
 vios_workbench_key(' ',VIOS_WB_MOD_CTRL);vios_workbench_key(VIOS_WB_DOWN,0);vios_workbench_key(VIOS_WB_END,0);vios_workbench_compose();
 assert(cells[19][78]=='Z'&&cells[19][79]==' '&&attributes[19][79]==32);equivalent_full();
 /* Browser scrolling stays visible in the upper area without touching the
  * full-width source below it. */
 vios_workbench_key(9,0);assert(vios_workbench_browse(VIOS_WB_WORDS,NULL));vios_workbench_key(VIOS_WB_END,0);vios_workbench_compose();
 assert(cells[19][78]=='Z'&&attributes[19][79]==3);assert(cells[17][0]!=' ');equivalent_full();
 assert(cells[59][79]==' '&&attributes[59][79]==3);
}
int main(void){full_width_editor();semantic_scroll();editor();viewport();transaction();aliased_metadata();activation_failure();pending_identity();copied_dependencies();browsing();incremental();cursor_line_shift();sparse_line_changes();source_projection_edges();puts("Workbench checks passed");return 0;}

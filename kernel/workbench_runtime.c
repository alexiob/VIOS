/* Serialized ASCII controller and bounded idle adapter; no VM opcode changes. */
#include "vios/workbench_runtime.h"
#include "vios/workbench.h"
#include "vios/text_screen.h"
#include "vios/runtime.h"
#include "platform/vdp.h"
#include "platform/input.h"
extern unsigned short platform_timer_read(void);
static struct vios_text_stats totals;
static uint32_t elapsed_ticks;
static int paint_failed;
static uint32_t save_revision;
static fcell save_xt,paint_xt,paint_hook_xt;
static const char *skip(const char *p){while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')++p;return p;}
static int error(const char *s){vios_workbench_note_error(s);forth_transaction_error(s);return -1;}
static int argument(const char **rest,char *out,size_t capacity){
 const char *p=skip(*rest);size_t n=0;
 while(*p&&*p!=' '&&*p!='\t'&&*p!='\r'&&*p!='\n'){if(n+1>=capacity){error("workbench argument too long");return 0;}out[n++]=*p++;}
 out[n]=0;*rest=p;if(!n){error("missing workbench argument");return 0;}return 1;
}
static int number(const char **rest,uint32_t *out){
 char b[12];if(!argument(rest,b,sizeof b))return 0;uint32_t n=0;
 for(unsigned i=0;b[i];++i){unsigned d=(unsigned)(b[i]-'0');if(d>9||n>(UINT32_MAX-d)/10){error("invalid workbench decimal number");return 0;}n=n*10+d;}*out=n;return 1;
}
static void field(const char *name,uint32_t value){vios_out(name);vios_uint(value);}
static void hex_bytes(const char *p,size_t n){if(!n){vios_out("-");return;}for(size_t i=0;i<n;++i)vios_hex((unsigned char)p[i],2);}
static void hex_string(const char *p){hex_bytes(p,vios_length(p));}
static void status(void){
 struct vios_workbench_status s;struct forth_resources r;vios_workbench_status(&s);forth_inspect_resources(&r);
 field("WB-STATUS revision ",s.revision);field(" catalog ",s.catalog_revision);field(" open ",s.open);field(" draft ",s.has_draft);
 field(" dirty ",s.dirty);field(" staged ",s.staged);field(" active ",s.activated);field(" saved ",s.saved);field(" length ",s.length);field(" cursor ",s.cursor);
 field(" selection ",s.selection_index);field(" browser ",s.browser);field(" focus ",s.focus);field(" words ",r.words);field(" words_max ",r.words_max);
 vios_out(" module ");hex_string(s.module);vios_out(" version ");hex_string(s.version);vios_out(" test ");hex_string(s.test_word);vios_out(" error ");hex_string(s.error);vios_out("\r\n");
}
static int display_reset(void){
 static const unsigned char mode[]={22,1},query[]={23,0,0x86};
 unsigned char info[8];unsigned actual=0;platform_vdp_send(mode,sizeof mode);
 if(!platform_vdp_request(query,sizeof query,0x86,info,sizeof info,&actual)||actual!=8||info[4]!=80||info[5]!=60||info[6]!=4||info[7]!=1){
  vios_workbench_close();platform_keyboard_route(0);error("workbench requires mode 1 (80x60, four colours)");return 0;
 }
 /* Pinned VDP 1.04 mode-1 palette: black/red/yellow/white. Keep its hardware
  * palette and cached RGB colour lookup in agreement (including CHAR@). */
 static const unsigned char setup[]={17,3,17,128,12,23,1,0};
 platform_vdp_send(setup,sizeof setup);vios_text_reset();paint_failed=0;vios_workbench_note_error("");return 1;
}
static int display_repaint(void){
 /* Repainting an owned mode needs no VGA mode transition. Explicit REDRAW
  * remains the complete recovery route after raw display configuration. */
 static const unsigned char query[]={23,0,0x86};unsigned char info[8];unsigned actual=0;
 if(!platform_vdp_request(query,sizeof query,0x86,info,sizeof info,&actual)||actual!=8||info[4]!=80||info[5]!=60||info[6]!=4||info[7]!=1)return display_reset();
 static const unsigned char setup[]={26,17,3,17,128,12,23,1,0};
 platform_vdp_send(setup,sizeof setup);vios_text_reset();paint_failed=0;vios_workbench_note_error("");return 1;
}
static int paint(void){
 struct vios_text_stats before,after;vios_text_stats(&before);unsigned short start=platform_timer_read();
 vios_text_render(VIOS_TEXT_BATCH_BYTES);elapsed_ticks+=(unsigned short)(start-platform_timer_read());vios_text_stats(&after);
 totals.cells+=after.cells-before.cells;totals.spans+=after.spans-before.spans;totals.batches+=after.batches-before.batches;totals.bytes+=after.bytes-before.bytes;return 1;
}
static int observe(void){
 /* Only this boot's exact binding and native callback bypass VM isolation.
  * Replaced hooks and shadowed names retain the bounded Forth observer. */
 if(!forth_compiling()&&!forth_transaction_active()&&!fvm_rdepth&&!fvm_ldepth&&
    forth_find_xt("WB-PAINT")==paint_hook_xt&&forth_find_xt("WB-PAINT-DEFAULT")==paint_xt){
  struct forth_word_info hook;
  if(forth_inspect_word((unsigned)paint_hook_xt-1,&hook)&&hook.kind==FORTH_WORD_DEFER&&hook.target_xt==paint_xt)return paint();
 }
 char message[96];if(!forth_observe("WB-PAINT",4096,message,sizeof message)){paint_failed=1;vios_workbench_note_error(message);return 0;}return 1;
}
static int save_callback(void){
 if(!vios_workbench_can_save(save_revision))return forth_error(vios_workbench_error());
 struct vios_workbench_status s;vios_workbench_status(&s);
 return vios_module_save(s.module,NULL);
}
static int save(uint32_t revision){
 if(!vios_workbench_can_save(revision))return error(vios_workbench_error());
 if(forth_find_xt("WB-SAVE-DEFAULT")!=save_xt)return error("workbench save service was shadowed");
 /* Existing persistence errors clear DS. The observer retains caller values. */
 save_revision=revision;char message[96];
 if(!forth_observe("WB-SAVE-DEFAULT",4096,message,sizeof message))return error(message);
 return vios_workbench_mark_saved(revision)?1:error(vios_workbench_error());
}
static void close(void){vios_workbench_close();platform_keyboard_route(0);}
static int key(unsigned code,unsigned modifiers){
 enum vios_workbench_request request=vios_workbench_key(code,modifiers);struct vios_workbench_status s;vios_workbench_status(&s);
 if(request==VIOS_WB_STAGE){if(!vios_workbench_stage(s.revision))return error(vios_workbench_error());}
 else if(request==VIOS_WB_ACTIVATE){if(!vios_workbench_activate(s.revision))return error(vios_workbench_error());}
 else if(request==VIOS_WB_SAVE)return save(s.revision);
 else if(request==VIOS_WB_CLOSED)close();return 1;
}
static unsigned physical_key(const struct platform_key_event *e){
 if(e->virtual_key==PLATFORM_KEY_SPACE&&(e->modifiers&PLATFORM_MOD_CTRL))return ' ';
 switch(e->virtual_key){
 case PLATFORM_KEY_UP:case PLATFORM_KEY_KP_UP:return VIOS_WB_UP;
 case PLATFORM_KEY_DOWN:case PLATFORM_KEY_KP_DOWN:return VIOS_WB_DOWN;
 case PLATFORM_KEY_LEFT:case PLATFORM_KEY_KP_LEFT:return VIOS_WB_LEFT;
 case PLATFORM_KEY_RIGHT:case PLATFORM_KEY_KP_RIGHT:return VIOS_WB_RIGHT;
 case PLATFORM_KEY_HOME:case PLATFORM_KEY_KP_HOME:case PLATFORM_KEY_PAGEUP:case PLATFORM_KEY_KP_PAGEUP:return VIOS_WB_HOME;
 case PLATFORM_KEY_END:case PLATFORM_KEY_KP_END:case PLATFORM_KEY_PAGEDOWN:case PLATFORM_KEY_KP_PAGEDOWN:return VIOS_WB_END;
 case PLATFORM_KEY_DELETE:return VIOS_WB_DELETE;case PLATFORM_KEY_BACKSPACE:return 8;
 case PLATFORM_KEY_TAB:return 9;case PLATFORM_KEY_RETURN:case PLATFORM_KEY_KP_ENTER:return 13;
 case PLATFORM_KEY_F5:return VIOS_WB_F5;case PLATFORM_KEY_F6:return VIOS_WB_F6;case PLATFORM_KEY_F7:return VIOS_WB_F7;case PLATFORM_KEY_F8:return VIOS_WB_F8;case PLATFORM_KEY_F10:return VIOS_WB_F10;
 default:return e->ascii;
 }
}
void vios_workbench_idle(int changed){
 if(!vios_workbench_active())return;
 struct platform_key_event event;if(platform_keyboard_event(&event)&&event.down){unsigned modifiers=event.modifiers&(PLATFORM_MOD_CTRL|PLATFORM_MOD_LALT|PLATFORM_MOD_RALT|PLATFORM_MOD_GUI);key(physical_key(&event),modifiers);changed=1;}
 /* Polling leaves RX paused throughout C composition/paint. Reopen the
  * hardware flow-control window only after this bounded slice completes. */
 if(vios_workbench_active()){
  /* Compose with the real application stack before entering the observer. */
  if(changed)vios_workbench_compose();if(!paint_failed&&vios_text_pending())observe();
 }
 platform_input_resume();
}
static void render_stats(void){field("WB-RENDER-STATS cells ",totals.cells);field(" spans ",totals.spans);field(" batches ",totals.batches);field(" bytes ",totals.bytes);field(" elapsed_ticks ",elapsed_ticks);field(" pending ",vios_text_pending());vios_out("\r\n");}
int vios_workbench_command(const char *word,const char **rest){
 if(word[0]!='W'||word[1]!='B'||word[2]!='-')return 0;
 char a[162],b[32];uint32_t rev=0,x=0,y=0;int ok=1;
 if(vios_same(word,"WB-OPEN")){if(!display_reset())return -1;vios_workbench_open();platform_keyboard_route(1);}
 else if(vios_same(word,"WB-CLOSE"))close();
 else if(vios_same(word,"WB-STATUS")){}
 else if(vios_same(word,"WB-REDRAW")){if(!display_reset())return -1;}
 else if(vios_same(word,"WB-REPAINT")){if(!display_repaint())return -1;}
 else if(vios_same(word,"WB-ZOOM")){if(!number(rest,&x))return -1;if(x>1)return error("zoom must be 0 or 1");vios_workbench_zoom((int)x);}
 else if(vios_same(word,"WB-ZOOM?")){field("WB-ZOOM enabled ",(unsigned)vios_workbench_zoomed());vios_out("\r\n");return 1;}
 else if(vios_same(word,"WB-COMPOSE")){
  /* Explicit profiling without placing a clock value on the inspected DS.
   * TMR0 wraps every 227.56 ms; long full-frame samples need a 32-bit clock
   * cross-check. Output/dispatch/idle/painting are outside this interval. */
  unsigned short start=platform_timer_read();vios_workbench_compose();
  unsigned short ticks=(unsigned short)(start-platform_timer_read());
  field("WB-COMPOSE elapsed_ticks ",ticks);field(" timer_bits ",16);field(" timer_hz ",288000);vios_out("\r\n");return 1;
 }
 else if(vios_same(word,"WB-RENDER-STATS")){
  const char *p=skip(*rest);if(*p=='R'&&p[1]=='E'&&p[2]=='S'&&p[3]=='E'&&p[4]=='T'&&(!p[5]||p[5]==' '||p[5]=='\r'||p[5]=='\n'||p[5]=='\t')){*rest=p+5;totals.cells=totals.spans=totals.batches=totals.bytes=elapsed_ticks=0;}render_stats();return 1;
 }
 else if(vios_same(word,"WB-FLUSH")||vios_same(word,"WB-FLUSH-FENCED")){
  struct vios_text_stats before=totals;uint32_t start=platform_millis();vios_workbench_compose();unsigned limit=2048;
  while(vios_workbench_active()&&vios_text_pending()&&limit--&&!paint_failed)if(!observe())break;
  /* The diagnostic variant includes completed ESP framebuffer drawing, with
   * no console output before the end timestamp. Ordinary FLUSH retains its
   * previous command-drain contract and its wire-compatible result fields. */
  if(vios_same(word,"WB-FLUSH-FENCED")&&vios_workbench_active()&&!vios_text_pending()&&!paint_failed){
   static const unsigned char query[]={23,0,0x83,0,0,0,0};unsigned char glyph;unsigned actual=0;
   if(!platform_vdp_request(query,sizeof query,0x83,&glyph,1,&actual)||actual!=1){paint_failed=1;vios_workbench_note_error("workbench render fence timeout");}
  }
  field("WB-FLUSH elapsed_ms ",platform_millis()-start);field(" bytes ",totals.bytes-before.bytes);field(" spans ",totals.spans-before.spans);field(" batches ",totals.batches-before.batches);field(" cells ",totals.cells-before.cells);field(" pending ",vios_text_pending());vios_out("\r\n");return paint_failed?error(vios_workbench_error()):1;
 }
 else if(vios_same(word,"WB-NEW")){
  if(!argument(rest,a,32)||!argument(rest,b,sizeof b)||!number(rest,&x))return -1;if(x>1)return error("discard must be 0 or 1");ok=vios_workbench_new(a,b,(int)x);
 }
 else if(vios_same(word,"WB-EDIT")){if(!argument(rest,a,32)||!number(rest,&x))return -1;if(x>1)return error("discard must be 0 or 1");ok=vios_workbench_edit(a,(int)x);}
 else if(vios_same(word,"WB-PATCH")){
  if(!number(rest,&rev)||!number(rest,&x)||!number(rest,&y)||!argument(rest,a,sizeof a))return -1;
  char bytes[80];size_t n=vios_length(a);if(vios_same(a,"-"))n=0;else{if(n&1)return error("patch hex must have even length");for(size_t i=0;i<n;++i){unsigned d=a[i]>='0'&&a[i]<='9'?(unsigned)(a[i]-'0'):a[i]>='A'&&a[i]<='F'?(unsigned)(a[i]-'A'+10):a[i]>='a'&&a[i]<='f'?(unsigned)(a[i]-'a'+10):99;if(d>15)return error("invalid patch hex");if(!(i&1))bytes[i/2]=(char)(d<<4);else bytes[i/2]=(char)((unsigned char)bytes[i/2]|d);}n/=2;}
  if(x>VIOS_WB_SOURCE_MAX||y>VIOS_WB_SOURCE_MAX)return error("patch range out of bounds");ok=vios_workbench_patch(rev,(size_t)x,(size_t)y,bytes,n);
 }
 else if(vios_same(word,"WB-META")){
  if(!number(rest,&rev)||!argument(rest,a,32)||!argument(rest,b,sizeof b))return -1;struct vios_workbench_status s;vios_workbench_status(&s);ok=vios_workbench_set_metadata(rev,a,vios_same(b,"-")?"":b,s.dependencies,s.dependency_count);
 }
 else if(vios_same(word,"WB-UNDO")||vios_same(word,"WB-STAGE")||vios_same(word,"WB-ACTIVATE")||vios_same(word,"WB-SAVE")){
  if(!number(rest,&rev))return -1;if(vios_same(word,"WB-UNDO"))ok=vios_workbench_undo(rev);else if(vios_same(word,"WB-STAGE"))ok=vios_workbench_stage(rev);else if(vios_same(word,"WB-ACTIVATE"))ok=vios_workbench_activate(rev);else ok=save(rev)>0;
 }
 else if(vios_same(word,"WB-KEY")){if(!number(rest,&x)||!number(rest,&y))return -1;if(x>VIOS_WB_F10||y>255)return error("invalid workbench key");ok=key((unsigned)x,(unsigned)y)>0;}
 else if(vios_same(word,"WB-SELECT")){
  if(!argument(rest,a,32))return -1;if(vios_same(a,"WORD")){if(!number(rest,&x))return -1;if(x>=forth_word_count())return error("word index out of range");ok=vios_workbench_select_word((unsigned)x);}else if(vios_same(a,"MODULE")){if(!argument(rest,b,sizeof b))return -1;ok=vios_workbench_select_module(b);}else return error("select requires MODULE or WORD");
 }
 else if(vios_same(word,"WB-BROWSE")){
  if(!argument(rest,a,32))return -1;if(vios_same(a,"MODULES")){ok=vios_workbench_browse(VIOS_WB_MODULES,NULL);for(unsigned i=0;ok&&i<forth_module_count();++i){struct forth_module_info m;if(forth_module_info(i,&m)){field("WB-ITEM module ",i);vios_out(" name ");hex_string(m.name);vios_out(" version ");hex_string(m.version);field(" active ",m.active);vios_out("\r\n");}}}
  else if(vios_same(a,"WORDS")){b[0]=0;if(*skip(*rest)&&!argument(rest,b,sizeof b))return -1;ok=vios_workbench_browse(VIOS_WB_WORDS,b);for(unsigned i=0;ok&&i<forth_word_count();++i){struct forth_word_info w;if(forth_inspect_word(i,&w)&&(!b[0]||(w.module&&vios_same(w.module,b)))){field("WB-ITEM word ",i);field(" kind ",w.kind);vios_out(" name ");hex_string(w.name);vios_out(" module ");hex_string(w.module?w.module:"");vios_out("\r\n");}}}else return error("browse requires MODULES or WORDS");
 }
 else if(vios_same(word,"WB-SOURCE")){
  if(!number(rest,&x)||!number(rest,&y))return -1;struct vios_workbench_status s;vios_workbench_status(&s);if(!s.has_draft||x>s.length||y>80)return error("source range out of bounds");if(y>s.length-x)y=s.length-x;
  field("WB-SOURCE revision ",s.revision);field(" offset ",x);field(" total ",s.length);vios_out(" hex ");hex_bytes(vios_workbench_source()+(size_t)x,(size_t)y);vios_out("\r\n");return 1;
 }
 else if(vios_same(word,"WB-VIEW")){
  if(!number(rest,&x)||!number(rest,&y))return -1;if(x>=VIOS_TEXT_ROWS||y>VIOS_TEXT_ROWS-x)return error("view row out of bounds");vios_workbench_compose();
  for(unsigned row=(unsigned)x;row<x+y;++row){field("WB-VIEW row ",row);vios_out(" text ");for(unsigned col=0;col<80;++col){struct vios_text_cell c;vios_text_cell(col,row,&c);vios_hex(c.ch,2);}vios_out(" attr ");for(unsigned col=0;col<80;++col){struct vios_text_cell c;vios_text_cell(col,row,&c);vios_hex(c.attr,2);}vios_out("\r\n");}return 1;
 }
 else return error("unknown workbench command");
 /* The REPL idle boundary composes once after the complete command line.
  * VIEW/FLUSH explicitly compose for synchronous observations. Rebuilding
  * here and again at idle would double the cost of every ordinary request. */
 if(!ok)return error(vios_workbench_error());status();return 1;
}
int vios_workbench_runtime_init(void){
 vios_workbench_init();vios_text_reset();platform_keyboard_route(0);totals.cells=totals.spans=totals.batches=totals.bytes=elapsed_ticks=0;paint_failed=0;
 if(!forth_register_native("WB-PAINT-DEFAULT",paint,0)||!forth_register_native("WB-SAVE-DEFAULT",save_callback,0)||!forth_eval("DEFER WB-PAINT ' WB-PAINT-DEFAULT IS WB-PAINT"))return 0;
 save_xt=forth_find_xt("WB-SAVE-DEFAULT");paint_xt=forth_find_xt("WB-PAINT-DEFAULT");paint_hook_xt=forth_find_xt("WB-PAINT");
 forth_register_idle(vios_workbench_idle);return 1;
}

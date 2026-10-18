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
/* VGA REPL console view (F1 toggles with the editor). Console output is
 * tapped into a RAM ring and interpreted only at REPL idle, never from the
 * tap, into a shadow of the console text; the shadow is drawn only while the
 * view is visible, so output that arrives while the editor is shown is kept.
 * Harness framing is hidden so host commands read as plain commands: a
 * line-start "@NONCE " and following "!CRC32XX " prefix are dropped, and a
 * bare "@NONCE" frame line is skipped. Rows 0..57 scroll; row 58 is status.
 * A block cursor blinks at the terminal position (500 ms phase; solid while
 * output or typing is arriving). */
#define CON_ROWS 58
#define CON_RING 512
#define CON_BLINK_TICKS 144000u
static unsigned char con_ring[CON_RING],con_state,con_held;static unsigned short con_head,con_count;
static unsigned con_dropped,con_shown_dropped,con_col,con_row,con_cursor_x,con_cursor_y;
static int con_open,con_bol,con_home,con_cursor_on,con_redraw;static char con_hold[10];
/* Circular rows: scrolling the shadow advances con_top, no copying. */
static char con_shadow[CON_ROWS][VIOS_TEXT_COLUMNS];static unsigned con_top;
#define CON_LINE(y) con_shadow[(con_top+(y))%CON_ROWS]
static uint32_t con_blink;static unsigned short con_tick;
void vios_console_putc(unsigned char c){
 if(!con_home)return;if(con_count==CON_RING){++con_dropped;return;}
 con_ring[(con_head+con_count)%CON_RING]=c;++con_count;
}
static void con_status(void){
 char line[80];static const char text[]=" VIOS console   F1: editor   Up/Down: history   dropped ";unsigned n=0;
 for(;text[n]&&n<79;++n)line[n]=text[n];
 char d[11];unsigned k=0;uint32_t v=con_dropped;do{d[k++]=(char)('0'+v%10);v/=10;}while(v);while(k&&n<79)line[n++]=d[--k];
 while(n<80)line[n++]=' ';vios_text_put(0,CON_ROWS,line,80,0,3);con_shown_dropped=con_dropped;
}
static void con_hide_cursor(void){
 if(!con_cursor_on)return;con_cursor_on=0;
 if(con_open)vios_text_put(con_cursor_x,con_cursor_y,&CON_LINE(con_cursor_y)[con_cursor_x],1,3,0);
}
static void con_show_cursor(void){
 if(!con_open||con_cursor_on)return;con_cursor_on=1;con_cursor_x=con_col;con_cursor_y=con_row;
 vios_text_put(con_col,con_row,&CON_LINE(con_row)[con_col],1,0,3);
}
static void con_newline(void){
 con_col=0;if(con_row+1<CON_ROWS){++con_row;return;}
 for(unsigned x=0;x<VIOS_TEXT_COLUMNS;++x)CON_LINE(0)[x]=' ';con_top=(con_top+1)%CON_ROWS;
 if(!con_open)return;
 /* Hardware scroll needs a clean region: drain painting first. */
 for(unsigned guard=256;vios_text_pending()&&guard;--guard)paint();
 if(!vios_text_scroll(0,0,VIOS_TEXT_COLUMNS,CON_ROWS,1)){
  for(unsigned y=0;y<CON_ROWS;++y)vios_text_put(0,y,CON_LINE(y),VIOS_TEXT_COLUMNS,3,0);
 }
}
static void con_draw(unsigned char c){
 con_bol=c=='\n';
 if(c=='\r'){con_col=0;return;}
 if(c=='\n'){con_newline();return;}
 if(c==8){if(con_col)--con_col;return;}
 if(c<32||c>126)return;
 CON_LINE(con_row)[con_col]=(char)c;
 if(con_open)vios_text_put(con_col,con_row,&CON_LINE(con_row)[con_col],1,3,0);
 if(++con_col==VIOS_TEXT_COLUMNS)con_newline();
}
static int con_hex(unsigned char c){return (c>='0'&&c<='9')||(c>='A'&&c<='F');}
/* Host envelopes start at a line start or right after the REPL prompt. */
static int con_prompt(void){
 static const char *const prompts[]={"vios> ","compile> "};
 for(unsigned k=0;k<2;++k){unsigned n=0;while(prompts[k][n])++n;
  if(con_col!=n)continue;unsigned i=0;while(i<n&&CON_LINE(con_row)[i]==prompts[k][i])++i;if(i==n)return 1;}
 return 0;
}
static void con_release(void){unsigned n=con_held;con_held=0;con_state=0;for(unsigned i=0;i<n;++i)con_draw((unsigned char)con_hold[i]);}
static void con_byte(unsigned char c){
 /* 0 text, 1 nonce digits, 2 CRC digits, 3 skip to LF, 4 after nonce. */
 if(con_state==3){if(c=='\n'){con_state=0;con_bol=1;}return;}
 if(con_state==4){con_state=0;if(c=='!'){con_state=2;con_hold[0]='!';con_held=1;return;}}
 if(con_state==1||con_state==2){
  con_hold[con_held++]=(char)c;
  if(con_held<=9&&con_hex(c))return;
  if(con_held==10&&c==' '){con_held=0;con_state=con_state==1?4:0;return;}
  if(con_held==10&&con_state==1&&c=='\r'){con_held=0;con_state=3;return;}
  con_release();return;
 }
 if(c=='@'&&(con_bol||con_prompt())){con_state=1;con_hold[0]='@';con_held=1;return;}
 con_draw(c);
}
static void con_clear(void){
 for(unsigned y=0;y<CON_ROWS;++y)for(unsigned x=0;x<VIOS_TEXT_COLUMNS;++x)con_shadow[y][x]=' ';
 con_top=0;con_head=con_count=0;con_dropped=0;con_col=con_row=0;con_state=con_held=0;con_bol=1;
}
/* Shows the console, redrawing everything it has captured so far. */
int vios_console_open(void){
 if(!display_reset())return 0;
 if(vios_workbench_active())vios_workbench_close();
 if(!con_home)con_clear();
 con_open=con_home=1;con_cursor_on=0;con_blink=0;con_tick=platform_timer_read();con_redraw=1;
 platform_keyboard_route(0);return 1;
}
/* On hardware, cells sent right after the mode reset/CLS can be lost while
 * the VDP is still clearing; redraw on the next idle after a VDP fence. */
static void con_redraw_now(void){
 static const unsigned char query[]={23,0,0x86};unsigned char info[8];unsigned actual=0;
 if(!platform_vdp_request(query,sizeof query,0x86,info,sizeof info,&actual))return; /* retry next idle */
 con_redraw=0;
 for(unsigned y=0;y<CON_ROWS;++y){unsigned used=0;for(unsigned x=0;x<VIOS_TEXT_COLUMNS;++x)if(CON_LINE(y)[x]!=' ')used=x+1;
  if(used)vios_text_put(0,y,CON_LINE(y),used,3,0);}
 con_status();con_show_cursor();
}
static void con_close(void){con_hide_cursor();con_open=0;}
/* Interpret captured output into the shadow (drawn only when visible). */
static int con_drain(void){
 int any=con_count!=0;
 for(unsigned budget=256;con_count&&budget;--budget){unsigned char c=con_ring[con_head];con_head=(unsigned short)((con_head+1)%CON_RING);--con_count;con_byte(c);}
 return any;
}
static void console_idle(void){
 if(platform_keyboard_hotkey()){con_close();if(display_reset()){vios_workbench_open();platform_keyboard_route(1);vios_workbench_compose();}platform_input_resume();return;}
 if(con_redraw){con_redraw_now();platform_input_resume();return;}
 unsigned short now=platform_timer_read();con_blink+=(unsigned short)(con_tick-now);con_tick=now;
 if(con_count){con_hide_cursor();con_drain();con_blink=0;}
 if(con_blink>=2*CON_BLINK_TICKS)con_blink%=2*CON_BLINK_TICKS;
 if(con_blink<CON_BLINK_TICKS){if(con_cursor_on&&(con_cursor_x!=con_col||con_cursor_y!=con_row))con_hide_cursor();con_show_cursor();}
 else con_hide_cursor();
 if(con_shown_dropped!=con_dropped)con_status();
 if(!paint_failed&&vios_text_pending())paint();
 platform_input_resume();
}
/* Once opened, the console is the home view the editor returns to. */
static void close(void){int home=con_home;con_close();vios_workbench_close();platform_keyboard_route(0);if(home)(void)vios_console_open();}
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
 if(con_open){console_idle();return;}
 if(!vios_workbench_active())return;
 /* The event poll pauses RX; only then interpret console output for the
  * hidden view (bounded RAM work must never run with the peer enabled). */
 struct platform_key_event event;int got=platform_keyboard_event(&event);
 if(con_home)con_drain();
 if(got&&event.down){
  if(event.virtual_key==PLATFORM_KEY_F1){vios_console_open();platform_input_resume();return;}
  unsigned modifiers=event.modifiers&(PLATFORM_MOD_CTRL|PLATFORM_MOD_LALT|PLATFORM_MOD_RALT|PLATFORM_MOD_GUI);key(physical_key(&event),modifiers);changed=1;}
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
 if(vios_same(word,"WB-CONSOLE")){if(!vios_console_open())return -1;vios_out("WB-CONSOLE open 1\r\n");return 1;}
 if(vios_same(word,"WB-CONSOLE-CLEAR")){con_clear();if(con_open&&!vios_console_open())return -1;vios_out("WB-CONSOLE cleared\r\n");return 1;}
 if(vios_same(word,"WB-CONSOLE?")){field("WB-CONSOLE open ",(unsigned)con_open);field(" row ",con_row);field(" pending ",con_count);field(" dropped ",con_dropped);vios_out("\r\n");return 1;}
 if(vios_same(word,"WB-OPEN")){if(!display_reset())return -1;con_close();vios_workbench_open();platform_keyboard_route(1);}
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

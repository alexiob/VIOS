#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "vios/workbench.h"
int old_vios_workbench_select_word(unsigned);void old_vios_workbench_init(void);void old_vios_workbench_open(void);void old_vios_workbench_close(void);void old_vios_workbench_compose(void);
int old_vios_workbench_new(const char*,const char*,int);int old_vios_workbench_patch(uint32_t,size_t,size_t,const char*,size_t);int old_vios_workbench_undo(uint32_t);
void old_vios_workbench_status(struct vios_workbench_status*);enum vios_workbench_request old_vios_workbench_key(unsigned,unsigned);
static char cells[2][60][80];static unsigned char attrs[2][60][80];static unsigned target,step;static unsigned generation[2];
uint32_t vios_text_generation(void){return generation[target];}
void vios_text_begin_frame(void){++generation[target];memset(cells[target],' ',sizeof cells[0]);memset(attrs[target],3,sizeof attrs[0]);}
void vios_text_clear(unsigned x,unsigned y,unsigned w,unsigned h){for(unsigned r=y;r<y+h;++r){memset(cells[target][r]+x,' ',w);memset(attrs[target][r]+x,3,w);}}
void vios_text_put(unsigned x,unsigned y,const char*s,size_t n,unsigned fg,unsigned bg){assert(y<60&&x+n<=80);memcpy(cells[target][y]+x,s,n);memset(attrs[target][y]+x,fg|(bg<<4),n);}
void platform_console_putc(unsigned char c){(void)c;}int platform_console_getc(void){return -1;}void platform_console_init(void){}const char*platform_identity(void){return "test";}void platform_restart(void){}
static struct vios_workbench_status status(void){struct vios_workbench_status s;vios_workbench_status(&s);return s;}
static void dump(void){const char*s=vios_workbench_source();fprintf(stderr,"SOURCE:");for(unsigned i=0;i<status().length;++i)fprintf(stderr," %02x",(unsigned char)s[i]);fprintf(stderr,"\n");}
static void same(void){target=0;old_vios_workbench_compose();target=1;vios_workbench_compose();for(unsigned y=0;y<60;++y)for(unsigned x=0;x<80;++x)if(cells[0][y][x]!=cells[1][y][x]||attrs[0][y][x]!=attrs[1][y][x]){fprintf(stderr,"step %u diff at %u,%u old %c/%u new %c/%u cursor %u len %u\n",step,x,y,cells[0][y][x],attrs[0][y][x],cells[1][y][x],attrs[1][y][x],status().cursor,status().length);dump();exit(2);}}
static void key(unsigned k){assert(old_vios_workbench_key(k,0)==vios_workbench_key(k,0));}
static void patch(unsigned at,unsigned remove,const char*s,unsigned n){uint32_t r=status().revision;assert(old_vios_workbench_patch(r,at,remove,s,n)==vios_workbench_patch(r,at,remove,s,n));}
static void cursor_reproduction(int column_case){
 forth_init();old_vios_workbench_init();vios_workbench_init();old_vios_workbench_open();vios_workbench_open();
 assert(old_vios_workbench_new("shift","1",1));assert(vios_workbench_new("shift","1",1));
 patch(0,0,column_case?"a\nb":"a\n\n",3);key(9);same();
 if(column_case){patch(0,0,"x",1);key(VIOS_WB_DOWN);key(VIOS_WB_LEFT);}
 else {patch(1,0,"\t",1);key(VIOS_WB_DOWN);}
 assert(status().cursor==3);same();printf("cursor %s reproduction: fixed output matches baseline\n",column_case?"column":"row");
}
int main(int argc,char **argv){
 if(argc==2){cursor_reproduction(!strcmp(argv[1],"--cursor-column"));return 0;}
forth_init();old_vios_workbench_init();vios_workbench_init();old_vios_workbench_open();vios_workbench_open();assert(old_vios_workbench_new("r","1",1));assert(vios_workbench_new("r","1",1));assert(forth_eval("VARIABLE RV 888 RV ! DEFER RD : LONGTARGET 1 ; : RT 2 ; ' LONGTARGET IS RD"));
unsigned rv=0,rd=0;for(unsigned j=0;j<forth_word_count();++j){struct forth_word_info w;assert(forth_inspect_word(j,&w));if(!strcmp(w.name,"RV"))rv=j;if(!strcmp(w.name,"RD"))rd=j;}
assert(old_vios_workbench_select_word(rv));assert(vios_workbench_select_word(rv));same();assert(forth_eval("9 RV !"));same();
assert(old_vios_workbench_select_word(rd));assert(vios_workbench_select_word(rd));same();assert(forth_eval("' RT IS RD"));same();assert(forth_eval(": NEWDICTIONARYWORD 44 ;"));same();
assert(fvm_push(123));assert(fvm_push(456));assert(fvm_push(789));same();assert(fvm_pop()==789);assert(fvm_pop()==456);same();assert(fvm_pop()==123);same();assert(fvm_push(17));same();assert(fvm_pop()==17);assert(fvm_push(18));same();assert(forth_eval("HEX"));same();assert(forth_eval("DECIMAL"));same();
key(9);char initial[4096];for(unsigned i=0;i<4096;++i)initial[i]=i%73==0?'\n':i%47==0?'\r':i%13==0?'\t':'a'+i%26;patch(0,0,initial,4096);same();unsigned seed=7781;const char*inserts[]={"a","\n","\r","\t","\r\n","","longer"};
for(step=0;step<30000;++step){seed=seed*1664525u+1013904223u;struct vios_workbench_status s=status();unsigned op=(seed>>24)%14;if(op<7){unsigned at=seed%(s.length+1);unsigned remove=at<s.length?(seed>>13)%((s.length-at<3?s.length-at:3)+1):0;const char*ins=inserts[op];if(s.length-remove+strlen(ins)<4097)patch(at,remove,ins,strlen(ins));}else if(op<13){key(VIOS_WB_UP+op-7);}else if(s.undo_available){assert(old_vios_workbench_undo(s.revision)==vios_workbench_undo(s.revision));}if(step%11==0)key(9);if(step%3==0)same();}same();puts("differential pass: 30000 operations, seed 7781, initial 4096 bytes; all 4800 characters and styles compared every third operation; live variable/defer/dictionary/DS/BASE scenarios passed");}

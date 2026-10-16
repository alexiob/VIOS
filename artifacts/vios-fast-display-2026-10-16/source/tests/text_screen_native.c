#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "vios/text_screen.h"
static unsigned char wire[100000];
static unsigned used,calls,scrolls,cx,cy,fg=3,bg;
static struct vios_text_cell screen[60][80];
void platform_vdp_send(const void *data,size_t length) {
 const unsigned char *p=data; assert(length<=128);assert(used+length<=sizeof wire);
 memcpy(wire+used,p,length);used+=(unsigned)length;++calls;
 for(unsigned i=0;i<length;) {
  unsigned c=p[i++];
  if(c==17) {assert(i<length);unsigned color=p[i++];if(color&128)bg=color&15;else fg=color&15;}
  else if(c==31) {assert(i+1<length);cx=p[i++];cy=p[i++];assert(cx<80&&cy<60);}
  else {assert(c>=32&&c<=126);screen[cy][cx].ch=(unsigned char)c;screen[cy][cx].attr=(unsigned char)(fg|(bg<<4));if(++cx==80){cx=0;if(++cy==60){cy=59;++scrolls;}}}
 }
}
static void drain(unsigned budget) {
 unsigned steps=0;while(vios_text_pending()){unsigned n=vios_text_render(budget);assert(n&&n<=budget&&n<=128);assert(++steps<10000);}
}
static unsigned expected_dirty(void) {
 unsigned count=0;
 for(unsigned y=0;y<60;++y)for(unsigned x=0;x<80;++x){
  struct vios_text_cell wanted;assert(vios_text_cell(x,y,&wanted));
  if(x!=79||y!=59)count+=wanted.ch!=screen[y][x].ch||wanted.attr!=screen[y][x].attr;
 }
 assert(vios_text_pending()==(count!=0));return count;
}
static void clear_rectangles(void) {
 vios_text_reset();
 for(unsigned y=0;y<60;++y)for(unsigned x=0;x<80;++x){screen[y][x].ch=' ';screen[y][x].attr=3;}
 uint32_t generation=vios_text_generation();
 vios_text_put(2,3,"ABCDE",5,1,2);drain(128);
 vios_text_clear(3,3,2,1);assert(expected_dirty()==2);drain(8);
 assert(screen[3][2].ch=='A'&&screen[3][3].ch==' '&&screen[3][3].attr==3&&screen[3][5].ch=='D');
 assert(vios_text_generation()==generation);
 vios_text_put(79,58,"X",1,2,3);vios_text_clear(79,58,~0u,~0u);
 assert(!expected_dirty());vios_text_clear(80,0,1,1);vios_text_clear(0,60,1,1);
 vios_text_clear(0,0,0,60);vios_text_clear(0,0,80,0);assert(!expected_dirty());
 vios_text_begin_frame();assert(vios_text_generation()!=generation);generation=vios_text_generation();
 vios_text_reset();assert(vios_text_generation()!=generation);
 assert(!vios_text_pending());
}
static void dirty_churn(void) {
 vios_text_reset();
 for(unsigned y=0;y<60;++y)for(unsigned x=0;x<80;++x){screen[y][x].ch=' ';screen[y][x].attr=3;}
 /* Fairness: repeated writes behind the cursor cannot starve a dirty
  * later row, including when each batch can paint only one cell. */
 vios_text_put(0,0,"A",1,3,0);vios_text_put(79,58,"B",1,3,0);
 assert(vios_text_render(8)==8&&screen[0][0].ch=='A');
 vios_text_put(0,0,"C",1,3,0);
 assert(vios_text_render(8)==8&&screen[58][79].ch=='B'&&vios_text_pending());
 assert(vios_text_render(8)==8&&screen[0][0].ch=='C'&&!vios_text_pending());
 for(unsigned step=0;step<320;++step){
  unsigned index=step*239%4799,x=index%80,y=index/80;
  char ch=(char)('!'+step%90);vios_text_put(x,y,&ch,1,step%4,(step/4)%4);expected_dirty();
  /* Repeated desired writes can change style, become clean again, or change
   * again before paint. No extra dirty flag plane is needed. */
  struct vios_text_cell actual=screen[y][x];ch=actual.ch;
  vios_text_put(x,y,&ch,1,actual.attr&15,actual.attr>>4);expected_dirty();
  ch=(char)('A'+step%26);vios_text_put(x,y,&ch,1,3,0);
  if(step%13==0)vios_text_begin_frame();
  unsigned count=expected_dirty();
  assert(vios_text_render(8)==(count?8u:0u));assert(expected_dirty()==(count?count-1:0));
  if(step%37==0){
   struct vios_text_stats before,after;vios_text_stats(&before);count=expected_dirty();
   drain(8);vios_text_stats(&after);assert(after.cells-before.cells==count);assert(!expected_dirty());
  }
 }
 unsigned count=expected_dirty();struct vios_text_stats before,after;vios_text_stats(&before);
 drain(8);vios_text_stats(&after);assert(after.cells-before.cells==count);
 vios_text_put(0,0,"pending",7,1,2);assert(vios_text_pending());vios_text_reset();assert(!vios_text_pending());
}
int main(void) {
 struct vios_text_cell cell;struct vios_text_stats stats;
 assert(sizeof cell==2);vios_text_reset();assert(!vios_text_pending());assert(!vios_text_render(128));
 assert(!vios_text_cell(80,0,&cell));assert(!vios_text_cell(0,60,&cell));
 vios_text_put(2,3,"ABC",3,2,1);assert(vios_text_pending());
 assert(!vios_text_render(0));assert(!vios_text_render(7));assert(used==0);
 drain(128);assert(used==10&&calls==1);assert(!memcmp(wire,"\021\002\021\201\037\002\003ABC",10));
 vios_text_stats(&stats);assert(stats.cells==3&&stats.spans==1&&stats.batches==1&&stats.bytes==10);
 vios_text_begin_frame();vios_text_put(2,3,"ABC",3,2,1);assert(!vios_text_pending());assert(!vios_text_render(128));
 vios_text_put(3,3,"Z",1,2,1);assert(vios_text_render(8)==8);assert(screen[3][3].ch=='Z');
 const char unsafe[]={0,10,17,31,127,(char)255,'!'};vios_text_put(78,1,unsafe,sizeof unsafe,3,0);
 assert(vios_text_cell(78,1,&cell)&&cell.ch=='?');assert(vios_text_cell(79,1,&cell)&&cell.ch=='?');
 drain(8);assert(screen[1][78].ch=='?'&&screen[1][79].ch=='?');
 /* Adjacent styles reuse the cursor while changing both colors. */
 vios_text_put(0,0,"ab",2,1,2);vios_text_put(1,0,"b",1,2,3);vios_text_stats_reset();drain(128);
 vios_text_stats(&stats);assert(stats.cells==2&&stats.spans==2&&stats.bytes==13);
 /* New desired frames clear previous content and repaint erased cells. */
 vios_text_begin_frame();drain(128);assert(screen[3][2].ch==' ');
 /* A complete grid never prints bottom-right, which would scroll mode 1. */
 char row[80];memset(row,'A',sizeof row);vios_text_reset();
 for(unsigned y=0;y<60;++y)vios_text_put(0,y,row,sizeof row,3,0);
 assert(vios_text_cell(79,59,&cell)&&cell.ch==' ');drain(128);assert(!scrolls);
 vios_text_stats(&stats);assert(stats.cells==4799&&stats.bytes==5079&&stats.batches==40);
 for(unsigned y=0;y<60;++y)for(unsigned x=0;x<80;++x)if(x!=79||y!=59)assert(screen[y][x].ch=='A');
 unsigned old=used;vios_text_begin_frame();for(unsigned y=0;y<60;++y)vios_text_put(0,y,row,sizeof row,3,0);assert(!vios_text_pending());assert(!vios_text_render(128)&&used==old);
 /* An edit behind the persistent cursor is still found; tiny batches progress. */
 vios_text_put(79,58,"z",1,0,3);assert(vios_text_render(8)==8);vios_text_put(0,0,"x",1,1,0);drain(8);assert(screen[0][0].ch=='x'&&!scrolls);
 dirty_churn();
 clear_rectangles();
 puts("text screen bounded dirty spans, safe glyphs, styles and no scrolling passed");return 0;
}

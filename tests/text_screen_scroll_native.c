#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "vios/text_screen.h"
static struct vios_text_cell frame[60][80];
static unsigned left,top,right=79,bottom=59,cx,cy,fg=3,bg;
static unsigned wire_bytes,buffer_writes,buffer_clears,physical_scrolls;
static unsigned char buffers[2][128];static unsigned lengths[2];
static void process(const unsigned char *p,unsigned n) {
 for(unsigned i=0;i<n;){unsigned c=p[i++];
  if(c==17){assert(i<n);unsigned color=p[i++];if(color&128)bg=color&15;else fg=color&15;}
  else if(c==31){assert(i+2<=n);cx=left+p[i++];cy=top+p[i++];assert(cx<=right&&cy<=bottom);}
  else if(c==28){assert(i+4<=n);left=p[i++];bottom=p[i++];right=p[i++];top=p[i++];assert(left<=right&&top<=bottom&&right<80&&bottom<60);if(cx<left)cx=left;if(cx>right)cx=right;if(cy<top)cy=top;if(cy>bottom)cy=bottom;}
  else if(c==23){assert(i<n);unsigned command=p[i++];
   if(command==7){assert(i+3<=n);assert(p[i++]==0);unsigned direction=p[i++],pixels=p[i++];assert(pixels&&pixels%8==0);int rows=(int)(pixels/8);assert(direction==2||direction==3);++physical_scrolls;
    struct vios_text_cell old[60][80];memcpy(old,frame,sizeof old);
    for(unsigned y=top;y<=bottom;++y)for(unsigned x=left;x<=right;++x){int sy=(int)y+(direction==3?rows:-rows);if(sy<(int)top||sy>(int)bottom){frame[y][x].ch=' ';frame[y][x].attr=(unsigned char)(fg|(bg<<4));}else frame[y][x]=old[sy][x];}
   }else{assert(command==0&&i+4<=n&&p[i++]==0xA0);unsigned id=p[i++];id|=(unsigned)p[i++]<<8;assert(id==0xFA00||id==0xFA01);id-=0xFA00;unsigned op=p[i++];
    if(op==2){lengths[id]=0;++buffer_clears;}
    else if(op==0){assert(i+2<=n);unsigned size=p[i++];size|=(unsigned)p[i++]<<8;assert(i+size<=n&&lengths[id]+size<=128);memcpy(buffers[id]+lengths[id],p+i,size);lengths[id]+=size;i+=size;++buffer_writes;}
    else{assert(op==1&&lengths[id]);process(buffers[id],lengths[id]);}
   }
  }else{assert(c>=32&&c<=126&&cx<=right&&cy<=bottom);assert(cx!=79||cy!=59);frame[cy][cx].ch=(unsigned char)c;frame[cy][cx].attr=(unsigned char)(fg|(bg<<4));if(++cx>right){cx=left;assert(cy<bottom);++cy;}}
 }
}
void platform_vdp_send(const void *p,size_t n){assert(n&&n<=128);wire_bytes+=(unsigned)n;process(p,(unsigned)n);}
static void reset(void){vios_text_reset();left=top=cx=cy=bg=0;right=79;bottom=59;fg=3;wire_bytes=0;for(unsigned y=0;y<60;++y)for(unsigned x=0;x<80;++x){frame[y][x].ch=' ';frame[y][x].attr=3;}}
static void same(void){assert(!vios_text_pending());for(unsigned y=0;y<60;++y)for(unsigned x=0;x<80;++x){struct vios_text_cell want;assert(vios_text_cell(x,y,&want));assert(frame[y][x].ch==want.ch&&frame[y][x].attr==want.attr);}}
static void drain(unsigned budget){unsigned guard=0;while(vios_text_pending()){unsigned n=vios_text_render(budget);assert(n&&n<=budget&&n<=128&&++guard<20000);}same();}
static void seed(void){reset();vios_text_put(2,3,"AAAA",4,1,2);vios_text_put(2,4,"BBBB",4,2,1);vios_text_put(2,5,"CCCC",4,3,0);vios_text_put(2,6,"DDDD",4,0,3);vios_text_put(20,5,"outside",7,2,3);drain(128);}
/* Random pending edits catch counter drift and glyphs sent into a viewport
 * that has not been restored yet; deterministic seed keeps failures replayable. */
static unsigned rng=0x6a09e667u;
static unsigned random_value(void){rng=rng*1664525u+1013904223u;return rng;}
static void interleaved_frames(void){
 static const unsigned budgets[]={8,9,17,32,64,128};
 reset();
 for(unsigned turn=0;turn<160;++turn){
  unsigned x=random_value()%60,y=random_value()%45,width=1+random_value()%19,height=4+random_value()%10;
  int amount=(int)(1+random_value()%3);if(turn&1)amount= -amount;
  for(unsigned n=0;n<12;++n){unsigned col=x+random_value()%width,row=y+random_value()%height;char ch=(char)('A'+random_value()%26);vios_text_put(col,row,&ch,1,random_value()%4,random_value()%4);}
  drain(128);
  struct vios_text_cell old[60][80];memcpy(old,frame,sizeof old);
  assert(vios_text_scroll(x,y,width,height,amount));
  for(unsigned row=y;row<y+height;++row)for(unsigned col=x;col<x+width;++col){
   struct vios_text_cell want;assert(vios_text_cell(col,row,&want));int source=(int)row+amount;
   if(source<(int)y||source>=(int)(y+height))assert(want.ch==' '&&want.attr==3);
   else assert(want.ch==old[source][col].ch&&want.attr==old[source][col].attr);
  }
  assert(!memcmp(old,frame,sizeof old));
  unsigned budget=budgets[turn%6];assert(vios_text_render(budget)<=budget);
  for(unsigned n=0;n<9;++n){unsigned col=random_value()%80,row=random_value()%60;char ch=(char)('a'+random_value()%26);vios_text_put(col,row,&ch,1,random_value()%4,random_value()%4);}
  if(turn%11==0)vios_text_begin_frame();
  if(turn%7==0)vios_text_clear(x,y,width,height);
  drain(budget);
 }
}
int main(void){
 seed();
 assert(vios_text_scroll(2,3,4,4,1));assert(vios_text_pending());
 assert(!vios_text_scroll(2,3,4,4,1));
 vios_text_put(3,3,"X",1,0,1);vios_text_put(2,6,"new",3,1,3);vios_text_put(0,0,"O",1,2,0);
 drain(128);assert(frame[3][2].ch=='B'&&frame[3][3].ch=='X'&&frame[3][3].attr==16&&frame[6][5].ch==' '&&frame[6][5].attr==3);assert(frame[5][20].ch=='o');
 unsigned writes=buffer_writes,clears=buffer_clears,before=wire_bytes;
 assert(vios_text_scroll(2,3,4,4,1));drain(128);assert(buffer_writes==writes&&buffer_clears==clears&&wire_bytes-before==6);
 assert(vios_text_scroll(2,3,4,4,-2));drain(128);assert(frame[3][2].ch==' '&&frame[3][2].attr==3&&frame[5][3].ch=='C');
 assert(vios_text_scroll(1,2,6,6,-1));drain(128);assert(buffer_clears==clears+2);assert(lengths[1]==19);
 /* A dirty region refuses without touching desired or the physical frame. */
 vios_text_put(2,3,"Z",1,0,3);struct vios_text_cell keep;vios_text_cell(2,3,&keep);
 assert(!vios_text_scroll(2,3,4,4,1));struct vios_text_cell after;vios_text_cell(2,3,&after);assert(!memcmp(&keep,&after,sizeof keep));drain(8);
 assert(!vios_text_scroll(0,0,0,4,1));assert(!vios_text_scroll(80,0,1,4,1));assert(!vios_text_scroll(0,60,1,4,1));assert(!vios_text_scroll(79,58,2,2,1));assert(!vios_text_scroll(0,0,80,60,1));assert(!vios_text_scroll(0,0,1,4,0));assert(!vios_text_scroll(0,0,1,4,4));assert(!vios_text_scroll(0,0,1,40,32));assert(!vios_text_scroll(0,0,1,40,INT_MIN));assert(!vios_text_pending());
 /* Outside dirt does not disable the operation. Tiny budgets make progress
  * using complete VDU commands and cannot paint before viewport restoration. */
 seed();vios_text_put(30,40,"outside dirt",12,1,2);assert(vios_text_scroll(2,3,4,4,1));assert(!vios_text_render(3));
 assert(vios_text_render(8)<=8);vios_text_put(2,6,"TINY",4,2,1);drain(8);assert(frame[6][2].ch=='T');
 /* Recompose and edit while a physical scroll is still pending. */
 assert(vios_text_scroll(2,3,4,4,-1));vios_text_begin_frame();vios_text_put(78,59,"L!",2,0,3);drain(8);assert(frame[59][78].ch=='L'&&frame[59][79].ch==' ');
 /* A reset cancels queued operations and forces replacement on reuse. */
 seed();assert(vios_text_scroll(2,3,4,4,1));unsigned scrolls=physical_scrolls;reset();assert(!vios_text_render(128)&&physical_scrolls==scrolls);writes=buffer_writes;assert(vios_text_scroll(2,3,4,4,1));drain(128);assert(buffer_writes==writes+1);
 /* Sparse same-style gaps are cheaper to resend than a cursor command;
  * colors are retained inside a batch, but never between calls. */
 reset();vios_text_put(0,0,"abcde",5,1,2);drain(128);before=wire_bytes;vios_text_put(0,0,"A",1,1,2);vios_text_put(3,0,"D",1,1,2);drain(128);assert(wire_bytes-before==11);
 before=wire_bytes;vios_text_put(0,0,"a",1,1,2);vios_text_put(4,0,"E",1,1,2);drain(128);assert(wire_bytes-before==12);
 before=wire_bytes;vios_text_put(0,1,"X",1,1,2);vios_text_put(0,2,"Y",1,1,2);drain(128);assert(wire_bytes-before==12);
 reset();char line[80];memset(line,'F',80);for(unsigned y=0;y<60;++y)vios_text_put(0,y,line,80,y%4,(y/4)%4);drain(128);vios_text_begin_frame();drain(8);same();
 interleaved_frames();
 puts("scroll queue, buffer replacement, tiny budgets and compressed framebuffer equivalence passed");
}

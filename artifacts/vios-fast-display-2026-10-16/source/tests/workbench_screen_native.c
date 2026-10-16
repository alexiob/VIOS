/* Real compositor + renderer integration. The wire decoder is deliberately
 * independent of retained compositor caches and checks every painted cell. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vios/workbench.h"
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
static void reset_screen(void){vios_text_reset();left=top=cx=cy=bg=0;right=79;bottom=59;fg=3;wire_bytes=0;for(unsigned y=0;y<60;++y)for(unsigned x=0;x<80;++x){frame[y][x].ch=' ';frame[y][x].attr=3;}}
static void same(void){assert(!vios_text_pending());for(unsigned y=0;y<60;++y)for(unsigned x=0;x<80;++x){struct vios_text_cell want;assert(vios_text_cell(x,y,&want));assert(frame[y][x].ch==want.ch&&frame[y][x].attr==want.attr);}}
static void drain(unsigned budget){unsigned guard=0;while(vios_text_pending()){unsigned n=vios_text_render(budget);assert(n&&n<=budget&&n<=128&&++guard<20000);}same();}

void platform_console_putc(unsigned char c){(void)c;}
int platform_console_getc(void){return -1;}
void platform_console_init(void){}
const char *platform_identity(void){return "test";}
void platform_restart(void){}
static struct vios_workbench_status status(void){struct vios_workbench_status s;vios_workbench_status(&s);return s;}
/* Slow source-only oracle: expand whole lines, then crop a literal viewport.
 * It uses neither the production source index nor the retained row caches. */
static void source_matches(unsigned first,unsigned crop,unsigned origin,unsigned width){
 const char *source=vios_workbench_source();struct vios_workbench_status state=status();
 unsigned starts[4097],count=1;starts[0]=0;
 for(unsigned i=0;i<state.length;++i){if(source[i]=='\r'){if(i+1<state.length&&source[i+1]=='\n')++i;starts[count++]=i+1;}else if(source[i]=='\n')starts[count++]=i+1;}
 for(unsigned row=0;row<38;++row){
  unsigned char chars[80],attrs[80];memset(chars,' ',sizeof chars);memset(attrs,3,sizeof attrs);
  unsigned line=first+row;
  if(line<count){unsigned col=0,stop=line+1<count?starts[line+1]:state.length;
   for(unsigned i=starts[line];i<=stop&&i<=state.length;++i){
    unsigned char ch=(unsigned char)source[i];
    if(state.focus==VIOS_WB_EDITOR&&state.cursor==i&&col>=crop&&col-crop<width)attrs[col-crop]=32;
    if(!ch||ch=='\n'||ch=='\r'){
     if(ch=='\r'&&i+1<state.length&&source[i+1]=='\n'&&state.cursor==i+1&&state.focus==VIOS_WB_EDITOR&&col>=crop&&col-crop<width)attrs[col-crop]=32;
     break;
    }
    if(ch!='\t'&&col>=crop&&col-crop<width)chars[col-crop]=ch;
    col=ch=='\t'?(col/4+1)*4:col+1;
   }
  }
  for(unsigned x=0;x<width;++x){struct vios_text_cell actual;assert(vios_text_cell(origin+x,18+row,&actual));assert(actual.ch==chars[x]&&actual.attr==attrs[x]);}
 }
}
int main(void){
 forth_init();reset_screen();vios_workbench_init();vios_workbench_open();assert(vios_workbench_new("integration","1",0));
 char source[3000];unsigned n=0;for(unsigned i=0;i<65;++i)n+=(unsigned)snprintf(source+n,sizeof source-n,"row %02u\tcontent\r\n",i);
 assert(vios_workbench_patch(status().revision,0,0,source,n));vios_workbench_key(9,0);vios_workbench_compose();source_matches(28,0,26,54);
 /* The screen is still dirty: a scroll must fall back without cache drift. */
 vios_workbench_key(VIOS_WB_UP,0);vios_workbench_compose();source_matches(27,0,26,54);assert(!physical_scrolls);drain(8);
 unsigned before=physical_scrolls;
 vios_workbench_key(VIOS_WB_UP,0);vios_workbench_compose();source_matches(26,0,26,54);
 /* Partly transmit the queued semantic operation, then change the viewport.
  * The second compose must converge using the existing queued operation. */
 assert(vios_text_render(4)==4);vios_workbench_key(VIOS_WB_UP,0);vios_workbench_compose();source_matches(25,0,26,54);drain(8);assert(physical_scrolls==before+1);
 for(unsigned i=0;i<80;++i)vios_workbench_key(VIOS_WB_UP,0);vios_workbench_key(VIOS_WB_HOME,0);vios_workbench_compose();source_matches(0,0,26,54);drain(128);
 vios_workbench_key(13,0);vios_workbench_compose();source_matches(0,0,26,54);drain(128);
 vios_workbench_key(8,0);vios_workbench_compose();source_matches(0,0,26,54);drain(8);
 vios_workbench_key(13,0);vios_workbench_compose();assert(vios_text_render(4)==4);
 vios_workbench_zoom(1);vios_workbench_compose();source_matches(0,0,0,80);drain(8);assert(vios_workbench_zoomed());
 vios_workbench_key(13,0);vios_workbench_key(13,0);vios_workbench_compose();source_matches(0,0,0,80);drain(128);
 vios_workbench_zoom(0);vios_workbench_compose();source_matches(0,0,26,54);drain(8);assert(!vios_workbench_zoomed());
 /* Tabs before the crop must expand before clipping. */
 char tabs[72];tabs[0]='A';memset(tabs+1,'\t',70);tabs[71]='Z';
 assert(vios_workbench_patch(status().revision,0,status().length,tabs,sizeof tabs));vios_workbench_compose();source_matches(0,228,26,54);drain(128);
 vios_workbench_zoom(1);vios_workbench_compose();source_matches(0,202,0,80);drain(8);
 /* Real clean full-width source scroll uses the same physical path. */
 assert(vios_workbench_patch(status().revision,0,status().length,source,n));vios_workbench_compose();source_matches(28,0,0,80);drain(128);before=physical_scrolls;
 vios_workbench_key(VIOS_WB_UP,0);vios_workbench_compose();source_matches(27,0,0,80);drain(8);assert(physical_scrolls==before+1);
 puts("Real compositor/renderer source oracle and wire framebuffer checks passed");return 0;
}

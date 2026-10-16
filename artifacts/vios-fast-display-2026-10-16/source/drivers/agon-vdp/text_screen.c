#include "vios/text_screen.h"
#include "platform/vdp.h"
#include <string.h>
#define CELL_COUNT (VIOS_TEXT_COLUMNS*VIOS_TEXT_ROWS)
/* Exactly 19,200 bytes: no heap, per-cell flags, or third screen copy. */
static struct vios_text_cell desired[CELL_COUNT],painted[CELL_COUNT];
static unsigned paint_cursor,dirty_cells;
static unsigned char dirty_rows[VIOS_TEXT_ROWS];
static struct vios_text_stats stats;
static uint32_t generation;
/* Two dedicated buffered-command IDs: one entry per direction. The VDP
 * WRITE operation appends, so every miss MUST CLEAR before replacement.
 * No ESP character plane is kept: these entries hold only 19-byte scripts. */
struct scroll_command { unsigned char x,y,width,height,pixels; };
static struct scroll_command scroll_queue,scroll_cache[2];
static unsigned char scroll_pending,scroll_phase,scroll_direction,cache_valid[2];

uint32_t vios_text_generation(void) {return generation;}
static int dirty(unsigned index) {
 return desired[index].ch!=painted[index].ch||desired[index].attr!=painted[index].attr;
}
void vios_text_stats_reset(void) {stats.cells=stats.spans=stats.batches=stats.bytes=0;}
void vios_text_begin_frame(void) {
 ++generation;
 dirty_cells=0;unsigned index=0;
 for(unsigned row=0;row<VIOS_TEXT_ROWS;++row){
  dirty_rows[row]=0;
  for(unsigned col=0;col<VIOS_TEXT_COLUMNS;++col,++index){
   desired[index].ch=' ';desired[index].attr=3;
   if(index<CELL_COUNT-1&&(painted[index].ch!=' '||painted[index].attr!=3)){++dirty_cells;++dirty_rows[row];}
  }
 }
}
void vios_text_reset(void) {
 ++generation;
 for(unsigned i=0;i<CELL_COUNT;++i){desired[i].ch=painted[i].ch=' ';desired[i].attr=painted[i].attr=3;}
 for(unsigned row=0;row<VIOS_TEXT_ROWS;++row)dirty_rows[row]=0;
 paint_cursor=dirty_cells=0;scroll_pending=scroll_phase=0;
 cache_valid[0]=cache_valid[1]=0;vios_text_stats_reset();
}
void vios_text_clear(unsigned x,unsigned y,unsigned width,unsigned height) {
 if(x>=VIOS_TEXT_COLUMNS||y>=VIOS_TEXT_ROWS)return;
 if(width>VIOS_TEXT_COLUMNS-x)width=VIOS_TEXT_COLUMNS-x;
 if(height>VIOS_TEXT_ROWS-y)height=VIOS_TEXT_ROWS-y;
 for(unsigned row=y;row<y+height;++row){
  unsigned index=row*VIOS_TEXT_COLUMNS+x,end=index+width;
  if(end==CELL_COUNT)--end;
  for(;index<end;++index){
   if(dirty(index)){--dirty_cells;--dirty_rows[row];}
   desired[index].ch=' ';desired[index].attr=3;
   if(dirty(index)){++dirty_cells;++dirty_rows[row];}
  }
 }
}
void vios_text_put(unsigned x,unsigned y,const char *text,size_t length,unsigned fg,unsigned bg) {
 if(x>=VIOS_TEXT_COLUMNS||y>=VIOS_TEXT_ROWS||!text)return;
 unsigned available=VIOS_TEXT_COLUMNS-x;
 if(length>available)length=available;
 unsigned index=y*VIOS_TEXT_COLUMNS+x;
 unsigned char attr=(unsigned char)((fg&15)|((bg&15)<<4));
 for(size_t n=0;n<length;++n,++index) {
  /* Printing the bottom-right cell would scroll the entire VDP viewport. */
  if(index==CELL_COUNT-1)break;
  unsigned char ch=(unsigned char)text[n];
  if(dirty(index)){--dirty_cells;--dirty_rows[y];}
  desired[index].ch=ch>=32&&ch<=126?ch:'?';desired[index].attr=attr;
  if(dirty(index)){++dirty_cells;++dirty_rows[y];}
 }
}
int vios_text_cell(unsigned x,unsigned y,struct vios_text_cell *out) {
 if(x>=VIOS_TEXT_COLUMNS||y>=VIOS_TEXT_ROWS||!out)return 0;
 *out=desired[y*VIOS_TEXT_COLUMNS+x];return 1;
}
int vios_text_pending(void) {return dirty_cells!=0||scroll_pending!=0;}
int vios_text_scroll(unsigned x,unsigned y,unsigned width,unsigned height,int rows) {
 if(scroll_pending||!width||!height||x>=VIOS_TEXT_COLUMNS||y>=VIOS_TEXT_ROWS||
    width>VIOS_TEXT_COLUMNS-x||height>VIOS_TEXT_ROWS-y||!rows||rows>31||rows< -31)return 0;
 unsigned amount=(unsigned)(rows<0?-rows:rows);
 if(amount>=height||(x+width==VIOS_TEXT_COLUMNS&&y+height==VIOS_TEXT_ROWS))return 0;
 for(unsigned row=y;row<y+height;++row)if(dirty_rows[row])
  for(unsigned col=x;col<x+width;++col)if(dirty(row*VIOS_TEXT_COLUMNS+col))return 0;
 /* Copy in movement order to preserve the two authoritative planes without
  * allocating a third grid. Every affected cell is clean before and after. */
 unsigned row_bytes=width*sizeof(struct vios_text_cell);
 int source_offset=(int)(amount*VIOS_TEXT_COLUMNS);if(rows<0)source_offset= -source_offset;
 for(unsigned n=0;n<height;++n){
  unsigned row=rows>0?y+n:y+height-1-n;
  struct vios_text_cell *want=desired+row*VIOS_TEXT_COLUMNS+x;
  struct vios_text_cell *seen=painted+row*VIOS_TEXT_COLUMNS+x;
  if(n+amount<height){
   /* Distinct rows never overlap; ordered row copies preserve unread rows
    * and use the target's byte-copy loop rather than per-cell indexing. */
   memcpy(want,want+source_offset,row_bytes);
   memcpy(seen,seen+source_offset,row_bytes);
  }else{
   unsigned char *bytes=(unsigned char *)want;
   for(unsigned col=0;col<width;++col){*bytes++=' ';*bytes++=3;}
   memcpy(seen,want,row_bytes);
  }
 }
 scroll_queue.x=(unsigned char)x;scroll_queue.y=(unsigned char)y;
 scroll_queue.width=(unsigned char)width;scroll_queue.height=(unsigned char)height;
 scroll_queue.pixels=(unsigned char)(amount*8);scroll_direction=(unsigned char)(rows<0);
 scroll_phase=0;scroll_pending=1;return 1;
}
static void scroll_script(unsigned char *p) {
 p[0]=17;p[1]=3;p[2]=17;p[3]=128;
 p[4]=28;p[5]=scroll_queue.x;p[6]=(unsigned char)(scroll_queue.y+scroll_queue.height-1);
 p[7]=(unsigned char)(scroll_queue.x+scroll_queue.width-1);p[8]=scroll_queue.y;
 p[9]=23;p[10]=7;p[11]=0;p[12]=(unsigned char)(scroll_direction?2:3);p[13]=scroll_queue.pixels;
 p[14]=28;p[15]=0;p[16]=VIOS_TEXT_ROWS-1;p[17]=VIOS_TEXT_COLUMNS-1;p[18]=0;
}
static void buffer_command(unsigned char *p,unsigned command) {
 p[0]=23;p[1]=0;p[2]=0xA0;p[3]=scroll_direction;p[4]=0xFA;p[5]=(unsigned char)command;
}
static unsigned render_scroll(unsigned char *bytes,unsigned budget) {
 unsigned direction=scroll_direction;
 const struct scroll_command *cached=scroll_cache+direction;
 int hit=cache_valid[direction]&&cached->x==scroll_queue.x&&cached->y==scroll_queue.y&&
  cached->width==scroll_queue.width&&cached->height==scroll_queue.height&&cached->pixels==scroll_queue.pixels;
 if(!scroll_phase&&hit&&budget>=6){buffer_command(bytes,1);scroll_pending=0;return 6;}
 if(!scroll_phase&&!hit&&budget>=39){
  buffer_command(bytes,2);buffer_command(bytes+6,0);bytes[12]=19;bytes[13]=0;
  scroll_script(bytes+14);buffer_command(bytes+33,1);
  scroll_cache[direction]=scroll_queue;cache_valid[direction]=1;scroll_pending=0;return 39;
 }
 /* Keep small budgets useful without leaving a partially received command
  * in the VDP parser. Glyph output waits for the full viewport restoration. */
 unsigned char script[19];unsigned used=0;scroll_script(script);
 while(scroll_phase<4){
  unsigned offset=scroll_phase?4+(scroll_phase-1)*5:0,length=scroll_phase?5:4;
  if(budget-used<length)break;
  for(unsigned n=0;n<length;++n)bytes[used++]=script[offset+n];
  ++scroll_phase;
 }
 if(scroll_phase==4)scroll_pending=0;
 return used;
}
static unsigned scan_row(unsigned index,unsigned end) {
 const struct vios_text_cell *want=desired+index,*seen=painted+index;
 while(index<end){
  if(want->ch!=seen->ch||want->attr!=seen->attr)return index;
  ++index;++want;++seen;
 }
 return CELL_COUNT;
}
/* Keep row lookup out of the eZ80 batch writer: inlining combines their
 * register pressure and makes even clean-row skips spill many stack locals. */
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static unsigned find_dirty(unsigned start) {
 unsigned row=start/VIOS_TEXT_COLUMNS,row_end=(row+1)*VIOS_TEXT_COLUMNS;
 if(row_end==CELL_COUNT)--row_end;
 if(dirty_rows[row]){unsigned index=scan_row(start,row_end);if(index!=CELL_COUNT)return index;}
 const unsigned char *first=dirty_rows+row,*next=first;
 /* Skip clean rows using only their byte counters. Revisit the first row
  * after wrapping to catch changed cells before the persistent cursor. */
 do{
  if(++next==dirty_rows+VIOS_TEXT_ROWS)next=dirty_rows;
  if(*next){
   unsigned row_start=(unsigned)(next-dirty_rows)*VIOS_TEXT_COLUMNS;
   row_end=row_start+VIOS_TEXT_COLUMNS;if(row_end==CELL_COUNT)--row_end;
   return scan_row(row_start,row_end);
  }
 }while(next!=first);
 return CELL_COUNT;
}
unsigned vios_text_render(unsigned budget) {
 unsigned char bytes[VIOS_TEXT_BATCH_BYTES];unsigned used=0;
 unsigned cursor=CELL_COUNT;unsigned char fg=255,bg=255;
 if(budget>sizeof bytes)budget=sizeof bytes;
 if(scroll_pending){used=render_scroll(bytes,budget);if(scroll_pending)goto send;}
 while(dirty_cells&&used<budget) {
  unsigned index=find_dirty(paint_cursor);if(index==CELL_COUNT)break;
  unsigned row=index/VIOS_TEXT_COLUMNS,row_end=(row+1)*VIOS_TEXT_COLUMNS;
  if(row_end==CELL_COUNT)--row_end;
  unsigned char attr=desired[index].attr,new_fg=attr&15,new_bg=attr>>4;
  unsigned prefix=(fg!=new_fg?2u:0u)+(bg!=new_bg?2u:0u)+(cursor!=index?3u:0u);
  if(budget-used<prefix+1)break;
  if(fg!=new_fg){bytes[used++]=17;bytes[used++]=new_fg;fg=new_fg;}
  if(bg!=new_bg){bytes[used++]=17;bytes[used++]=(unsigned char)(128|new_bg);bg=new_bg;}
  if(cursor!=index){bytes[used++]=31;bytes[used++]=(unsigned char)(index%VIOS_TEXT_COLUMNS);bytes[used++]=(unsigned char)row;}
  ++stats.spans;unsigned clean_gap=0;
  do {
   bytes[used++]=desired[index].ch;
   if(clean_gap)--clean_gap;
   else{--dirty_cells;--dirty_rows[row];}
   painted[index]=desired[index];++stats.cells;++index;
   if(used==budget||index==row_end||desired[index].attr!=attr)break;
   if(clean_gap)continue;
   if(!dirty(index)){
    /* An unchanged gap only wins when it costs less than a new 3-byte
     * TAB. Never cross style changes or consume the final dirty glyph's
     * space: otherwise reprinting a gap could waste this entire batch. */
    unsigned next=index;
    while(next<row_end&&next-index<3&&desired[next].attr==attr&&!dirty(next))++next;
    if(next==row_end||next-index>=3||desired[next].attr!=attr||!dirty(next)||budget-used<=next-index)break;
    clean_gap=next-index;
   }
  }while(1);
  cursor=index;paint_cursor=index==CELL_COUNT-1?0:index;
 }
send:
 if(used){platform_vdp_send(bytes,used);++stats.batches;stats.bytes+=used;}
 return used;
}
void vios_text_stats(struct vios_text_stats *out) {if(out)*out=stats;}

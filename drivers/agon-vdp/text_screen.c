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
/* Set only while every painted cell is known blank (after reset). Lets a
 * full frame skip rescanning 4,800 painted cells for its dirty counts. */
static unsigned char painted_blank;

uint32_t vios_text_generation(void) {return generation;}
static int dirty(unsigned index) {
 return desired[index].ch!=painted[index].ch||desired[index].attr!=painted[index].attr;
}
int vios_text_owns_display(void) {return 0;}
void vios_text_show(void) {}
void vios_text_hide(void) {}
void vios_text_stats_reset(void) {stats.cells=stats.spans=stats.batches=stats.bytes=0;}
/* Count painted cells that differ from the blank cell (' ', attr 3). A
 * pointer walk keeps the eZ80 loop in registers instead of IX spill slots. */
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static unsigned painted_nonblank(const struct vios_text_cell *cell,unsigned count) {
 unsigned n=0;
 for(const struct vios_text_cell *end=cell+count;cell!=end;++cell)if(cell->ch!=' '||cell->attr!=3)++n;
 return n;
}
void vios_text_begin_frame(void) {
 ++generation;
 /* Blank one row, then block-copy it: distinct rows never overlap. */
 for(unsigned col=0;col<VIOS_TEXT_COLUMNS;++col){desired[col].ch=' ';desired[col].attr=3;}
 for(unsigned row=1;row<VIOS_TEXT_ROWS;++row)memcpy(desired+row*VIOS_TEXT_COLUMNS,desired,sizeof desired[0]*VIOS_TEXT_COLUMNS);
 if(painted_blank){dirty_cells=0;memset(dirty_rows,0,sizeof dirty_rows);return;}
 unsigned total=0;const struct vios_text_cell *seen=painted;
 /* The bottom-right cell is never painted, so it is never counted dirty. */
 for(unsigned row=0;row<VIOS_TEXT_ROWS;++row,seen+=VIOS_TEXT_COLUMNS){
  unsigned n=painted_nonblank(seen,row==VIOS_TEXT_ROWS-1?VIOS_TEXT_COLUMNS-1:VIOS_TEXT_COLUMNS);
  dirty_rows[row]=(unsigned char)n;total+=n;
 }
 dirty_cells=total;
}
void vios_text_reset(void) {
 ++generation;
 for(unsigned i=0;i<CELL_COUNT;++i){desired[i].ch=painted[i].ch=' ';desired[i].attr=painted[i].attr=3;}
 for(unsigned row=0;row<VIOS_TEXT_ROWS;++row)dirty_rows[row]=0;
 paint_cursor=dirty_cells=0;scroll_pending=scroll_phase=0;painted_blank=1;
 cache_valid[0]=cache_valid[1]=0;vios_text_stats_reset();
}
void vios_text_clear(unsigned x,unsigned y,unsigned width,unsigned height) {
 if(x>=VIOS_TEXT_COLUMNS||y>=VIOS_TEXT_ROWS)return;
 if(width>VIOS_TEXT_COLUMNS-x)width=VIOS_TEXT_COLUMNS-x;
 if(height>VIOS_TEXT_ROWS-y)height=VIOS_TEXT_ROWS-y;
 for(unsigned row=y;row<y+height;++row){
  unsigned index=row*VIOS_TEXT_COLUMNS+x,count=width;
  if(index+count==CELL_COUNT)--count;
  /* Pointer walk with one dirty delta per row: same counts as per-cell
   * dirty() checks before and after each store, without the calls. */
  struct vios_text_cell *want=desired+index;const struct vios_text_cell *seen=painted+index;
  unsigned delta=0;
  for(;count;--count,++want,++seen){
   if(want->ch!=seen->ch||want->attr!=seen->attr)--delta;
   want->ch=' ';want->attr=3;
   if(seen->ch!=' '||seen->attr!=3)++delta;
  }
  dirty_cells+=delta;dirty_rows[row]=(unsigned char)(dirty_rows[row]+delta);
 }
}
void vios_text_put(unsigned x,unsigned y,const char *text,size_t length,unsigned fg,unsigned bg) {
 if(x>=VIOS_TEXT_COLUMNS||y>=VIOS_TEXT_ROWS||!text)return;
 unsigned available=VIOS_TEXT_COLUMNS-x;
 if(length>available)length=available;
 /* Printing the bottom-right cell would scroll the entire VDP viewport. */
 if(y==VIOS_TEXT_ROWS-1&&length>=available)length=available-1;
 unsigned index=y*VIOS_TEXT_COLUMNS+x;
 struct vios_text_cell *want=desired+index;const struct vios_text_cell *seen=painted+index;
 unsigned char attr=(unsigned char)((fg&15)|((bg&15)<<4));
 const unsigned char *bytes=(const unsigned char *)text;unsigned delta=0;
 for(;length;--length,++want,++seen) {
  unsigned char ch=*bytes++;if(ch<32||ch>126)ch='?';
  if(want->ch!=seen->ch||want->attr!=seen->attr)--delta;
  want->ch=ch;want->attr=attr;
  if(ch!=seen->ch||attr!=seen->attr)++delta;
 }
 dirty_cells+=delta;dirty_rows[y]=(unsigned char)(dirty_rows[y]+delta);
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
   memcpy(seen,seen+source_offset,row_bytes);painted_blank=0;
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
 /* Static: a 128-byte stack buffer pushes every local out of IX's +-128
  * displacement range, turning each access into a push/add/pop sequence.
  * Rendering is not re-entrant and platform_vdp_send copies synchronously. */
 static unsigned char bytes[VIOS_TEXT_BATCH_BYTES];unsigned used=0;
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
  ++stats.spans;painted_blank=0;
  {
   /* Pointer walk with the dirty/stat counters settled once per span: the
    * indexed form cost ~1000 eZ80 cycles per cell in shifts and globals. */
   const struct vios_text_cell *want=desired+index,*stop=desired+row_end;
   struct vios_text_cell *seen=painted+index;
   unsigned char *out=bytes+used,*limit=bytes+budget;
   unsigned consumed=0,clean_gap=0,cells=0;
   for(;;){
    *out++=want->ch;
    if(clean_gap)--clean_gap;else ++consumed;
    *seen++=*want++;++cells;
    if(out==limit||want==stop||want->attr!=attr)break;
    if(clean_gap)continue;
    if(want->ch==seen->ch&&want->attr==seen->attr){
     /* An unchanged gap only wins when it costs less than a new 3-byte
      * TAB. Never cross style changes or consume the final dirty glyph's
      * space: otherwise reprinting a gap could waste this entire batch. */
     const struct vios_text_cell *next=want,*next_seen=seen;unsigned gap=0;
     while(next<stop&&gap<3&&next->attr==attr&&next->ch==next_seen->ch&&next->attr==next_seen->attr){++next;++next_seen;++gap;}
     if(next==stop||gap>=3||next->attr!=attr||(next->ch==next_seen->ch&&next->attr==next_seen->attr)||(unsigned)(limit-out)<=gap)break;
     clean_gap=gap;
    }
   }
   used=(unsigned)(out-bytes);index+=cells;
   dirty_cells-=consumed;dirty_rows[row]=(unsigned char)(dirty_rows[row]-consumed);stats.cells+=cells;
  }
  cursor=index;paint_cursor=index==CELL_COUNT-1?0:index;
 }
send:
 if(used){platform_vdp_send(bytes,used);++stats.batches;stats.bytes+=used;}
 return used;
}
void vios_text_stats(struct vios_text_stats *out) {if(out)*out=stats;}

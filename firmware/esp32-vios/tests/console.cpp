/* Host test of the ESP console terminal against a fake 80x60 screen. The
 * vectors are the ones the former CPU console view was tested with
 * (tests/workbench_runtime_native.c), plus screen-level checks. */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "vios_console.h"
using namespace vios_console;

struct Screen : Renderer {
    char text[60][COLS];bool inverse[60][COLS];unsigned scrolls=0,scrolled_rows=0,clears=0,cell_writes=0;
    Screen(){wipe();}
    void wipe(){memset(text,' ',sizeof text);memset(inverse,0,sizeof inverse);}
    void cells(unsigned x,unsigned y,const char *t,unsigned n,bool inv) override{
        assert(y<60&&x+n<=COLS);for(unsigned i=0;i<n;++i){text[y][x+i]=t[i];inverse[y][x+i]=inv;}cell_writes+=n;
    }
    void scroll_up(unsigned n) override{
        assert(n>=1&&n<ROWS);++scrolls;scrolled_rows+=n;
        for(unsigned y=0;y+n<ROWS;++y){memcpy(text[y],text[y+n],COLS);memcpy(inverse[y],inverse[y+n],COLS);}
        for(unsigned y=ROWS-n;y<ROWS;++y){memset(text[y],' ',COLS);memset(inverse[y],0,COLS);}
    }
    void clear_tail() override{++clears;}
    std::string row(unsigned y) const{std::string s(text[y],COLS);s.erase(s.find_last_not_of(' ')+1);return s;}
    unsigned inverse_cells() const{unsigned n=0;for(unsigned y=0;y<ROWS;++y)for(unsigned x=0;x<COLS;++x)n+=inverse[y][x];return n;}
};
static void flush(Terminal &t){for(unsigned guard=0;t.pending()&&guard<1000;++guard)t.service(1u<<30);t.service(1u<<30);}
static void feed(Terminal &t,const char *s,uint32_t now=0){t.feed((const uint8_t*)s,strlen(s),now);flush(t);}
static std::string shadow(const Terminal &t,unsigned y){char b[COLS+1];t.row_text(y,b,sizeof b);return b;}
/* Screen and shadow agree on every console row (ignoring the cursor cell). */
static void agree(const Screen &s,const Terminal &t){for(unsigned y=0;y<ROWS;++y)assert(s.row(y)==shadow(t,y));}

int main(){
    Screen screen;Terminal t(&screen);t.show(0);flush(t);
    assert(screen.clears==1&&screen.cell_writes==ROWS*COLS+COLS+1&&screen.row(STATUS_ROW).find("VIOS console")!=std::string::npos&&screen.row(STATUS_ROW).find("F1: editor")!=std::string::npos);
    for(unsigned x=0;x<COLS;++x)assert(screen.inverse[STATUS_ROW][x]);
    // Harness framing hidden, results and prompt kept.
    feed(t,"@12345678 !ABCDEF01 1 2 + .\r\n3 \r\n\r\n@12345678\r\nok\r\nvios> ");
    assert(shadow(t,0)=="1 2 + ."&&shadow(t,1)=="3"&&shadow(t,2)==""&&shadow(t,3)=="ok"&&shadow(t,4)=="vios>");
    agree(screen,t);assert(screen.inverse[4][6]&&screen.inverse_cells()==1); // Cursor after the prompt.
    feed(t,"@0000ABCD !12345678 SEE X\r\n");assert(shadow(t,4)=="vios> SEE X"&&t.cursor_row()==5);
    // Not framing: '@' mid-line, short or non-hex nonces are shown verbatim.
    feed(t,"a@12345678 x\r\n@12G45678 y\r\n@1234 z\r\n");
    assert(shadow(t,5)=="a@12345678 x"&&shadow(t,6)=="@12G45678 y"&&shadow(t,7)=="@1234 z");agree(screen,t);
    // Scrolling: 70 lines keep the last 57 plus an empty input row; the screen scrolls in place.
    {unsigned w0=screen.cell_writes;t.reset();flush(t);assert(screen.cell_writes-w0==ROWS*COLS+COLS+1);}
    char line[16];for(int n=0;n<70;++n){snprintf(line,sizeof line,"L%02d\r\n",n);feed(t,line);}
    assert(shadow(t,56)=="L69"&&shadow(t,57)==""&&shadow(t,0)=="L13"&&screen.scrolls==13);agree(screen,t);
    // No ring to overflow: a 600-byte burst is fully drawn (the CPU view dropped 88 of these).
    std::string burst(600,'x');feed(t,burst.c_str());
    assert(shadow(t,57-7)==std::string(80,'x')&&shadow(t,57)==std::string(600%80,'x'));agree(screen,t);
    // Backspace and line editing echo.
    t.reset();flush(t);feed(t,"vios> abc\b\b  \b\bX");assert(shadow(t,0)=="vios> aX"&&t.cursor_col()==8);agree(screen,t);
    // Output while hidden updates only the shadow; show() redraws it all.
    t.reset();flush(t);feed(t,"before\r\n");unsigned writes=screen.cell_writes;t.hide();feed(t,"while-editor\r\n");
    assert(screen.cell_writes==writes&&shadow(t,1)=="while-editor");
    screen.wipe();t.show(0);flush(t);assert(screen.row(0)=="before"&&screen.row(1)=="while-editor"&&screen.row(STATUS_ROW).find("VIOS console")==1);
    // Blink: 500 ms phases, solid while output arrives, exactly one inverse cell when on.
    t.tick(100);assert(screen.inverse_cells()==1);t.tick(600);assert(screen.inverse_cells()==0);t.tick(1100);assert(screen.inverse_cells()==1);
    feed(t,"y",1400);assert(screen.inverse_cells()==1);t.tick(1800);assert(screen.inverse_cells()==1);t.tick(1950);assert(screen.inverse_cells()==0);
    // Hidden terminal never draws, even on tick.
    t.hide();writes=screen.cell_writes;t.tick(2500);t.tick(3000);feed(t,"z");assert(screen.cell_writes==writes);
    // Bounded work: show() clears but draws nothing; service(budget) draws at
    // most the budget (one span minimum), top to bottom; the cursor comes last.
    {Screen s2;Terminal u(&s2);u.show(0);flush(u);char l[64];
     for(int n=0;n<40;++n){snprintf(l,sizeof l,"row%02d %s\r\n",n,"0123456789012345678901234567890123456789");u.feed((const uint8_t*)l,strlen(l),0);}
     flush(u);u.hide();s2.wipe();unsigned before=s2.cell_writes;u.show(0);
     assert(s2.cell_writes==before&&u.pending()&&s2.inverse_cells()==0);
     unsigned steps=0;while(u.pending()){unsigned spent=u.service(400);assert(spent<=400+80);++steps;}
     assert(steps>=12&&s2.row(0).rfind("row00 ",0)==0&&s2.row(39).rfind("row39 ",0)==0);
     u.service(400);assert(s2.inverse_cells()==1);}
    // A flood record with many newlines scrolls the screen once per service,
    // by the whole amount; a whole screen of scrolling becomes one clear.
    {Screen s3;Terminal w(&s3);w.show(0);flush(w);std::string lines;char l[16];
     for(int n=0;n<130;++n){snprintf(l,sizeof l,"F%03d\r\n",n);lines+=l;}
     w.feed((const uint8_t*)lines.c_str(),lines.size(),0);assert(s3.scrolls==0);
     unsigned w0=s3.cell_writes;flush(w);assert(s3.scrolls==0&&s3.cell_writes-w0==ROWS*COLS+COLS+1);
     for(unsigned y=0;y<ROWS;++y)assert(s3.row(y)==shadow(w,y));
     std::string few="a\r\nb\r\nc\r\n";w.feed((const uint8_t*)few.c_str(),few.size(),0);flush(w);
     assert(s3.scrolls==1&&s3.scrolled_rows==3);for(unsigned y=0;y<ROWS;++y)assert(s3.row(y)==shadow(w,y));}
    // Echo-sized changes are "small"; scrolls, status and big spans are not.
    {Screen s4;Terminal v(&s4);v.show(0);flush(v);assert(!v.small_pending(160));
     v.feed((const uint8_t*)"x",1,0);assert(v.small_pending(160));flush(v);
     v.feed((const uint8_t*)"\r\n",2,0);flush(v);
     std::string big(200,'y');v.feed((const uint8_t*)big.c_str(),big.size(),0);assert(!v.small_pending(160));flush(v);}
    puts("console terminal checks passed");
    return 0;
}

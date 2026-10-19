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
    char text[60][COLS];bool inverse[60][COLS];unsigned scrolls=0,clears=0,cell_writes=0;
    Screen(){wipe();}
    void wipe(){memset(text,' ',sizeof text);memset(inverse,0,sizeof inverse);}
    void cells(unsigned x,unsigned y,const char *t,unsigned n,bool inv) override{
        assert(y<60&&x+n<=COLS);for(unsigned i=0;i<n;++i){text[y][x+i]=t[i];inverse[y][x+i]=inv;}cell_writes+=n;
    }
    void scroll_up() override{
        ++scrolls;for(unsigned y=0;y+1<ROWS;++y){memcpy(text[y],text[y+1],COLS);memcpy(inverse[y],inverse[y+1],COLS);}
        memset(text[ROWS-1],' ',COLS);memset(inverse[ROWS-1],0,COLS);
    }
    void clear_screen() override{++clears;wipe();}
    std::string row(unsigned y) const{std::string s(text[y],COLS);s.erase(s.find_last_not_of(' ')+1);return s;}
    unsigned inverse_cells() const{unsigned n=0;for(unsigned y=0;y<ROWS;++y)for(unsigned x=0;x<COLS;++x)n+=inverse[y][x];return n;}
};
static void feed(Terminal &t,const char *s,uint32_t now=0){t.feed((const uint8_t*)s,strlen(s),now);}
static std::string shadow(const Terminal &t,unsigned y){char b[COLS+1];t.row_text(y,b,sizeof b);return b;}
/* Screen and shadow agree on every console row (ignoring the cursor cell). */
static void agree(const Screen &s,const Terminal &t){for(unsigned y=0;y<ROWS;++y)assert(s.row(y)==shadow(t,y));}

int main(){
    Screen screen;Terminal t(&screen);t.show(0);
    assert(screen.clears==1&&screen.row(STATUS_ROW).find("VIOS console")!=std::string::npos&&screen.row(STATUS_ROW).find("F1: editor")!=std::string::npos);
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
    t.reset();assert(screen.clears==2);
    char line[16];for(int n=0;n<70;++n){snprintf(line,sizeof line,"L%02d\r\n",n);feed(t,line);}
    assert(shadow(t,56)=="L69"&&shadow(t,57)==""&&shadow(t,0)=="L13"&&screen.scrolls==13);agree(screen,t);
    // No ring to overflow: a 600-byte burst is fully drawn (the CPU view dropped 88 of these).
    std::string burst(600,'x');feed(t,burst.c_str());
    assert(shadow(t,57-7)==std::string(80,'x')&&shadow(t,57)==std::string(600%80,'x'));agree(screen,t);
    // Backspace and line editing echo.
    t.reset();feed(t,"vios> abc\b\b  \b\bX");assert(shadow(t,0)=="vios> aX"&&t.cursor_col()==8);agree(screen,t);
    // Output while hidden updates only the shadow; show() redraws it all.
    t.reset();feed(t,"before\r\n");unsigned writes=screen.cell_writes;t.hide();feed(t,"while-editor\r\n");
    assert(screen.cell_writes==writes&&shadow(t,1)=="while-editor");
    screen.wipe();t.show(0);assert(screen.row(0)=="before"&&screen.row(1)=="while-editor"&&screen.row(STATUS_ROW).find("VIOS console")==1);
    // Blink: 500 ms phases, solid while output arrives, exactly one inverse cell when on.
    t.tick(100);assert(screen.inverse_cells()==1);t.tick(600);assert(screen.inverse_cells()==0);t.tick(1100);assert(screen.inverse_cells()==1);
    feed(t,"y",1400);assert(screen.inverse_cells()==1);t.tick(1800);assert(screen.inverse_cells()==1);t.tick(1950);assert(screen.inverse_cells()==0);
    // Hidden terminal never draws, even on tick.
    t.hide();writes=screen.cell_writes;t.tick(2500);t.tick(3000);feed(t,"z");assert(screen.cell_writes==writes);
    puts("console terminal checks passed");
    return 0;
}

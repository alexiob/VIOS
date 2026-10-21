/* Host test of the ESP cooked line editor (GUI Stage 2a). The echo is rendered
 * as a terminal would (BS moves left, CR/LF start a new row, other bytes
 * overwrite), and the expectations follow the former CPU editor's tests. */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
extern "C" {
#include "platform/input.h"
}
#include "vios_line.h"
using namespace vios_line;

struct Term : Sink {
    std::vector<std::string> rows{std::string()};unsigned col=0;std::vector<std::string> lines;bool room=true;size_t echoed=0;
    void echo(const uint8_t *b,size_t n) override{
        echoed+=n;
        for(size_t i=0;i<n;++i){uint8_t c=b[i];std::string &r=rows.back();
            if(c==8){assert(col);--col;}
            else if(c=='\r')col=0;
            else if(c=='\n'){rows.emplace_back();col=0;}
            else{if(r.size()<=col)r.resize(col+1,' ');r[col++]=(char)c;}}
    }
    bool submit(const uint8_t *l,size_t n) override{if(!room)return false;lines.emplace_back((const char*)l,n);return true;}
    std::string row() const{std::string r=rows.back();r.erase(r.find_last_not_of(' ')+1);return r;}
};
static void type(Field &f,const char *s){for(;*s;++s)assert(f.key((uint8_t)*s,0));}

int main(){
    // Virtual key values match the CPU's platform header.
    static_assert((int)K_ESCAPE==(int)PLATFORM_KEY_ESCAPE&&(int)K_DELETE==(int)PLATFORM_KEY_DELETE&&(int)K_BACKSPACE==(int)PLATFORM_KEY_BACKSPACE,"keys");
    static_assert((int)K_HOME==(int)PLATFORM_KEY_HOME&&(int)K_KP_HOME==(int)PLATFORM_KEY_KP_HOME&&(int)K_END==(int)PLATFORM_KEY_END&&(int)K_KP_END==(int)PLATFORM_KEY_KP_END,"keys");
    static_assert((int)K_RETURN==(int)PLATFORM_KEY_RETURN&&(int)K_KP_ENTER==(int)PLATFORM_KEY_KP_ENTER,"keys");
    static_assert((int)K_UP==(int)PLATFORM_KEY_UP&&(int)K_KP_UP==(int)PLATFORM_KEY_KP_UP&&(int)K_DOWN==(int)PLATFORM_KEY_DOWN&&(int)K_KP_DOWN==(int)PLATFORM_KEY_KP_DOWN,"keys");
    static_assert((int)K_LEFT==(int)PLATFORM_KEY_LEFT&&(int)K_KP_LEFT==(int)PLATFORM_KEY_KP_LEFT&&(int)K_RIGHT==(int)PLATFORM_KEY_RIGHT&&(int)K_KP_RIGHT==(int)PLATFORM_KEY_KP_RIGHT,"keys");

    Term t;Field f(&t);
    // Typing, Backspace, mid-line insert and Delete; the echo always equals the line.
    type(f,"abc");assert(t.row()=="abc"&&t.col==3);
    assert(f.key(8,PLATFORM_KEY_BACKSPACE));assert(t.row()=="ab"&&t.col==2);
    assert(f.key(0,PLATFORM_KEY_LEFT));assert(f.key('X',0));assert(t.row()=="aXb"&&t.col==2);
    assert(f.key(0,PLATFORM_KEY_DELETE));assert(t.row()=="aX"&&t.col==2);
    assert(f.key(0,PLATFORM_KEY_HOME)&&t.col==0&&f.key(0,PLATFORM_KEY_END)&&t.col==2);
    // Enter erases the shown line and hands it to the CPU with CR.
    assert(f.key('\r',PLATFORM_KEY_RETURN));assert(t.lines.size()==1&&t.lines[0]=="aX\r"&&t.row()==""&&t.col==0&&!f.busy());
    // History: Up recalls, Down returns to an empty line; duplicates are not stored twice.
    type(f,"second");f.key('\r',PLATFORM_KEY_RETURN);type(f,"second");f.key('\r',PLATFORM_KEY_RETURN);
    assert(f.key(0,PLATFORM_KEY_UP)&&t.row()=="second");assert(f.key(0,PLATFORM_KEY_UP)&&t.row()=="aX");
    assert(f.key(0,PLATFORM_KEY_UP)&&t.row()=="aX"); // oldest stays
    assert(f.key(0,PLATFORM_KEY_DOWN)&&t.row()=="second");assert(f.key(0,PLATFORM_KEY_DOWN)&&t.row()=="");
    // The VDP's ASCII for the arrows (Up 11, Down 10, Left 8, Right 21): Down is
    // not Enter and Left is not Backspace; the key code decides.
    {size_t n=t.lines.size();assert(f.key(11,PLATFORM_KEY_UP)&&t.row()=="second");
     assert(f.key(10,PLATFORM_KEY_DOWN)&&t.row()==""&&t.lines.size()==n&&!f.busy());
     assert(f.key(10,PLATFORM_KEY_DOWN)&&t.row()==""&&t.lines.size()==n);   // past the newest: nothing
     type(f,"ab");assert(f.key(8,PLATFORM_KEY_LEFT)&&t.row()=="ab"&&t.col==1);
     assert(f.key(21,PLATFORM_KEY_RIGHT)&&t.col==2);f.key(27,PLATFORM_KEY_ESCAPE);}
    // Esc clears the line.
    type(f,"junk");assert(f.key(27,PLATFORM_KEY_ESCAPE)&&t.row()==""&&f.length()==0);
    // Non-editing keys pass through to the CPU (F1 view toggle, function keys).
    assert(!f.key(0,PLATFORM_KEY_F1)&&!f.key(0,PLATFORM_KEY_F5));
    // No room for the line: it stays pending, edits are dropped, poll() retries.
    t.room=false;type(f,"wait");f.key('\r',PLATFORM_KEY_RETURN);assert(f.busy()&&t.lines.size()==3);
    assert(f.key('z',0)&&f.dropped_keys()==1);t.room=true;f.poll();assert(!f.busy()&&t.lines.back()=="wait\r");
    // Output from elsewhere while a line is shown: the next edit redisplays it on a fresh row.
    type(f,"ab");size_t rows=t.rows.size();t.echo((const uint8_t*)"tick",4);f.foreign_output();
    assert(f.key('c',0));assert(t.rows.size()==rows+1&&t.row()=="abc");
    f.key('\r',PLATFORM_KEY_RETURN);assert(t.lines.back()=="abc\r");
    // A full line drops further characters instead of overflowing.
    for(unsigned i=0;i<LENGTH;++i)f.key('x',0);unsigned before=f.dropped_keys();f.key('y',0);
    assert(f.length()==LENGTH&&f.dropped_keys()==before+1);
    puts("line editor checks passed");
    return 0;
}

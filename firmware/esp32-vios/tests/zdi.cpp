/* Exercise the actual GPIO bit transport, decoding instruction-store writes. */
#include <cassert>
#include <cstdint>
#include <vector>
#define REG_PC 7
#define REG_AF 0
static unsigned critical_depth;
static bool interrupts_enabled=true,previous_interrupts;
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
static void enter(int*) {assert(!critical_depth++);previous_interrupts=interrupts_enabled;interrupts_enabled=false;}
static void leave(int*) {assert(critical_depth--==1);interrupts_enabled=previous_interrupts;}
#define portENTER_CRITICAL enter
#define portEXIT_CRITICAL leave
static std::vector<std::vector<unsigned>> transactions;
static bool clock_level,data_level;
static unsigned read_bit;
static std::vector<uint8_t> read_script{0x98};
struct MMIO {
    unsigned address;
    void operator=(uint32_t bits) {
        assert(critical_depth==1&&!interrupts_enabled);
        if(address==8) {
            if(bits&(1u<<27))data_level=true;
            if(bits&(1u<<26)) {
                if(!clock_level&&!transactions.empty())transactions.back().push_back(data_level);
                clock_level=true;
            }
        } else if(address==12) {
            if(bits&(1u<<26))clock_level=false;
            if(bits&(1u<<27)) {
                if(clock_level&&data_level)transactions.emplace_back();
                data_level=false;
            }
        }
    }
    operator uint32_t() const {
        assert(address==0x3C&&critical_depth==1&&!interrupts_enabled);
        assert(read_bit<read_script.size()*8);
        unsigned index=read_bit++;return ((read_script[index/8]>>(7-index%8))&1u)<<27;
    }
};
#define VIOS_GPIO_REG(a) MMIO{a}
static void delayMicroseconds(unsigned delay) {
    if(critical_depth)assert(critical_depth==1&&!interrupts_enabled);
    else assert(delay==10); // Boot IN0 allows CPU execution after the protected transfer.
}
#include "vios_zdi.h"
static unsigned byte(const std::vector<unsigned>& bits,size_t at) {
    unsigned value=0;for(unsigned i=0;i<8;++i)value=(value<<1)|bits[at+i];return value;
}
static void check(size_t index,unsigned reg,std::vector<unsigned> bytes) {
    auto &bits=transactions[index];assert(bits.size()==8+bytes.size()*9+1);
    assert(byte(bits,0)==reg*2);size_t at=8;
    for(unsigned value:bytes){assert(bits[at++]==0);assert(byte(bits,at)==value);at+=8;}
    assert(bits[at]==1);
}
int main() {
    vios_zdi_out0(0xA8,0x04);
    assert(transactions.size()==7);
    check(0,0x25,{0});check(1,0x24,{0x04});check(2,0x25,{0x3E});
    check(3,0x25,{0});check(4,0x23,{0xA8});check(5,0x24,{0x39});check(6,0x25,{0xED});
    assert(!critical_depth&&interrupts_enabled);
    vios_zdi_di();check(7,0x25,{0xF3});
    interrupts_enabled=false;vios_zdi_out0(0xAA,0x08);
    assert(!interrupts_enabled&&!critical_depth);check(8,0x25,{0});check(9,0x24,{0x08});check(10,0x25,{0x3E});
    check(11,0x25,{0});check(12,0x23,{0xAA});check(13,0x24,{0x39});check(14,0x25,{0xED});
    assert(vios_zdi_status()==0x98&&read_bit==8&&!interrupts_enabled&&!critical_depth);
    // Production discovery deliberately retains the original pure IN0 burst.
    size_t next=transactions.size();read_script={0xB7,0,0};read_bit=0;
    assert(vios_zdi_boot_in0(0xB5)==0xB7);check(next,0x23,{0xB5,0x38,0xED});
    check(next+1,0x16,{0});assert(byte(transactions[next+2],0)==0x21);
    assert(read_bit==24&&!interrupts_enabled&&!critical_depth);
}

/* RAM-only eZ80 ZDI readout through AgonLight2 ESP32 GPIO26/27.
 * No flash erase/program command is implemented. RAM writes and the UART bridge
 * are included only in the separately built RAM_LOADER variant.
 * ZDI bit protocol adapted from AgonPlatform/agon-recovery (MIT):
 * Copyright (c) 2024 Jeroen Venema. See NOTICE.txt in this directory.
 * MMIO and ROM symbols: Espressif ESP-IDF ESP32 headers/ROM linker map.
 */
#include <stdint.h>
#define REG(a) (*(volatile uint32_t *)(a))
#define U0 0x3ff40000u
#define GPIO 0x3ff44000u
#define CLK (1u<<26)
#define DAT (1u<<27)
extern uint32_t _bss_start, _bss_end;
extern void ets_set_user_start(uint32_t);
extern void ets_delay_us(uint32_t);
extern void gpio_pad_select_gpio(uint32_t);
extern void gpio_matrix_out(uint32_t,uint32_t,uint32_t,uint32_t);
static uint32_t bit_delay;
static uint32_t flash_base;
static unsigned flash_ready;
static void put(unsigned char c) {
    while (((REG(U0+0x1c)>>16)&255)>=120) {}
    REG(U0)=c;
}
static void text(const char *p) { while(*p) put((unsigned char)*p++); }
static void hex(uint32_t n, unsigned digits) {
    static const char h[]="0123456789ABCDEF";
    while(digits--) put(h[(n>>(digits*4))&15]);
}
static void high(uint32_t bits) { REG(GPIO+8)=bits; }
static void low(uint32_t bits) { REG(GPIO+12)=bits; }
static void output(uint32_t bits) { REG(GPIO+0x24)=bits; }
static void input(uint32_t bits) { REG(GPIO+0x28)=bits; }
static void bit_write(unsigned bit) {
    low(CLK);
    if(bit) high(DAT); else low(DAT);
    output(DAT); ets_delay_us(bit_delay);
    high(CLK); ets_delay_us(bit_delay);
}
static unsigned bit_read(void) {
    low(CLK); input(DAT); ets_delay_us(bit_delay);
    unsigned b=(REG(GPIO+0x3c)>>27)&1;
    high(CLK); ets_delay_us(bit_delay);
    return b;
}
static void start(unsigned reg, unsigned read) {
    high(DAT); output(DAT); high(CLK); ets_delay_us(2);
    low(DAT); ets_delay_us(2);
    for(int i=6;i>=0;--i) bit_write(reg&(1u<<i));
    bit_write(read);
}
static void done(void) { bit_write(1); ets_delay_us(3); }
static void read_regs(unsigned reg, unsigned count, unsigned char *p) {
    output(CLK);
    start(reg,1);
    while(count--) {
        bit_write(0); ets_delay_us(3);
        unsigned b=0;
        for(unsigned i=0;i<8;++i) b=(b<<1)|bit_read();
        *p++=(unsigned char)b;
    }
    done();
}
static void write_regs(unsigned reg, unsigned count, const unsigned char *p) {
    output(CLK); start(reg,0);
    while(count--) {
        bit_write(0); ets_delay_us(3);
        unsigned b=*p++;
        for(int i=7;i>=0;--i) bit_write(b&(1u<<i));
    }
    done();
}
static void wr(unsigned reg, unsigned char value) { write_regs(reg,1,&value); }
static unsigned status(void) { unsigned char s;read_regs(3,1,&s);return s; }
static uint32_t cpu_read(unsigned r) {
    unsigned char p[3];wr(0x16,(unsigned char)r);read_regs(0x10,3,p);
    return p[0]|(p[1]<<8)|(p[2]<<16);
}
static void cpu_write(unsigned r, uint32_t n) {
    unsigned char p[3]={(unsigned char)n,(unsigned char)(n>>8),(unsigned char)(n>>16)};
    write_regs(0x13,3,p);wr(0x16,(unsigned char)(r|0x80));
}
static int stopped(void) {
    if((status()&0x90)!=0x90) {text("ERR require halted ADL CPU\r\n");return 0;}
    return 1;
}
static void snapshot(void) {
    if(!stopped()) return;
    text("REGS ");hex(status(),2);
    for(unsigned r=0;r<8;++r) {put(' ');hex(cpu_read(r),6);}
    text("\r\n");
}
static void halt(void) {
    unsigned char id[2];read_regs(0,2,id);
    if(id[0]!=7||id[1]!=0) {text("ERR CPU identity\r\n");return;}
    wr(0x10,0x80);ets_delay_us(100);
    snapshot();
}
static unsigned io_read(unsigned char port) {
    uint32_t af=cpu_read(0),pc=cpu_read(7);
    /* IN0 A,(port) = ED 38 nn, loaded backwards into IS2..IS0. */
    unsigned char op[3]={port,0x38,0xed};
    write_regs(0x23,3,op);ets_delay_us(10);
    unsigned value=cpu_read(0)&255;
    cpu_write(0,af);cpu_write(7,pc);
    return value;
}
#ifdef INSTRUCTION_PROBEONLY
/* Compare the original burst loader with the Arduino split-register diagnostic.
 * No OUT/OUT0, memory write, reset or flash-program instruction is injected.
 */
static void instruction_probe(unsigned split) {
    if(!stopped()) return;
    uint32_t saved[8];
    for(unsigned r=0;r<8;++r) saved[r]=cpu_read(r);
    unsigned char load[2]={0x42,0x3e},in[3]={0xa8,0x38,0xed};
    if(split) {wr(0x24,load[0]);wr(0x25,load[1]);}
    else write_regs(0x24,2,load);
    ets_delay_us(10);uint32_t loaded=cpu_read(0);
    if(split) {wr(0x23,in[0]);wr(0x24,in[1]);wr(0x25,in[2]);}
    else write_regs(0x23,3,in);
    ets_delay_us(10);uint32_t value=cpu_read(0);
    for(unsigned r=0;r<8;++r) cpu_write(r,saved[r]);
    text(split?"INJECT SPLIT LD ":"INJECT LD ");hex(loaded,6);
    text(" IN ");hex(value,6);text("\r\n");
}
static void instruction_neutral_probe(unsigned clear_after) {
    if(!stopped()) return;
    uint32_t saved[8];
    for(unsigned r=0;r<8;++r) saved[r]=cpu_read(r);
    wr(0x25,0x00);ets_delay_us(10);uint32_t nop=cpu_read(0);
    wr(0x24,0x42);uint32_t pre=cpu_read(0);
    wr(0x25,0x3e);ets_delay_us(10);
    if(clear_after) {wr(0x25,0x00);ets_delay_us(10);}
    uint32_t loaded=cpu_read(0);
    wr(0x25,0x00);ets_delay_us(10);uint32_t in_nop=cpu_read(0);
    wr(0x23,0xa8);wr(0x24,0x38);uint32_t in_pre=cpu_read(0);
    wr(0x25,0xed);ets_delay_us(10);
    if(clear_after) {wr(0x25,0x00);ets_delay_us(10);}
    uint32_t value=cpu_read(0);
    /* Do not let register-access operations resurrect a stale instruction. */
    wr(0x25,0x00);ets_delay_us(10);
    for(unsigned r=0;r<8;++r) cpu_write(r,saved[r]);
    text(clear_after?"INJECT POSTNOP NOP ":"INJECT NEUTRAL NOP ");hex(nop,6);
    text(" PRE ");hex(pre,6);text(" LD ");hex(loaded,6);
    text(" INNOP ");hex(in_nop,6);text(" INPRE ");hex(in_pre,6);
    text(" IN ");hex(value,6);text("\r\n");
}
static unsigned wait_halted(void) {
    for(unsigned poll=0;poll<512;++poll) {
        if(status()&0x80) return 1;
        ets_delay_us(100);
    }
    return 0;
}
static void instruction_read_sequence(void) {
    if(!stopped()) return;
    uint32_t saved[8],raw[7];unsigned wrapped[3];
    for(unsigned r=0;r<8;++r) saved[r]=cpu_read(r);
    wr(0x25,0xf3);ets_delay_us(10);
    for(unsigned i=0;i<7;++i) {
        unsigned char in[3]={(unsigned char)(i<4?0xb5:0xb4),0x38,0xed};
        write_regs(0x23,3,in);ets_delay_us(10);raw[i]=cpu_read(0);
    }
    wr(0x25,0xf3);ets_delay_us(10);
    for(unsigned i=0;i<3;++i) wrapped[i]=io_read(0xb5);
    wr(0x25,0x00);ets_delay_us(10);
    for(unsigned r=0;r<8;++r) cpu_write(r,saved[r]);
    text("Q RAW B5");for(unsigned i=0;i<4;++i) {put(' ');hex(raw[i],6);}
    text(" B4");for(unsigned i=4;i<7;++i) {put(' ');hex(raw[i],6);}
    text("\r\nQ WRAPPED B5");
    for(unsigned i=0;i<3;++i) {put(' ');hex(wrapped[i],2);}
    text("\r\n");
}
static void reset_ram_probe(void) {
    /* PS0153: F92 on-chip SRAM resets enabled at FFE000..FFFFFF.
     * This path uses only ZDI register/memory operations, never IS0..IS4.
     */
    wr(0x10,0x80);
    if(!wait_halted()) {text("RESET ERR pre-halt timeout\r\n");return;}
    unsigned before=status();uint32_t oldpc=cpu_read(7);
    wr(0x11,0x80);wr(0x10,0x80);
    unsigned halted=wait_halted(),after=status();
    text("RESET BEFORE STAT ");hex(before,2);text(" PC ");hex(oldpc,6);
    text(" AFTER STAT ");hex(after,2);
    if(!halted) {text(" ERR halt timeout\r\n");return;}
    uint32_t registers[8];
    for(unsigned r=0;r<8;++r) registers[r]=cpu_read(r);
    text(" PC ");hex(registers[7],6);text("\r\n");
    unsigned char id[4];read_regs(0,4,id);
    text("ID ");hex(id[0]|((uint32_t)id[1]<<8),4);
    text(" REV ");hex(id[2],2);text(" STAT ");hex(id[3],2);text("\r\n");
    text("RESET REGS ");hex(after,2);
    for(unsigned r=0;r<8;++r) {put(' ');hex(registers[r],6);}
    text("\r\n");
    if(id[0]!=7||id[1]!=0) {text("RESET ERR CPU identity; no SRAM write\r\n");return;}
    if(!(status()&0x10)) {wr(0x16,8);ets_delay_us(10);}
    if((status()&0x90)!=0x90) {text("RESET ERR require halted ADL; no SRAM write\r\n");return;}
    unsigned char original[64],pattern[64],verify[64];
    uint32_t pc=cpu_read(7);
    cpu_write(7,0xffe000);read_regs(0x20,sizeof original,original);
    text("SRAM ORIGINAL FFE000 ");
    for(unsigned i=0;i<sizeof original;++i) {hex(original[i],2);pattern[i]=(unsigned char)(0xa5^(i*37));}
    text("\r\n");
    cpu_write(7,0xffe000);write_regs(0x30,sizeof pattern,pattern);
    cpu_write(7,0xffe000);read_regs(0x20,sizeof verify,verify);
    unsigned pattern_ok=1,restore_ok=1;
    for(unsigned i=0;i<sizeof pattern;++i) if(verify[i]!=pattern[i]) pattern_ok=0;
    /* Always restore exactly the captured bytes, even after pattern failure. */
    cpu_write(7,0xffe000);write_regs(0x30,sizeof original,original);
    cpu_write(7,0xffe000);read_regs(0x20,sizeof verify,verify);
    for(unsigned i=0;i<sizeof original;++i) if(verify[i]!=original[i]) restore_ok=0;
    cpu_write(7,pc);
    text(pattern_ok?"SRAM PATTERN OK RESTORE ":"SRAM PATTERN FAIL RESTORE ");
    text(restore_ok?"OK":"FAIL");text(" STAT ");hex(status(),2);
    text(" PC ");hex(cpu_read(7),6);text("\r\n");
}
#endif
static void flash_info(void) {
    flash_ready=0;
    if(!stopped()) return;
    unsigned a=io_read(0xf7),c=io_read(0xf8);
    unsigned rb=io_read(0xb5),rc=io_read(0xb4);
    text("MAP FLASH ");hex(a,2);put(' ');hex(c,2);
    text(" RAM ");hex(rb,2);put(' ');hex(rc,2);text("\r\n");
    if(!(c&8)) {text("ERR flash disabled\r\n");return;}
    flash_base=(a&0xfe)<<16;
    /* Do not mistake overlapping on-chip SRAM for flash. */
    if((rc&0x80)&&(((rb<<16)&0xfe0000)==flash_base)) {
        text("ERR SRAM overlaps flash\r\n");return;
    }
    flash_ready=1;
}
static int hex_input(unsigned count,uint32_t *value) {
    *value=0;
    while(count--) {
        while(!(REG(U0+0x1c)&255)) {}
        unsigned char c=(unsigned char)REG(U0);
        unsigned d;
        if(c>='0'&&c<='9') d=c-'0';
        else if(c>='A'&&c<='F') d=c-'A'+10;
        else return 0;
        *value=(*value<<4)|d;
    }
    return 1;
}
#ifdef RAM_LOADER
static int ram_range(uint32_t address,uint32_t count) {
    return count&&count<=256&&address>=0x40000&&address<=0xaf000-count;
}
#endif
static void memory_read(unsigned ram) {
    uint32_t address,count;
    if(!hex_input(6,&address)||!hex_input(4,&count)) {text("ERR syntax\r\n");return;}
    if(ram) {
        if(!count||count>256||address<0x40000||address>0xc0000-count) {
            text("ERR RAM range\r\n");return;
        }
    } else if(!flash_ready||!count||count>256||address<flash_base||
       address-flash_base>0x20000-count) {text("ERR flash range\r\n");return;}
    if(!stopped()) return;
    unsigned char data[256];
    uint32_t pc=cpu_read(7);cpu_write(7,address);
    read_regs(0x20,count,data);cpu_write(7,pc);
    uint32_t crc=0xffffffffu;
    for(unsigned i=0;i<count;++i) {
        crc^=data[i];
        for(unsigned b=0;b<8;++b) crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    text("MEM ");hex(address,6);put(' ');hex(count,4);put(' ');hex(~crc,8);put(' ');
    for(unsigned i=0;i<count;++i) hex(data[i],2);
    text("\r\n");
}
#ifdef RAM_LOADER
static unsigned ram_ready;
static void ram_map(void) {
    ram_ready=0;
    if(!stopped()) return;
    /* DI before any instruction injection: no interrupts during the RAM run. */
    wr(0x25,0xf3);ets_delay_us(10);
    unsigned l=io_read(0xa8),u=io_read(0xa9),c=io_read(0xaa),m=io_read(0xf0);
    text("CS0 ");hex(l,2);put(' ');hex(u,2);put(' ');hex(c,2);put(' ');hex(m,2);text("\r\n");
    /* Older MOS may decode external RAM below040000 as well. Require the
     * complete payload/stack region to be covered, not an exact lower bound. */
    ram_ready=l<=4&&u>=10&&(c&0xf)==8&&(m&3)==1;
    if(!ram_ready) text("ERR unsupported RAM mapping\r\n");
}
static void memory_write(void) {
    uint32_t address,count,want;
    if(!hex_input(6,&address)||!hex_input(4,&count)||!hex_input(8,&want)) {
        text("ERR syntax\r\n");return;
    }
    if(!ram_ready||!ram_range(address,count)) {text("ERR RAM range\r\n");return;}
    unsigned char data[256];uint32_t crc=0xffffffffu,n;
    for(unsigned i=0;i<count;++i) {
        if(!hex_input(2,&n)) {text("ERR syntax\r\n");return;}
        data[i]=(unsigned char)n;crc^=n;
        for(unsigned b=0;b<8;++b) crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    if(~crc!=want) {text("ERR CRC\r\n");return;}
    if(!stopped()) return;
    uint32_t pc=cpu_read(7);cpu_write(7,address);
    write_regs(0x30,count,data);cpu_write(7,pc);
    text("WROTE ");hex(address,6);put(' ');hex(count,4);put(' ');hex(~crc,8);text("\r\n");
}
static void bridge(unsigned restart) {
    if(!ram_ready||!stopped()) return;
    unsigned char header[5];uint32_t pc=cpu_read(7);
    cpu_write(7,0x40040);read_regs(0x20,5,header);cpu_write(7,pc);
    if(header[0]!='M'||header[1]!='O'||header[2]!='S'||header[3]!=0||header[4]!=1) {
        text("ERR RAM image header\r\n");return;
    }
    const uint32_t u2=0x3ff6e000u;
    REG(0x3ff000c0)|=1u<<23;
    REG(0x3ff000c4)&=~(1u<<23);
    gpio_pad_select_gpio(2);gpio_pad_select_gpio(13);gpio_pad_select_gpio(34);
    REG(0x3ff49014)|=1u<<9;
    gpio_matrix_out(2,198,0,0);
    extern void gpio_matrix_in(uint32_t,uint32_t,uint32_t);
    gpio_matrix_in(34,198,0);
    gpio_matrix_out(13,256,0,0);low(1u<<13);output((1u<<13)|(1u<<2));
    /* Both UARTs share APB/reference clocks: copy calibrated USB baud divider. */
    REG(u2+0x14)=REG(U0+0x14);
    uint32_t conf=(REG(U0+0x20)&(1u<<27))|0x1c;
    /* Preserve UART FIFO state when resuming an existing program. Resetting
     * it between debug operations can discard pending console bytes. */
    if(restart) {REG(u2+0xc)=0;REG(u2+0x20)=conf|(3u<<17);REG(u2+0x20)=conf;}
    if(restart) {
        text("GO 040000 USB/UART bridge 115200\r\n");
        cpu_write(7,0x40000);
    } else text("RESUME USB/UART bridge 115200\r\n");
    wr(0x10,0);
    input(CLK|DAT);
    unsigned escape=0;
    for(;;) {
        if(REG(u2+0x1c)&255) put((unsigned char)REG(u2));
        if((REG(U0+0x1c)&255)&&((REG(u2+0x1c)>>16)&255)<120) {
            unsigned char c=(unsigned char)REG(U0);
            if(c==29) {
                if(++escape==3) {
                    output(CLK);high(CLK);wr(0x10,0x80);
                    text("\r\nDEBUG\r\n");return;
                }
            } else {
                while(escape) {REG(u2)=29;--escape;}
                REG(u2)=c;
            }
        }
    }
}
#endif
static void pin_test(void) {
    text("PINTEST driven:sampled ");
    for(unsigned pattern=0;pattern<4;++pattern) {
        input(CLK|DAT);
        low(CLK|DAT); high(pattern<<26); output(CLK|DAT);
        ets_delay_us(10);
        unsigned got=(REG(GPIO+0x3c)>>26)&3;
        input(CLK|DAT);
        hex(pattern,1);put(':');hex(got,1);put(' ');
        if(got!=pattern) {text("MISMATCH ");break;}
    }
    ets_delay_us(1000);
    text(" released:");hex((REG(GPIO+0x3c)>>26)&3,1);text("\r\n");
}
static void passive(void) {
    input(CLK|DAT);
    ets_delay_us(1000);
    text("INPUT26/27 ");hex((REG(GPIO+0x3c)>>26)&3,1);text("\r\n");
}
#ifndef INSTRUCTION_PROBEONLY
static void levels(void) {
    text("GPIO ");hex(REG(GPIO+0x3c),8);
    text(" MUX26 ");hex(REG(0x3ff49028),8);
    text(" MUX27 ");hex(REG(0x3ff4902c),8);
    text(" OUT26 ");hex(REG(GPIO+0x598),8);
    text(" OUT27 ");hex(REG(GPIO+0x59c),8);text("\r\n");
    low(CLK|DAT);output(CLK|DAT);ets_delay_us(10);
    text("DRIVE00 ");hex((REG(GPIO+0x3c)>>26)&3,1);
    high(CLK|DAT);ets_delay_us(10);
    text(" DRIVE11 ");hex((REG(GPIO+0x3c)>>26)&3,1);text("\r\n");
    input(DAT);
}
#endif
static void identify(void) {
    unsigned char p[4];
    read_regs(0,4,p);
    text("ID "); hex(p[0]|(p[1]<<8),4);
    text(" REV ");hex(p[2],2);text(" STAT ");hex(p[3],2);text("\r\n");
}
void __attribute__((used)) stub_main(void) {
    uint32_t ps;
    __asm__ volatile("rsil %0,15":"=a"(ps)); (void)ps;
    ets_set_user_start(0);
    for(uint32_t *p=&_bss_start;p<&_bss_end;++p) *p=0;
    bit_delay=1;
    gpio_pad_select_gpio(26); gpio_pad_select_gpio(27);
    gpio_matrix_out(26,256,0,0); gpio_matrix_out(27,256,0,0);
    REG(0x3ff49028)|=1u<<9; /* GPIO26 input enable */
    REG(0x3ff4902c)|=1u<<9; /* GPIO27 input enable */
    high(CLK|DAT); output(CLK); input(DAT);
    text("\r\nagon-zdi-probe RAM only; I=identify\r\n");
#ifdef INSTRUCTION_PROBEONLY
    text("instruction probe; J=burst K=split L=neutral M=postnop Q=IN-sequence R=reset+SRAM\r\n");
#endif
    for(;;) {
        if(REG(U0+0x1c)&255) {
            unsigned char c=(unsigned char)REG(U0);
            if(c=='I') identify();
#ifndef INSTRUCTION_PROBEONLY
            if(c=='L') levels();
#endif
            if(c=='P') passive();
            if(c=='T') pin_test();
            if(c=='1') bit_delay=1;
            if(c=='2') bit_delay=10;
            if(c=='3') bit_delay=50;
            if(c=='H') halt();
            if(c=='S') snapshot();
#ifdef INSTRUCTION_PROBEONLY
            if(c=='J') instruction_probe(0);
            if(c=='K') instruction_probe(1);
            if(c=='L') instruction_neutral_probe(0);
            if(c=='M') instruction_neutral_probe(1);
            if(c=='R') reset_ram_probe();
            if(c=='Q') instruction_read_sequence();
#endif
            if(c=='F') flash_info();
#ifndef INSTRUCTION_PROBEONLY
            if(c=='R') memory_read(0);
#endif
            if(c=='G') {wr(0x10,0);text("RUN\r\n");}
#ifdef RAM_LOADER
            if(c=='D') ram_map();
            if(c=='W') memory_write();
            if(c=='V') memory_read(1);
            if(c=='X') bridge(1);
            if(c=='Y') bridge(0);
#endif
        }
    }
}

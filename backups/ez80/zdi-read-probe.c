/* RAM-only eZ80 ZDI readout through AgonLight2 ESP32 GPIO26/27.
 * No flash erase/program or memory-write command is implemented.
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
static void memory_read(void) {
    uint32_t address,count;
    if(!hex_input(6,&address)||!hex_input(4,&count)) {text("ERR syntax\r\n");return;}
    if(!flash_ready||!count||count>256||address<flash_base||
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
    for(;;) {
        if(REG(U0+0x1c)&255) {
            unsigned char c=(unsigned char)REG(U0);
            if(c=='I') identify();
            if(c=='L') levels();
            if(c=='P') passive();
            if(c=='T') pin_test();
            if(c=='1') bit_delay=1;
            if(c=='2') bit_delay=10;
            if(c=='3') bit_delay=50;
            if(c=='H') halt();
            if(c=='S') snapshot();
            if(c=='F') flash_info();
            if(c=='R') memory_read();
            if(c=='G') {wr(0x10,0);text("RUN\r\n");}
        }
    }
}

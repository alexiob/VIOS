/* RAM-only ESP32-PICO-D4 diagnostic; no SPI flash access. Reset restores VDP.
 * Register/ROM definitions: ESP-IDF esp32 soc headers and esp32.rom.ld.
 * GPIO routing: Olimex AgonLight2 Rev B / Agon VDP 1.04.
 */
#include <stdint.h>
#define REG(a) (*(volatile uint32_t *)(a))
#define U0 0x3ff40000u
#define U2 0x3ff6e000u
extern uint32_t _bss_start, _bss_end;
extern void ets_set_user_start(uint32_t);
extern void ets_delay_us(uint32_t);
extern void gpio_pad_select_gpio(uint32_t);
extern void gpio_matrix_in(uint32_t, uint32_t, uint32_t);
extern void gpio_matrix_out(uint32_t, uint32_t, uint32_t, uint32_t);

static void put(unsigned char c) {
    while (((REG(U0+0x1c) >> 16) & 255) >= 120) {}
    REG(U0) = c;
}
static void text(const char *p) { while (*p) put((unsigned char)*p++); }
static void hex(unsigned char c) {
    static const char digits[]="0123456789ABCDEF";
    put(digits[c>>4]); put(digits[c&15]);
}
static void baud(uint32_t rate, uint32_t clock) {
    uint32_t divisor=(clock*16u)/rate;
    REG(U2+0x14)=(divisor>>4)|((divisor&15)<<20);
    REG(U2+0x20)=(1u<<27)|0x1c|(3u<<17);
    REG(U2+0x20)=(1u<<27)|0x1c;
}
void __attribute__((used)) stub_main(void) {
    uint32_t ps, clock, div, k=0, sample=0;
    const uint32_t rates[]={1152000,384000,115200,9600};
    __asm__ volatile("rsil %0, 15" : "=a"(ps));
    (void)ps;
    ets_set_user_start(0);
    for (uint32_t *p=&_bss_start;p<&_bss_end;++p) *p=0;
    div=REG(U0+0x14);
    clock=(((div&0xfffff)*16)+((div>>20)&15))*7200u;
    REG(0x3ff000c0)|=1u<<23;  /* UART2 clock */
    REG(0x3ff000c4)&=~(1u<<23);
    gpio_pad_select_gpio(2); gpio_pad_select_gpio(13); gpio_pad_select_gpio(34);
    gpio_matrix_in(34,198,0); gpio_matrix_out(2,198,0,0);
    REG(0x3ff4400c)=1u<<13; /* RTS low: eZ80 is clear to send. */
    REG(0x3ff44024)=(1u<<2)|(1u<<13);
    text("\r\nagon-uart-probe RAM only; no flash writes\r\n");
    text("1=1152000 2=384000 3=115200 4=9600; output is RX hex\r\n");
    baud(rates[k],clock); text("BAUD 1\r\n");
    for (;;) {
        while (REG(U2+0x1c)&255) { hex((unsigned char)REG(U2)); put(' '); }
        if (REG(U0+0x1c)&255) {
            unsigned char c=(unsigned char)REG(U0);
            if(c>='1'&&c<='4') { k=c-'1'; baud(rates[k],clock); text("\r\nBAUD ");put(c);text("\r\n"); sample=0; }
        }
        ets_delay_us(1000);
        if (++sample==2000) { sample=0;k=(k+1)&3;baud(rates[k],clock);text("\r\nBAUD ");put('1'+k);text("\r\n"); }
    }
}

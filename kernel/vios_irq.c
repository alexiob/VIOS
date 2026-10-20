/* Interrupt-system probe (tasks design stage B0, read-only). MIT.
 * docs/superpowers/specs/2026-10-19-vios-tasks.md: before VIOS enables any
 * interrupt it must know where MOS left the vector chain and which sources
 * MOS left armed. IRQ-PROBE reports, from the CPU itself (no ZDI wire needed):
 *  - I, the flash vector table base, and for every vector its chain
 *    flash table -> 1st jump table (JP) -> RAM 2nd jump table (JP handler);
 *  - the handlers that differ from the common default (MOS's own ISRs);
 *  - timer, clock-gating, UART and GPIO interrupt configuration ports.
 * Valid only when MOS booted this run (the stub's VIOS_BOOT_MAGIC): after a
 * ZDI load MOS never filled its RAM table. */
#include "vios/runtime.h"
extern unsigned char platform_irq_i(void);
extern unsigned char platform_io_read(unsigned port);
extern unsigned platform_boot_source;
#define VIOS_BOOT_MAGIC 0x56420Bu
#define VECTORS 48u
static void hex(unsigned long v,unsigned digits){char b[9];for(unsigned i=0;i<digits;++i){unsigned d=(unsigned)(v>>(4*(digits-1-i)))&15u;b[i]=(char)(d<10?'0'+d:'A'+d-10);}b[digits]=0;vios_out(b);}
static unsigned long get24(const volatile unsigned char *p){return (unsigned long)p[0]|((unsigned long)p[1]<<8)|((unsigned long)p[2]<<16);}
/* Vector k (IVECT = 2k): the RAM slot it jumps through and that slot's handler, 0 if the chain is not JP/JP. */
static unsigned long slot_of(unsigned i,unsigned k,unsigned long *handler){
 const volatile unsigned char *vt=(const volatile unsigned char *)((unsigned long)i<<8);
 unsigned long first=(unsigned long)vt[2*k]|((unsigned long)vt[2*k+1]<<8);
 const volatile unsigned char *jp1=(const volatile unsigned char *)first;
 if(jp1[0]!=0xC3)return 0;
 unsigned long second=get24(jp1+1);
 const volatile unsigned char *jp2=(const volatile unsigned char *)second;
 *handler=jp2[0]==0xC3?get24(jp2+1):0;
 return second;
}
static int irq_probe(void){
 unsigned i=platform_irq_i();
 vios_out("irq boot ");vios_out(platform_boot_source==VIOS_BOOT_MAGIC?"mos":"zdi");
 vios_out(" I ");hex(i,2);vios_out("\r\n");
 unsigned long handlers[VECTORS],slots[VECTORS],common=0;unsigned best=0;
 for(unsigned k=0;k<VECTORS;++k){handlers[k]=0;slots[k]=slot_of(i,k,&handlers[k]);}
 for(unsigned k=0;k<VECTORS;++k){unsigned n=0;for(unsigned j=0;j<VECTORS;++j)n+=handlers[j]==handlers[k];if(n>best){best=n;common=handlers[k];}}
 vios_out("irq table ");hex(slots[0],6);vios_out(" default ");hex(common,6);vios_out(" x");vios_uint(best);vios_out("\r\n");
 for(unsigned k=0;k<VECTORS;++k)if(handlers[k]!=common||!slots[k]||slots[k]!=slots[0]+4ul*k){
  vios_out("irq vector ");hex(2*k,2);vios_out(" slot ");hex(slots[k],6);vios_out(" handler ");hex(handlers[k],6);vios_out("\r\n");}
 static const struct {const char *name;unsigned port;} ports[]={
  {"TMR0_CTL",0x80},{"TMR1_CTL",0x83},{"TMR2_CTL",0x86},{"TMR3_CTL",0x89},{"TMR4_CTL",0x8C},{"TMR5_CTL",0x8F},{"TMR_ISS",0x92},
  {"CLK_PPD1",0xDB},{"CLK_PPD2",0xDC},{"UART0_IER",0xC1},{"UART1_IER",0xD1},
  {"PB_DDR",0x9B},{"PB_ALT1",0x9C},{"PB_ALT2",0x9D},{"PC_DDR",0x9F},{"PC_ALT1",0xA0},{"PC_ALT2",0xA1},{"PD_DDR",0xA3},{"PD_ALT1",0xA4},{"PD_ALT2",0xA5}};
 vios_out("irq ports");
 for(unsigned n=0;n<sizeof ports/sizeof ports[0];++n){vios_out(" ");vios_out(ports[n].name);vios_out("=");hex(platform_io_read(ports[n].port),2);}
 vios_out("\r\n");return 1;
}

/* ---- Stage B1: 1 kHz tick and millisecond clock (preemption stays off) ----
 * All 48 RAM jump-table slots point at VIOS handlers before the first EI:
 * PRT1 (IVECT 0Ch) at the tick, every other vector at a counting no-op.
 * MOS's armed sources are disarmed first (B0 on the board, 2026-10-20: UART0/1
 * IER were 0; PB1, the VDP's vertical blank, was a rising-edge interrupt). */
extern void platform_io_write(unsigned port,unsigned char value);
extern unsigned char platform_irq_save(void);
extern void platform_irq_restore(unsigned char enabled);
extern void vios_tick_isr(void),vios_stray_isr(void);
extern volatile unsigned char vios_ms_clock[4],vios_irq_stray[2];
#define PRT1_VECTOR 6u                  /* IVECT 0Ch / 2 */
#define TICK_RELOAD 288u                /* 18.432 MHz / 64 / 288 = 1000 Hz */
static int ticking;
static unsigned long ms_now(void){
 unsigned char on=platform_irq_save();
 unsigned long ms=(unsigned long)vios_ms_clock[0]|((unsigned long)vios_ms_clock[1]<<8)|((unsigned long)vios_ms_clock[2]<<16)|((unsigned long)vios_ms_clock[3]<<24);
 platform_irq_restore(on);return ms;
}
static void put_jp(volatile unsigned char *slot,void (*handler)(void)){unsigned long a=(unsigned long)handler;slot[1]=(unsigned char)a;slot[2]=(unsigned char)(a>>8);slot[3]=(unsigned char)(a>>16);slot[0]=0xC3;}
/* Start the tick; 0 (and interrupts stay off) unless MOS booted this run and
 * the vector chain is exactly MOS's layout. */
int vios_tick_start(void){
 if(ticking)return 1;
 if(platform_boot_source!=VIOS_BOOT_MAGIC)return 0;
 unsigned i=platform_irq_i();unsigned long handler=0,base=slot_of(i,0,&handler);
 if(base<0x0B0000ul||base>0x0BFF00ul)return 0;
 for(unsigned k=0;k<VECTORS;++k){unsigned long h;if(slot_of(i,k,&h)!=base+4ul*k)return 0;}
 (void)platform_irq_save();
 platform_io_write(0x9C,platform_io_read(0x9C)&(unsigned char)~2u);   /* PB1: plain input, no edge interrupt */
 platform_io_write(0x9D,platform_io_read(0x9D)&(unsigned char)~2u);
 platform_io_write(0xC1,0);platform_io_write(0xD1,0);                /* UART0/1: polled, no interrupts */
 for(unsigned k=0;k<VECTORS;++k)put_jp((volatile unsigned char *)(base+4ul*k),k==PRT1_VECTOR?vios_tick_isr:vios_stray_isr);
 platform_io_write(0xDC,platform_io_read(0xDC)&(unsigned char)~2u);   /* CLK_PPD2: PRT1 clocked */
 platform_io_write(0x92,platform_io_read(0x92)&(unsigned char)~0x0Cu);/* TMR_ISS: PRT1 on the system clock */
 platform_io_write(0x83,0);
 platform_io_write(0x84,(unsigned char)TICK_RELOAD);platform_io_write(0x85,(unsigned char)(TICK_RELOAD>>8));
 (void)platform_io_read(0x83);
 platform_io_write(0x83,0x5B);          /* IRQ_EN, continuous, /64, reload+restart, enable */
 ticking=1;platform_irq_restore(1);return 1;
}
static uint32_t tick_clock(void){return (uint32_t)ms_now();}
/* Stage B2: forth/system.c plans a delay; the ISR counts it down and steals. */
extern volatile unsigned char vios_preempt_armed;extern volatile unsigned vios_preempt_count;
static void preempt_arm(unsigned delay_ms){
 unsigned char on=platform_irq_save();
 vios_preempt_count=delay_ms;vios_preempt_armed=delay_ms!=0;
 platform_irq_restore(on);
}
static int ticks(void){return fvm_push((fcell)ms_now()&FVM_MASK);}
static int irq_stats(void){
 vios_out("irq tick ");vios_out(ticking?"on":"off");vios_out(" ms ");vios_uint((unsigned)ms_now());
 vios_out(" stray ");vios_uint((unsigned)(vios_irq_stray[0]|vios_irq_stray[1]<<8));vios_out("\r\n");return 1;
}
int vios_board_init(void){
 if(ticking){forth_register_clock(tick_clock);forth_register_preempt(preempt_arm);}   /* MS deadlines; tick preemption (B2) */
 return forth_register_native("IRQ-PROBE",irq_probe,0)&&forth_register_native("TICKS",ticks,FORTH_NATIVE_TASK)&&forth_register_native("IRQ-STATS",irq_stats,0);
}

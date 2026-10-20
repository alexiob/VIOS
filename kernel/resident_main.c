#include "forth/system.h"
#include "platform/console.h"
#include "platform/timer.h"
#include "platform/vdp.h"
#include "vios/runtime.h"
#include "vios/workbench_runtime.h"
int vios_tick_start(void);   /* kernel/vios_irq.c: 1 kHz tick when MOS booted this run */
void kernel_main(void){
 platform_console_init();platform_timer_init();(void)vios_tick_start();forth_register_line_mode(platform_console_line_mode);forth_register_task_gate(platform_console_quiet);forth_init();forth_set_engine(FORTH_DIRECT);
 if(!vios_runtime_init()||!vios_boot_startup()){
  vios_out("VIOS startup error: ");vios_out(forth_last_error());vios_out("\r\n");platform_boot_ready();
 }
 /* Local keyboard + VGA are usable without a host: start on the console
  * view. A missing/unexpected display leaves the headless REPL unchanged. */
 (void)vios_console_open();(void)vios_user_startup();(void)vios_autorun();
 forth_repl();
}

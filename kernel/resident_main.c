#include "forth/system.h"
#include "platform/console.h"
#include "platform/timer.h"
#include "platform/vdp.h"
#include "vios/runtime.h"
void kernel_main(void){
 platform_console_init();platform_timer_init();forth_register_line_mode(platform_console_line_mode);forth_init();forth_set_engine(FORTH_DIRECT);
 if(!vios_runtime_init()||!vios_boot_startup()){
  vios_out("VIOS startup error: ");vios_out(forth_last_error());vios_out("\r\n");platform_boot_ready();
 }
 forth_repl();
}

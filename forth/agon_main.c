#include "forth/system.h"
#include "platform/console.h"
#include "platform/timer.h"
void kernel_main(void) {
 platform_console_init();platform_timer_init();forth_init();forth_set_engine(FORTH_DIRECT);forth_repl();
}

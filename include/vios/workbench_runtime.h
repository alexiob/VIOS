#ifndef VIOS_WORKBENCH_RUNTIME_H
#define VIOS_WORKBENCH_RUNTIME_H
int vios_workbench_runtime_init(void);
int vios_workbench_command(const char *word,const char **rest);
void vios_workbench_idle(int changed);
/* VGA REPL console view: open resets the display; putc is the RAM-only
 * console output tap (ignored while the view is closed). */
int vios_console_open(void);
void vios_console_putc(unsigned char c);
#endif

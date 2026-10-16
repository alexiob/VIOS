#include <stdio.h>
#include <stdlib.h>
#include "forth/system.h"
void platform_console_putc(unsigned char c) { putchar(c); }
int platform_console_getc(void) { int c=getchar();if(c==EOF)exit(0);return c; }
void platform_console_init(void) {}
const char *platform_identity(void) { return "VIOS host reference"; }
void platform_restart(void) {}
int main(int argc,char **argv) {
 (void)argv;
 char line[512];forth_init();
 if(argc>1)forth_repl();
 while(fgets(line,sizeof line,stdin)) {
  if(!forth_eval(line)) printf("ERR: %s\n",forth_last_error());
 }
 return 0;
}

#ifndef VIOS_FORTH_SYSTEM_H
#define VIOS_FORTH_SYSTEM_H
#include "forth/vm.h"
enum forth_engine { FORTH_REFERENCE,FORTH_TOKEN,FORTH_DIRECT };
void forth_init(void);
int forth_eval(const char *source);
int forth_execute(const char *name);
void forth_set_engine(enum forth_engine engine);
enum forth_engine forth_engine(void);
const char *forth_last_error(void);
int forth_compiling(void);
void forth_report(void);
size_t forth_code_bytes(enum forth_engine engine);
fcell forth_get_base(void);
void forth_set_base(fcell radix);
void forth_repl(void);
#endif

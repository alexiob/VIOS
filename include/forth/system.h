#ifndef VIOS_FORTH_SYSTEM_H
#define VIOS_FORTH_SYSTEM_H
#include "forth/vm.h"
enum forth_engine { FORTH_REFERENCE,FORTH_TOKEN,FORTH_DIRECT };
#define FORTH_NATIVE_PURE 1u
/* PURE callbacks are trusted: deterministic/nonblocking, no external writes;
 * all VM-data reads/writes must use fvm_* accessors for transaction overlays. */
int forth_register_native(const char *name,int (*callback)(void),unsigned flags);
int forth_register_command(int (*hook)(const char *word,const char **rest));
/* Called only at an empty, noncompiling REPL input boundary. changed marks
 * the first idle turn after startup, a command, or an interactive abort. */
void forth_register_idle(void (*hook)(int changed));
int forth_error(const char *message);
fcell forth_find_xt(const char *name);
const char *forth_xt_name(fcell xt);
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
void forth_transport_crc(int required);
/* Module implementation boundary: synchronous transactions, not task-safe. */
int forth_transaction_eval(const char *source,const char *test_word,unsigned origin,int commit);
int forth_transaction_rollback(void);
void forth_transaction_forget_rollback(void);
int forth_transaction_active(void);
int forth_transaction_error(const char *message);
void forth_transaction_sizes(unsigned *words,size_t *token,size_t *direct,size_t *data);
#endif

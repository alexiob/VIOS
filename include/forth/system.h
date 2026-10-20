#ifndef VIOS_FORTH_SYSTEM_H
#define VIOS_FORTH_SYSTEM_H
#include "forth/vm.h"
enum forth_engine { FORTH_REFERENCE,FORTH_TOKEN,FORTH_DIRECT };
#define FORTH_NATIVE_PURE 1u
/* TASK callbacks may run inside a background task slice (no VM re-entry). */
#define FORTH_NATIVE_TASK 2u
#define FORTH_TASKS_MAX 4
#define FORTH_TASK_DS 64
#define FORTH_TASK_RS 64
#define FORTH_TASK_LOOPS 16
#define FORTH_TASK_OUT 128
#define FORTH_TASK_SLICE 1024 /* Agon sweep 2026-10-19: no REPL latency cost */
/* PURE callbacks are trusted: deterministic/nonblocking, no external writes;
 * all VM-data reads/writes must use fvm_* accessors for transaction overlays. */
int forth_register_native(const char *name,int (*callback)(void),unsigned flags);
int forth_register_command(int (*hook)(const char *word,const char **rest));
/* Called only at an empty, noncompiling REPL input boundary. changed marks
 * the first idle turn after startup, a command, or an interactive abort. */
void forth_register_idle(void (*hook)(int changed));
/* Platform input policy, not VM state: survives forth_init(). The REPL calls
 * hook(1) while awaiting a command line and hook(0) while it executes one. */
void forth_register_line_mode(void (*hook)(int awaiting_line));
/* One round-robin pass over ready/woken background tasks, one budgeted slice
 * each; no-op while compiling, in a transaction/observer or inside a task.
 * Called at REPL idle, foreground KEY waits, PAUSE and MS. Returns slices. */
unsigned forth_tasks_run(void);
/* Millisecond clock for MS; survives forth_init() like the line-mode hook. */
void forth_register_clock(uint32_t (*hook)(void));
/* Platform admission check before each background round; 0 skips the round.
 * The resident console listens briefly and pauses flow-controlled input so
 * a slice never runs while host bytes can overrun the UART. Survives init. */
void forth_register_task_gate(int (*gate)(void));
/* Tick preemption (tasks design B2): the board arms its tick ISR with a delay
 * in ms (0 disarms); the ISR steals fvm_budget into forth_budget_stolen and
 * sets forth_tick_pending. Host tests simulate ticks with a tick source,
 * called before each VM run. */
extern volatile unsigned char forth_tick_pending;
extern volatile fcell forth_budget_stolen;
void forth_register_preempt(void (*arm)(unsigned delay_ms));
void forth_register_tick_source(void (*hook)(void));
/* Console tasks' held output, printed now (the REPL does this when idle); bytes printed. */
unsigned forth_console_tasks_flush(void);
/* Ctrl-C at foreground ticks: poll returns 1 once per request; the running
 * command then fails with "interrupted" at the next safe point. */
void forth_register_interrupt(int (*poll)(void));
/* Task-aware I/O for kernel services: forth_emit uses the console in the
 * foreground and the running task's output ring in a background slice. */
void forth_emit(unsigned char c);
unsigned forth_current_task(void); /* 0 foreground, else 1..FORTH_TASKS_MAX */
/* Called once when a task leaves ready/sleeping (done, failed or killed). */
void forth_register_task_exit(void (*hook)(unsigned id));
/* Handle I/O for redirected task stdin/stdout (write: 1 ok/0 full/-1 broken;
 * read: 1 byte/0 empty/-1 EOF; give: transfer ownership to a task id). */
void forth_register_task_io(int (*write)(unsigned handle,unsigned char c),int (*read)(unsigned handle,unsigned char *c),int (*give)(unsigned handle,unsigned task));
struct forth_task_info {
 const char *name,*error;unsigned state;fcell xt,depth,rdepth,ds_peak,rs_peak;
 uint32_t slices,steps;unsigned output_bytes,output_dropped;
};
int forth_task_info(unsigned id,struct forth_task_info *out);
/* Start xt as a background task with handles as stdin/stdout (0 = none),
 * without running Forth now; returns its id or 0 (no slot / not allowed). */
unsigned forth_task_start(fcell xt,unsigned in_handle,unsigned out_handle);
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
/* Boot-module cache (docs/superpowers/specs/2026-10-19-vios-boot-speed.md,
 * Phase 3). A snapshot holds what module loads add to the dictionary after a
 * base state: their words, code, source and source map, all hash buckets and
 * data space up to its use. Only valid for the same resident build and base.
 * io moves exactly size bytes or returns 0. A failed read may leave the
 * dictionary inconsistent: the caller must reinitialise the runtime. */
struct forth_snapshot_base {uint32_t token,direct,data,source,count,map;};
typedef int (*forth_snapshot_io)(void *data,size_t size,void *ctx);
void forth_snapshot_base(struct forth_snapshot_base *base);
int forth_snapshot_write(const struct forth_snapshot_base *base,forth_snapshot_io put,void *ctx);
int forth_snapshot_read(const struct forth_snapshot_base *base,forth_snapshot_io get,void *ctx);
#endif

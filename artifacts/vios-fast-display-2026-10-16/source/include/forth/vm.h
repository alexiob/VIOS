#ifndef VIOS_FORTH_VM_H
#define VIOS_FORTH_VM_H
#include <stdint.h>
#include <stddef.h>
#ifdef __ez80__
typedef unsigned int fcell;
#else
typedef uint32_t fcell;
#endif
#define FVM_MASK ((fcell)0xffffffUL)
#define FVM_DS 256
#define FVM_RS 256
#define FVM_LOOPS 32
#define FVM_TRAP(op) (0x80u+(op))
enum fop {
 F_EXIT,F_LIT,F_CALL,F_BRANCH,F_ZBRANCH,F_DUP,F_DROP,F_SWAP,F_OVER,
 F_ADD,F_SUB,F_MUL,F_AND,F_OR,F_XOR,F_INVERT,F_EQ,F_LT,F_ULT,F_ZEROEQ,
 F_FETCH,F_STORE,F_CFETCH,F_CSTORE,F_EMIT,F_DOT,F_CR,F_KEY,
 F_TOR,F_RFROM,F_RFETCH,F_DO,F_LOOP,F_I,F_EXECUTE,F_DEFER,
 F_DIV,F_MOD,F_LSHIFT,F_RSHIFT,F_DEPTH,F_NATIVE,F_OP_COUNT
};
enum ferr { FE_OK,FE_DS_UNDER,FE_DS_OVER,FE_RS_UNDER,FE_RS_OVER,
 FE_LOOP_UNDER,FE_LOOP_OVER,FE_MEMORY,FE_OPCODE,FE_BUDGET,FE_ARITH };
extern fcell fvm_ds[FVM_DS],fvm_rs[FVM_RS],fvm_loops[FVM_LOOPS*2];
extern fcell fvm_depth,fvm_rdepth,fvm_ldepth,fvm_ip,fvm_error,fvm_budget;
extern fcell fvm_ds_peak,fvm_rs_peak;
void fvm_reset(void);
int fvm_push(fcell n);
fcell fvm_pop(void);
unsigned char fvm_reference_run(void);
/* Complex arithmetic shared with assembly trap handling; returns 0 on error. */
int fvm_complex(unsigned op);
/* Memory is native addressing on eZ80; registered address regions on the host. */
void fvm_memory_reset(void);
int fvm_region(fcell base,size_t length,unsigned char *data);
fcell fvm_address(const void *pointer);
unsigned char fvm_read8(fcell address);
void fvm_write8(fcell address,unsigned char value);
fcell fvm_read_cell(fcell address);
void fvm_write_cell(fcell address,fcell value);
#ifdef __ez80__
unsigned char fvm_token_run(void);
unsigned char fvm_direct_run(void);
extern fcell fvm_direct_primitives[F_OP_COUNT];
extern void fvm_docol(void);
#endif
#endif

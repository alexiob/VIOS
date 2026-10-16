#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "forth/vm.h"

static unsigned char code[4096], data[128];
static size_t used;
static void setup(void) {
    fvm_reset(); fvm_memory_reset();
    memset(code, 0, sizeof code); memset(data, 0, sizeof data);
    assert(fvm_region(0x1000, sizeof code, code));
    assert(fvm_region(0x4000, sizeof data, data));
    fvm_ip = 0x1000; used = 0;
}
static void op(unsigned n) { assert(used < sizeof code); code[used++] = (unsigned char)n; }
static void operand(fcell n) {
    op(n & 255); op((n >> 8) & 255); op((n >> 16) & 255);
}
static void literal(fcell n) { op(F_LIT); operand(n); }
static void success(void) { assert(fvm_reference_run() == 0); assert(fvm_error == FE_OK); }
static void failure(unsigned e) { assert(fvm_reference_run() == 255); assert(fvm_error == e); }
static void arithmetic(void) {
    setup(); literal(0xffffff); literal(2); op(F_ADD); op(F_DUP);
    literal(3); op(F_SUB); op(F_SWAP); op(F_MUL); op(F_EXIT); success();
    assert(fvm_depth == 1 && fvm_pop() == 0xfffffe);
    setup(); literal(0xffffff); literal(0xffffff); op(F_MUL); op(F_EXIT); success();
    assert(fvm_pop() == 1);
    setup(); literal(0x800000); literal(1); op(F_SUB); op(F_EXIT); success();
    assert(fvm_pop() == 0x7fffff);
    setup(); assert(fvm_push(0x1ffffff)); assert(fvm_pop() == 0xffffff);
    setup(); literal(0x123456); literal(0xff00ff); op(F_AND);
    literal(0x10); op(F_OR); literal(0x100); op(F_XOR); op(F_INVERT); op(F_EXIT);
    success(); assert(fvm_pop() == ((~(((0x123456 & 0xff00ff) | 0x10) ^ 0x100)) & 0xffffff));
    setup(); literal(0x800000); literal(0x7fffff); op(F_LT);
    literal(0); op(F_ZEROEQ); op(F_AND); op(F_EXIT); success();
    assert(fvm_pop() == 0xffffff);
    setup(); literal(0x800000); literal(0x7fffff); op(F_ULT); op(F_ZEROEQ);
    literal(1); literal(1); op(F_EQ); op(F_AND); op(F_EXIT); success();
    assert(fvm_pop() == 0xffffff);
    setup(); literal(4); literal(9); op(F_OVER); op(F_DROP); op(F_SWAP); op(F_DEPTH); op(F_EXIT);
    success(); assert(fvm_pop() == 2); assert(fvm_pop() == 4); assert(fvm_pop() == 9);
}
static void complex_arithmetic(void) {
    static const fcell dividends[] = {7, 0xfffff9, 7, 0xfffff9};
    static const fcell divisors[] = {3, 3, 0xfffffd, 0xfffffd};
    static const fcell quotients[] = {2, 0xfffffe, 0xfffffe, 2};
    static const fcell remainders[] = {1, 0xffffff, 1, 0xffffff};
    unsigned i;
    for (i = 0; i < 4; ++i) {
        setup(); literal(dividends[i]); literal(divisors[i]); op(F_DIV); op(F_EXIT);
        success(); assert(fvm_pop() == quotients[i]);
        setup(); literal(dividends[i]); literal(divisors[i]); op(F_MOD); op(F_EXIT);
        success(); assert(fvm_pop() == remainders[i]);
    }
    setup(); literal(12); literal(0); op(F_DIV); failure(FE_ARITH);
    setup(); literal(12); literal(0); op(F_MOD); failure(FE_ARITH);
    setup(); literal(0x800000); literal(0xffffff); op(F_DIV); failure(FE_ARITH);
    setup(); literal(0x800000); literal(0xffffff); op(F_MOD); failure(FE_ARITH);
    setup(); literal(1); literal(23); op(F_LSHIFT); literal(23); op(F_RSHIFT); op(F_EXIT);
    success(); assert(fvm_pop() == 1);
    for (i = 24; i < 26; ++i) {
        setup(); literal(0xffffff); literal(i); op(F_LSHIFT); op(F_EXIT);
        success(); assert(fvm_pop() == 0);
        setup(); literal(0xffffff); literal(i); op(F_RSHIFT); op(F_EXIT);
        success(); assert(fvm_pop() == 0);
    }
    setup(); assert(fvm_push(0x800000)); assert(fvm_push(0xffffff));
    assert(fvm_complex(F_MUL)); assert(fvm_pop() == 0x800000);
    setup(); assert(!fvm_complex(F_ADD)); assert(fvm_error == FE_OPCODE);
}
static void calls_branches(void) {
    setup(); op(F_CALL); operand(0x100a); literal(2); op(F_ADD); op(F_EXIT);
    literal(40); op(F_EXIT); success(); assert(fvm_pop() == 42 && !fvm_rdepth);
    setup(); literal(0); op(F_ZBRANCH); operand(0x100c);
    literal(99); literal(42); op(F_EXIT); success(); assert(fvm_pop() == 42 && !fvm_depth);
    setup(); literal(1); op(F_ZBRANCH); operand(0x1010);
    literal(42); op(F_BRANCH); operand(0x1014);
    literal(99); op(F_EXIT); success(); assert(fvm_pop() == 42 && !fvm_depth);
    setup(); literal(123); op(F_TOR); op(F_RFETCH); op(F_RFROM); op(F_ADD); op(F_EXIT);
    success(); assert(fvm_pop() == 246 && !fvm_rdepth);
}
static void loops(void) {
    setup(); literal(3); literal(0); op(F_DO); /* body at 0x1009 */
    op(F_I); op(F_LOOP); operand(0x1009); op(F_EXIT); success();
    assert(fvm_depth == 3 && !fvm_ldepth);
    assert(fvm_pop() == 2); assert(fvm_pop() == 1); assert(fvm_pop() == 0);
    setup(); literal(1); literal(0xffffff); op(F_DO);
    op(F_I); op(F_LOOP); operand(0x1009); op(F_EXIT); success();
    assert(fvm_pop() == 0 && fvm_pop() == 0xffffff && !fvm_ldepth);
    setup(); literal(2); literal(0); op(F_DO);
    literal(2); literal(0); op(F_DO); /* inner body at 0x1012 */
    op(F_I); op(F_LOOP); operand(0x1012);
    op(F_LOOP); operand(0x1009); op(F_EXIT); success();
    assert(fvm_depth == 4 && !fvm_ldepth);
    assert(fvm_pop() == 1 && fvm_pop() == 0 && fvm_pop() == 1 && fvm_pop() == 0);
    setup(); literal(0); literal(0); op(F_DO); op(F_LOOP); operand(0x1009);
    fvm_budget = 50; failure(FE_BUDGET); assert(fvm_ldepth == 1);
}
static void memory(void) {
    setup(); fvm_write_cell(0x4001, 0x123456);
    assert(data[1] == 0x56 && data[2] == 0x34 && data[3] == 0x12);
    assert(fvm_read_cell(0x4001) == 0x123456);
    assert(fvm_address(data + 127) == 0x407f);
    literal(0xfedcba); literal(0x4003); op(F_STORE);
    literal(0x4003); op(F_FETCH);
    literal(0x1234); literal(0x4000); op(F_CSTORE);
    literal(0x4000); op(F_CFETCH); op(F_EXIT); success();
    assert(fvm_pop() == 0x34 && fvm_pop() == 0xfedcba);
    setup(); fvm_write8(0x407f, 0xaa); assert(fvm_read8(0x407f) == 0xaa);
    fvm_write_cell(0x407e, 0x123456); assert(fvm_error == FE_MEMORY);
    assert(data[126] == 0 && data[127] == 0xaa); /* reject the whole write */
    setup(); literal(0x4080); op(F_CFETCH); failure(FE_MEMORY);
    setup(); literal(0x407e); op(F_FETCH); failure(FE_MEMORY);
    setup(); fvm_ip = 0x1fff; code[4095] = F_LIT; failure(FE_MEMORY);
    setup(); assert(!fvm_region(0x4070, 32, code)); assert(fvm_error == FE_MEMORY);
    setup(); assert(!fvm_region(0xffffff, 2, data)); assert(fvm_error == FE_MEMORY);
    setup(); assert(!fvm_region(0x5000, 0, data)); assert(fvm_error == FE_MEMORY);
    setup(); assert(!fvm_region(0x5000, 1, NULL)); assert(fvm_error == FE_MEMORY);
    setup(); assert(!fvm_address((const void *)(uintptr_t)1)); assert(fvm_error == FE_MEMORY);
    setup(); fvm_memory_reset();
    for (unsigned i = 0; i < 8; ++i) assert(fvm_region(i * 0x100, 1, data));
    assert(!fvm_region(0x800, 1, data)); assert(fvm_error == FE_MEMORY);
}
static void traps_budget_errors(void) {
    static const unsigned traps[] = {F_EMIT, F_DOT, F_CR, F_KEY, F_EXECUTE};
    unsigned i;
    for (i = 0; i < sizeof traps / sizeof *traps; ++i) {
        setup(); op(traps[i]); op(F_EXIT); fvm_budget = 2;
        assert(fvm_reference_run() == FVM_TRAP(traps[i]));
        assert(fvm_ip == 0x1001 && fvm_budget == 1 && fvm_error == FE_OK);
        success(); assert(fvm_budget == 0);
    }
    setup(); fvm_write_cell(0x4000, 0x1020); op(F_DEFER); operand(0x4000); op(F_EXIT);
    assert(fvm_reference_run() == FVM_TRAP(F_DEFER));
    assert(fvm_ip == 0x1004 && fvm_pop() == 0x1020); success();
    setup(); op(F_DEFER); operand(0x4080); failure(FE_MEMORY);
    setup(); op(F_NATIVE); operand(0xffffff); op(F_EXIT);
    assert(fvm_reference_run() == FVM_TRAP(F_NATIVE));
    assert(fvm_ip == 0x1004 && fvm_pop() == 0xffffff); success();
    setup(); op(F_BRANCH); operand(0x1000); fvm_budget = 17;
    failure(FE_BUDGET); assert(!fvm_budget);
    setup(); op(255); failure(FE_OPCODE);
    setup(); op(F_DROP); failure(FE_DS_UNDER);
    setup(); op(F_ADD); failure(FE_DS_UNDER);
    setup(); op(F_RFROM); failure(FE_RS_UNDER);
    setup(); op(F_RFETCH); failure(FE_RS_UNDER);
    setup(); op(F_I); failure(FE_LOOP_UNDER);
    setup(); op(F_LOOP); operand(0x1000); failure(FE_LOOP_UNDER);
    setup(); for (i = 0; i < FVM_DS; ++i) assert(fvm_push(i));
    assert(!fvm_push(1)); assert(fvm_error == FE_DS_OVER && fvm_depth == FVM_DS);
    setup(); assert(fvm_pop() == 0 && fvm_error == FE_DS_UNDER);
    setup(); fvm_rdepth = FVM_RS; literal(1); op(F_TOR); failure(FE_RS_OVER);
    setup(); fvm_rdepth = FVM_RS; op(F_CALL); operand(0x1000); failure(FE_RS_OVER);
    setup(); fvm_ldepth = FVM_LOOPS; literal(2); literal(0); op(F_DO); failure(FE_LOOP_OVER);
    setup(); fvm_budget = 0; op(F_EXIT); failure(FE_BUDGET);
}
static void stack_peaks(void) {
    setup();
    assert(fvm_ds_peak == 0 && fvm_rs_peak == 0);
    assert(fvm_push(1)); assert(fvm_push(2)); assert(fvm_push(3));
    assert(fvm_pop() == 3 && fvm_pop() == 2 && fvm_pop() == 1);
    assert(!fvm_depth && fvm_ds_peak == 3);
    literal(42); op(F_DUP); op(F_DROP); op(F_TOR); op(F_RFROM); op(F_DROP); op(F_EXIT);
    success(); assert(!fvm_depth && !fvm_rdepth);
    assert(fvm_ds_peak == 3 && fvm_rs_peak == 1);
    setup(); op(F_CALL); operand(0x1005); op(F_EXIT);
    op(F_CALL); operand(0x100a); op(F_EXIT); op(F_EXIT);
    success(); assert(!fvm_rdepth && fvm_rs_peak == 2);
    fvm_reset(); assert(!fvm_ds_peak && !fvm_rs_peak);
}
int main(void) {
    arithmetic(); complex_arithmetic(); calls_branches(); loops(); memory(); traps_budget_errors();
    stack_peaks();
    puts("Forth VM contract checks passed"); return 0;
}

/* Standard-C semantic reference for the 24-bit token machine. */
#include "forth/vm.h"

fcell fvm_ds[FVM_DS], fvm_rs[FVM_RS], fvm_loops[FVM_LOOPS * 2];
fcell fvm_depth, fvm_rdepth, fvm_ldepth, fvm_ip, fvm_error, fvm_budget;
fcell fvm_ds_peak, fvm_rs_peak;

void fvm_reset(void) {
    fvm_depth = fvm_rdepth = fvm_ldepth = fvm_ip = fvm_error = 0;
    fvm_ds_peak = fvm_rs_peak = 0;
    fvm_budget = FVM_MASK;
}
int fvm_push(fcell n) {
    if (fvm_depth >= FVM_DS) { fvm_error = FE_DS_OVER; return 0; }
    fvm_ds[fvm_depth++] = n & FVM_MASK;
    /* Checked-profile instrumentation is part of measured execution cost. */
    if (fvm_depth > fvm_ds_peak) fvm_ds_peak = fvm_depth;
    return 1;
}
fcell fvm_pop(void) {
    if (!fvm_depth) { fvm_error = FE_DS_UNDER; return 0; }
    return fvm_ds[--fvm_depth];
}
static int need(unsigned n) {
    if (fvm_depth < n) { fvm_error = FE_DS_UNDER; return 0; }
    return 1;
}
static int32_t signed_cell(fcell n) {
    return (n & 0x800000UL) ? (int32_t)n - 0x1000000L : (int32_t)n;
}

int fvm_complex(unsigned op) {
    fcell a, b, result;
    int32_t sa, sb;
    if (op != F_MUL && op != F_DIV && op != F_MOD &&
        op != F_LSHIFT && op != F_RSHIFT) {
        fvm_error = FE_OPCODE; return 0;
    }
    if (!need(2)) return 0;
    a = fvm_ds[fvm_depth - 2]; b = fvm_ds[fvm_depth - 1];
    if (op == F_MUL) result = (fcell)((uint32_t)a * (uint32_t)b);
    else if (op == F_LSHIFT) result = b >= 24 ? 0 : (fcell)((uint32_t)a << b);
    else if (op == F_RSHIFT) result = b >= 24 ? 0 : a >> b;
    else {
        if (!b || (a == 0x800000UL && b == 0xffffffUL)) {
            fvm_error = FE_ARITH; return 0;
        }
        sa = signed_cell(a); sb = signed_cell(b);
        result = (fcell)(op == F_DIV ? sa / sb : sa % sb);
    }
    --fvm_depth;
    fvm_ds[fvm_depth - 1] = result & FVM_MASK;
    return 1;
}

static fcell operand(void) {
    fcell n = fvm_read_cell(fvm_ip);
    fvm_ip = (fvm_ip + 3) & FVM_MASK;
    return n;
}

unsigned char fvm_reference_run(void) {
    while (!fvm_error) {
        unsigned op;
        fcell a, b, n;
        if (!fvm_budget) { fvm_error = FE_BUDGET; break; }
        --fvm_budget;
        op = fvm_read8(fvm_ip);
        fvm_ip = (fvm_ip + 1) & FVM_MASK;
        if (fvm_error) break;
        switch (op) {
        case F_EXIT:
            if (!fvm_rdepth) return 0;
            fvm_ip = fvm_rs[--fvm_rdepth];
            break;
        case F_LIT:
            n = operand(); if (!fvm_error) fvm_push(n);
            break;
        case F_CALL:
            n = operand();
            if (fvm_error) break;
            if (fvm_rdepth >= FVM_RS) { fvm_error = FE_RS_OVER; break; }
            fvm_rs[fvm_rdepth++] = fvm_ip; fvm_ip = n;
            if (fvm_rdepth > fvm_rs_peak) fvm_rs_peak = fvm_rdepth;
            break;
        case F_BRANCH:
            n = operand(); if (!fvm_error) fvm_ip = n;
            break;
        case F_ZBRANCH:
            if (!need(1)) break;
            n = operand(); if (!fvm_error && !fvm_pop()) fvm_ip = n;
            break;
        case F_DUP:
            if (need(1)) fvm_push(fvm_ds[fvm_depth - 1]);
            break;
        case F_DROP:
            if (need(1)) --fvm_depth;
            break;
        case F_SWAP:
            if (!need(2)) break;
            a = fvm_ds[fvm_depth - 1];
            fvm_ds[fvm_depth - 1] = fvm_ds[fvm_depth - 2];
            fvm_ds[fvm_depth - 2] = a;
            break;
        case F_OVER:
            if (need(2)) fvm_push(fvm_ds[fvm_depth - 2]);
            break;
        case F_MUL: case F_DIV: case F_MOD: case F_LSHIFT: case F_RSHIFT:
            fvm_complex(op); break;
        case F_ADD: case F_SUB: case F_AND: case F_OR: case F_XOR:
        case F_EQ: case F_LT: case F_ULT:
            if (!need(2)) break;
            b = fvm_ds[--fvm_depth]; a = fvm_ds[fvm_depth - 1];
            switch (op) {
            case F_ADD: n = a + b; break;
            case F_SUB: n = a - b; break;
            case F_AND: n = a & b; break;
            case F_OR: n = a | b; break;
            case F_XOR: n = a ^ b; break;
            case F_EQ: n = a == b ? FVM_MASK : 0; break;
            case F_LT: n = signed_cell(a) < signed_cell(b) ? FVM_MASK : 0; break;
            default: n = a < b ? FVM_MASK : 0; break;
            }
            fvm_ds[fvm_depth - 1] = n & FVM_MASK;
            break;
        case F_INVERT: case F_ZEROEQ:
            if (!need(1)) break;
            a = fvm_ds[fvm_depth - 1];
            fvm_ds[fvm_depth - 1] = op == F_INVERT ? (~a & FVM_MASK) : (a ? 0 : FVM_MASK);
            break;
        case F_FETCH: case F_CFETCH:
            if (!need(1)) break;
            a = fvm_ds[fvm_depth - 1];
            n = op == F_FETCH ? fvm_read_cell(a) : fvm_read8(a);
            if (!fvm_error) fvm_ds[fvm_depth - 1] = n;
            break;
        case F_STORE: case F_CSTORE:
            if (!need(2)) break;
            a = fvm_ds[fvm_depth - 1]; b = fvm_ds[fvm_depth - 2];
            if (op == F_STORE) fvm_write_cell(a, b);
            else fvm_write8(a, (unsigned char)b);
            if (!fvm_error) fvm_depth -= 2;
            break;
        case F_TOR:
            if (!need(1)) break;
            if (fvm_rdepth >= FVM_RS) { fvm_error = FE_RS_OVER; break; }
            fvm_rs[fvm_rdepth++] = fvm_pop();
            if (fvm_rdepth > fvm_rs_peak) fvm_rs_peak = fvm_rdepth;
            break;
        case F_RFROM: case F_RFETCH:
            if (!fvm_rdepth) { fvm_error = FE_RS_UNDER; break; }
            if (fvm_push(fvm_rs[fvm_rdepth - 1]) && op == F_RFROM) --fvm_rdepth;
            break;
        case F_DO:
            if (!need(2)) break;
            if (fvm_ldepth >= FVM_LOOPS) { fvm_error = FE_LOOP_OVER; break; }
            n = fvm_ldepth++ * 2;
            fvm_loops[n + 1] = fvm_pop(); fvm_loops[n] = fvm_pop();
            break;
        case F_LOOP:
            if (!fvm_ldepth) { fvm_error = FE_LOOP_UNDER; break; }
            n = operand(); if (fvm_error) break;
            a = (fvm_ldepth - 1) * 2;
            b = (fvm_loops[a + 1] + 1) & FVM_MASK;
            fvm_loops[a + 1] = b;
            if (b != fvm_loops[a]) fvm_ip = n;
            else --fvm_ldepth;
            break;
        case F_I:
            if (!fvm_ldepth) fvm_error = FE_LOOP_UNDER;
            else fvm_push(fvm_loops[(fvm_ldepth - 1) * 2 + 1]);
            break;
        case F_DEPTH:
            n = fvm_depth; fvm_push(n);
            break;
        case F_DEFER:
            a = operand();
            if (fvm_error) break;
            n = fvm_read_cell(a);
            if (!fvm_error && fvm_push(n)) return (unsigned char)FVM_TRAP(op);
            break;
        case F_NATIVE:
            n=operand();
            if(!fvm_error && fvm_push(n))return (unsigned char)FVM_TRAP(op);
            break;
        case F_EMIT: case F_DOT: case F_CR: case F_KEY: case F_EXECUTE:
            return (unsigned char)FVM_TRAP(op);
        default:
            fvm_error = FE_OPCODE;
            break;
        }
    }
    return 255;
}

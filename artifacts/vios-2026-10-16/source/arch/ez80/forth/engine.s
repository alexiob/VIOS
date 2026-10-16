/* VIOS 24-bit checked VM. Assemble twice, with TOKEN_ENGINE=1 for tokens.
 * C ABI: IX/IY preserved, byte return in A. IY=IP; IX=next free DS cell.
 * The native stack contains only bounded helper frames/temporary cells.
 * Forth calls live on fvm_rs and never recurse on the native stack.
 */
.assume ADL = 1
.section .text
.extern _fvm_ds, _fvm_rs, _fvm_loops
.extern _fvm_depth, _fvm_rdepth, _fvm_ldepth
.extern _fvm_ip, _fvm_error, _fvm_budget
.extern _fvm_ds_peak, _fvm_rs_peak

.equ OP_COUNT,41
.equ E_DS_UNDER,1
.equ E_DS_OVER,2
.equ E_RS_UNDER,3
.equ E_RS_OVER,4
.equ E_LOOP_UNDER,5
.equ E_LOOP_OVER,6
.equ E_OPCODE,8
.equ E_BUDGET,9

.ifdef TOKEN_ENGINE
.global _fvm_token_run
_fvm_token_run:
.else
.global _fvm_direct_run, _fvm_docol, _fvm_direct_primitives
_fvm_direct_run:
.endif
    push ix
    push iy
    ld hl,0
    add hl,sp
    ld (.Lnative_sp),hl
    ld iy,(_fvm_ip)
    ld hl,(_fvm_error)
    ld de,0
    or a
    sbc hl,de
    jp nz,.Lalready_error
    ld a,E_DS_OVER
    ld hl,(_fvm_depth)
    ld de,257
    or a
    sbc hl,de
    jp nc,.Lfault
    ld a,E_RS_OVER
    ld hl,(_fvm_rdepth)
    ld de,257
    or a
    sbc hl,de
    jp nc,.Lfault
    ld a,E_LOOP_OVER
    ld hl,(_fvm_ldepth)
    ld de,33
    or a
    sbc hl,de
    jp nc,.Lfault
    ld hl,(_fvm_depth)
    push hl
    pop de
    add hl,hl
    add hl,de
    ld de,_fvm_ds
    add hl,de
    push hl
    pop ix
    jp .Lnext

/* All paths, including helper faults, unwind to this run's saved C frame. */
.Lfault:
    ld hl,0
    ld l,a
    ld (_fvm_error),hl
.Lalready_error:
    ld a,255
.Lreturn:
    ld (_fvm_ip),iy
    ld sp,(.Lnative_sp)
    pop iy
    pop ix
    ret
.Ldone:
    xor a
    jp .Lreturn

.Lnext:
    ld hl,(_fvm_budget)
    ld de,0
    or a
    sbc hl,de
    ld a,E_BUDGET
    jp z,.Lfault
    dec hl
    ld (_fvm_budget),hl
.ifdef TOKEN_ENGINE
    ld a,(iy+0)
    inc iy
    cp OP_COUNT
    jp nc,.Lbad_opcode
    ld hl,0
    ld l,a
    push hl
    pop de
    add hl,hl
    add hl,de
    ld de,.Lprimitives
    add hl,de
    ld hl,(hl)
.else
    ld hl,(iy+0)
    lea iy,iy+3
.endif
    jp (hl)
.Lbad_opcode:
    ld a,E_OPCODE
    jp .Lfault

/* Each successful push records transient high-water depth. The profile is
 * deliberately identical in both engines. BC=new depth; preserve HL/DE.
 */
.macro UPDATE_PEAK symbol
    push hl
    push de
    ld hl,(\symbol)
    push bc
    pop de
    or a
    sbc hl,de
    jp nc,.Lpeak_done\@
    ld (\symbol),bc
.Lpeak_done\@:
    pop de
    pop hl
.endm

/* Helpers use BC for depths. Pop returns HL; push accepts/preserves HL.
 * Entry validation bounds all three depths, so subsequent low-word tests
 * are sufficient. Every stored depth remains a native 24-bit cell.
 */
.Lpop:
    ld bc,(_fvm_depth)
    ld a,b
    or c
    jp z,.Lds_under
    dec bc
    ld (_fvm_depth),bc
    lea ix,ix-3
    ld hl,(ix+0)
    ret
.Lpush:
    ld bc,(_fvm_depth)
    ld a,b
    or a
    jp nz,.Lds_over
    ld (ix+0),hl
    lea ix,ix+3
    inc bc
    ld (_fvm_depth),bc
    UPDATE_PEAK _fvm_ds_peak
    ret
.Lrequire2:
    ld bc,(_fvm_depth)
    ld a,b
    or a
    ret nz
    ld a,c
    cp 2
    ret nc
.Lds_under:
    ld a,E_DS_UNDER
    jp .Lfault
.Lds_over:
    ld a,E_DS_OVER
    jp .Lfault
.Lrpop:
    ld bc,(_fvm_rdepth)
    ld a,b
    or c
    jp z,.Lrs_under
    dec bc
    ld (_fvm_rdepth),bc
    push bc
    pop hl
    push hl
    pop de
    add hl,hl
    add hl,de
    ld de,_fvm_rs
    add hl,de
    ld hl,(hl)
    ret
.Lrpush:
    ld bc,(_fvm_rdepth)
    ld a,b
    or a
    jp nz,.Lrs_over
    push hl
    push bc
    pop hl
    push hl
    pop de
    add hl,hl
    add hl,de
    ld de,_fvm_rs
    add hl,de
    pop de
    ld (hl),de
    ex de,hl
    inc bc
    ld (_fvm_rdepth),bc
    UPDATE_PEAK _fvm_rs_peak
    ret
.Lrs_under:
    ld a,E_RS_UNDER
    jp .Lfault
.Lrs_over:
    ld a,E_RS_OVER
    jp .Lfault
.Loperand:
    ld hl,(iy+0)
    lea iy,iy+3
    ret
/* Returns address of the last loop pair (limit first, then index). */
.Lloopaddr:
    ld bc,(_fvm_ldepth)
    ld a,c
    or a
    jp z,.Lloop_under
    dec bc
.Lpairaddr:
    push bc
    pop hl
    push hl
    pop de
    add hl,hl
    add hl,de
    add hl,hl
    ld de,_fvm_loops
    add hl,de
    ret
.Lloop_under:
    ld a,E_LOOP_UNDER
    jp .Lfault

.Lexit:
    ld bc,(_fvm_rdepth)
    ld a,b
    or c
    jp z,.Ldone
    call .Lrpop
    push hl
    pop iy
    jp .Lnext
.Llit:
    call .Loperand
    call .Lpush
    jp .Lnext
.Lcall:
    call .Loperand
    push hl
    push iy
    pop hl
    call .Lrpush
    pop iy
    jp .Lnext
.ifndef TOKEN_ENGINE
/* A colon CFA contains CALL _fvm_docol followed immediately by its thread.
 * The native CALL return address is that thread's initial IP. */
_fvm_docol:
    pop hl
    push hl
    push iy
    pop hl
    call .Lrpush
    pop iy
    jp .Lnext
.endif
.Lbranch:
    ld iy,(iy+0)
    jp .Lnext
.Lzbranch:
    call .Loperand
    push hl
    call .Lpop
    ld de,0
    or a
    sbc hl,de
    pop hl
    jp nz,.Lnext
    push hl
    pop iy
    jp .Lnext
.Ldup:
    call .Lpop
    call .Lpush
    call .Lpush
    jp .Lnext
.Ldrop:
    call .Lpop
    jp .Lnext
.Lswap:
    call .Lrequire2
    ld hl,(ix-3)
    ld de,(ix-6)
    ld (ix-3),de
    ld (ix-6),hl
    jp .Lnext
.Lover:
    call .Lrequire2
    ld hl,(ix-6)
    call .Lpush
    jp .Lnext
/* Binary stack operands: left in HL, right in DE. */
.Lbinary:
    call .Lrequire2
    call .Lpop
    push hl
    call .Lpop
    pop de
    ret
.Ladd:
    call .Lbinary
    add hl,de
    call .Lpush
    jp .Lnext
.Lsub:
    call .Lbinary
    or a
    sbc hl,de
    call .Lpush
    jp .Lnext
/* Bytewise operations cover all 24 bits, including the upper cell byte. */
.macro BITWISE name, instruction
.L\name:
    call .Lrequire2
    ld a,(ix-6)
    \instruction (ix-3)
    ld (ix-6),a
    ld a,(ix-5)
    \instruction (ix-2)
    ld (ix-5),a
    ld a,(ix-4)
    \instruction (ix-1)
    ld (ix-4),a
    call .Lpop
    jp .Lnext
.endm
BITWISE and,and
BITWISE or,or
BITWISE xor,xor
.Linvert:
    call .Lpop
    push hl
    ld hl,0
    add hl,sp
    ld a,(hl)
    cpl
    ld (hl),a
    inc hl
    ld a,(hl)
    cpl
    ld (hl),a
    inc hl
    ld a,(hl)
    cpl
    ld (hl),a
    pop hl
    call .Lpush
    jp .Lnext
.Leq:
    call .Lbinary
    or a
    sbc hl,de
    jp z,.Ltrue
    jp .Lfalse
.Llt:
    call .Lbinary
    or a
    sbc hl,de
    jp po,.Llt_no_overflow
    jp p,.Ltrue
    jp .Lfalse
.Llt_no_overflow:
    jp m,.Ltrue
    jp .Lfalse
.Lult:
    call .Lbinary
    or a
    sbc hl,de
    jp c,.Ltrue
    jp .Lfalse
.Lzeroeq:
    call .Lpop
    ld de,0
    or a
    sbc hl,de
    jp z,.Ltrue
.Lfalse:
    ld hl,0
    call .Lpush
    jp .Lnext
.Ltrue:
    ld hl,0xffffff
    call .Lpush
    jp .Lnext
.Lfetch:
    call .Lpop
    ld hl,(hl)
    call .Lpush
    jp .Lnext
.Lstore:
    call .Lpop
    push hl
    call .Lpop
    pop de
    ex de,hl
    ld (hl),de
    jp .Lnext
.Lcfetch:
    call .Lpop
    ld a,(hl)
    ld hl,0
    ld l,a
    call .Lpush
    jp .Lnext
.Lcstore:
    call .Lpop
    push hl
    call .Lpop
    ld a,l
    pop hl
    ld (hl),a
    jp .Lnext
.Ltor:
    call .Lpop
    call .Lrpush
    jp .Lnext
.Lrfrom:
    call .Lrpop
    call .Lpush
    jp .Lnext
.Lrfetch:
    call .Lrpop
    call .Lrpush
    call .Lpush
    jp .Lnext
.Ldo:
    ld bc,(_fvm_ldepth)
    ld a,c
    cp 32
    jp nc,.Lloop_over
    call .Lpop
    push hl
    call .Lpop
    push hl
    ld bc,(_fvm_ldepth)
    call .Lpairaddr
    pop de
    ld (hl),de
    inc hl
    inc hl
    inc hl
    pop de
    ld (hl),de
    inc bc
    ld (_fvm_ldepth),bc
    jp .Lnext
.Lloop_over:
    ld a,E_LOOP_OVER
    jp .Lfault
.Lloop:
    call .Loperand
    push hl
    call .Lloopaddr
    ld de,(hl)
    inc hl
    inc hl
    inc hl
    ld bc,(hl)
    inc bc
    ld (hl),bc
    push hl
    push bc
    pop hl
    or a
    sbc hl,de
    pop hl
    pop hl
    jp z,.Lloop_finished
    push hl
    pop iy
    jp .Lnext
.Lloop_finished:
    ld hl,(_fvm_ldepth)
    dec hl
    ld (_fvm_ldepth),hl
    jp .Lnext
.Li:
    call .Lloopaddr
    inc hl
    inc hl
    inc hl
    ld hl,(hl)
    call .Lpush
    jp .Lnext
.Ldefer:
    call .Loperand
    ld hl,(hl)
    call .Lpush
    ld a,0x80+35
    jp .Lreturn
.Ldepth:
    ld hl,(_fvm_depth)
    call .Lpush
    jp .Lnext
/* Services resume through the C dispatcher with IP and depths published. */
.macro TRAP name, opcode
.L\name:
    ld a,0x80+\opcode
    jp .Lreturn
.endm
TRAP mul,11
TRAP emit,24
TRAP dot,25
TRAP cr,26
TRAP key,27
TRAP execute,34
TRAP div,36
TRAP mod,37
TRAP lshift,38
TRAP rshift,39

.ifndef TOKEN_ENGINE
_fvm_direct_primitives:
.endif
.Lprimitives:

    .d24 .Lexit
    .d24 .Llit
    .d24 .Lcall
    .d24 .Lbranch
    .d24 .Lzbranch
    .d24 .Ldup
    .d24 .Ldrop
    .d24 .Lswap
    .d24 .Lover
    .d24 .Ladd
    .d24 .Lsub
    .d24 .Lmul
    .d24 .Land
    .d24 .Lor
    .d24 .Lxor
    .d24 .Linvert
    .d24 .Leq
    .d24 .Llt
    .d24 .Lult
    .d24 .Lzeroeq
    .d24 .Lfetch
    .d24 .Lstore
    .d24 .Lcfetch
    .d24 .Lcstore
    .d24 .Lemit
    .d24 .Ldot
    .d24 .Lcr
    .d24 .Lkey
    .d24 .Ltor
    .d24 .Lrfrom
    .d24 .Lrfetch
    .d24 .Ldo
    .d24 .Lloop
    .d24 .Li
    .d24 .Lexecute
    .d24 .Ldefer
    .d24 .Ldiv
    .d24 .Lmod
    .d24 .Llshift
    .d24 .Lrshift
    .d24 .Ldepth

.section .bss
.Lnative_sp:
    .space 3

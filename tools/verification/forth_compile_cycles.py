#!/usr/bin/env python3
"""Estimate Forth compile and call cost by running the linked eZ80 code.

Measurements behind docs/superpowers/specs/2026-10-21-vios-extensible-compiler.md:

  compile  forth_eval of a prelude-like source (201 tokens) after forth_init:
           cycles per token, and a per-function profile (approximate: the
           profile attributes cycles by function start address, so tail calls
           and shared epilogues land on a neighbour; compare build totals).
  calls    per-iteration cost on the TOKEN and DIRECT engines of
           T1 inline `V @` (what a VARIABLE/CREATE word compiles to),
           T2 a colon word `V @` (the cost of one extra nest),
           T3 `V DP` where DP is `@`: the proposed DOES> child layout
              (LIT body, then a call into the does-part).

Same cost model as compose_cycles.py (one cycle per bus byte at 18.432 MHz,
~6% below the board on its calibration). DIRECT executes the `CALL _fvm_docol`
at each colon word's code field, which lives in BSS and is absent from the
ELF disassembly: it is decoded from memory here. No board is touched.

  .venv/bin/python tools/verification/forth_compile_cycles.py [ELF ...]
"""
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT/'tests'), str(ROOT/'tools'), str(ROOT/'tools/verification')]
from compose_cycles import CycleCPU, TargetImage, HZ, MASK  # noqa: E402

SOURCE = (
    ": > SWAP < ; : 0< 0 < ; : 0> 0 SWAP < ; : <> = 0= ; : U> SWAP U< ; "
    ": ROT >R SWAP R> SWAP ; : -ROT ROT ROT ; : NIP SWAP DROP ; : TUCK SWAP OVER ; "
    ": ?DUP DUP IF DUP THEN ; : ABS DUP 0< IF NEGATE THEN ; "
    ": MAX 2DUP < IF SWAP THEN DROP ; : MIN 2DUP SWAP < IF SWAP THEN DROP ; "
    ": 2SWAP ROT >R ROT R> ; : 2OVER >R >R 2DUP R> R> 2SWAP ; "
    ": +! DUP @ ROT + SWAP ! ; : 2* 1 LSHIFT ; : BL 32 ; : SPACE BL EMIT ; "
    ": SPACES BEGIN DUP 0> WHILE SPACE 1- REPEAT DROP ; "
    ": COUNT DUP 1+ SWAP C@ ; : WITHIN OVER - >R - R> U< ; "
    ": ? @ . ; : ERASE 0 FILL ; : /MOD 2DUP MOD -ROT / ; : */ >R * R> / ; : BOUNDS OVER + SWAP ; "
    "VARIABLE CNT 10 CONSTANT TEN : BUMP CNT @ 1+ CNT ! ; : RUN TEN 0 DO BUMP I DROP LOOP ; ")
CALLS = ("VARIABLE V 7 V ! : DP @ ; : CHILD V DP ; : CHILD2 V @ ; "
         ": T0 1000 0 DO LOOP ; : T1 1000 0 DO V @ DROP LOOP ; "
         ": T2 1000 0 DO CHILD2 DROP LOOP ; : T3 1000 0 DO CHILD DROP LOOP ;")
TEXT = 0x9a000


class ThreadedCPU(CycleCPU):
    def decode(self, pc):
        if self.mem[pc] == 0xcd:  # CALL _fvm_docol at a colon word's code field (BSS)
            self.ins[pc] = (4, 'call 0x%x' % int.from_bytes(self.mem[pc+1:pc+4], 'little'))
            return
        super().decode(pc)


def fresh(elf):
    cpu = ThreadedCPU(TargetImage(Path(elf)), Path(elf))
    cpu.r['sp'] = 0xaeff0
    cpu.call('_forth_init', (), budget=10**8)
    return cpu


def evaluate(cpu, source, profile=False):
    cpu.mem[TEXT:TEXT+len(source)+1] = source.encode()+b'\0'
    cpu.cycles, cpu.profile = 0, {} if profile else None
    ok = cpu.call('_forth_eval', (TEXT,), budget=10**9) & MASK
    assert ok == 1, source[:40]
    return cpu.cycles


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('elf', nargs='*', default=[str(ROOT/'build/vios-resident.elf')])
    for elf in parser.parse_args().elf:
        print(elf)
        cpu = fresh(elf)
        tokens = len(SOURCE.split())
        cycles = evaluate(cpu, SOURCE, profile=True)
        print(f'  compile {tokens} tokens {cycles} cycles {cycles/HZ*1e3:.2f} ms '
              f'{cycles/tokens:.0f} cycles/token {cycles/tokens/HZ*1e6:.1f} us/token')
        total = sum(cpu.profile.values())
        for cost, name in sorted(((v, k) for k, v in cpu.profile.items()), reverse=True)[:8]:
            print(f'      {name:24s} {cost:9d} {100*cost/total:5.1f}%')
        cpu = fresh(elf)
        evaluate(cpu, CALLS)
        for engine, label in ((2, 'DIRECT'), (1, 'TOKEN')):
            cpu.call('_forth_set_engine', (engine,), budget=10**6)
            empty = evaluate(cpu, 'T0')
            costs = [(evaluate(cpu, t)-empty)/1000 for t in ('T1', 'T2', 'T3')]
            print(f'  {label:6s} cycles/call  inline {costs[0]:.0f}  colon {costs[1]:.0f}  does-child {costs[2]:.0f}')


if __name__ == '__main__':
    main()

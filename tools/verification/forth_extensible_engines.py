#!/usr/bin/env python3
"""Run the extensible compiler's code shapes on the linked eZ80 engines.

The host tests run only the C reference engine. This runs the resident image
(build/vios-resident.elf) under the semantic eZ80 interpreter of
tests/test_vios_native_c.py and checks that DIRECT, TOKEN and C agree on:
DOES> children (called, inlined, EXECUTEd), POSTPONEd control flow,
[ ] LITERAL, COMPILE, and EVALUATE nested inside running words, including
loop and return stacks. DIRECT executes the CALL _fvm_docol at each colon
code field and at each does-part, which live in BSS: they are decoded from
memory. No board is touched.

  .venv/bin/python tools/verification/forth_extensible_engines.py [ELF]
"""
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT/'tools/verification')]
from forth_compile_cycles import ThreadedCPU, TargetImage, MASK  # noqa: E402

DEFINITIONS = (
    ": CONST CREATE , DOES> @ ; 42 CONST ANSWER "
    ": FIELD CREATE , DOES> @ + ; 6 FIELD .Y "
    ": ARRAY CREATE 3 * ALLOT DOES> SWAP 3 * + ; 4 ARRAY AR "
    ": UNLESS POSTPONE 0= POSTPONE IF ; IMMEDIATE "
    ": C-DUP ['] DUP COMPILE, ; IMMEDIATE "
    ": SQ S\" DUP *\" EVALUATE ; IMMEDIATE ")
CASES = [
    ('ANSWER', 42),
    (': USE ANSWER 1 + ; USE', 43),
    ("' ANSWER EXECUTE", 42),
    ("' ANSWER >BODY @", 42),
    ('100 .Y', 106),
    ('99 2 AR ! 2 AR @ 1 AR @ +', 99),
    (': T UNLESS 1 ELSE 2 THEN ; 0 T 10 * 5 T +', 12),
    (': FOLD [ 2 3 + ] LITERAL ; FOLD', 5),
    (': Q 3 C-DUP + ; Q', 6),
    (': CUBE DUP SQ * ; 3 CUBE', 27),
    (': OUTER 1 S" 2" EVALUATE + 10 * ; OUTER', 30),
    (': LP 0 3 0 DO S" I" EVALUATE + LOOP ; LP', 3),
    (': DEEP ANSWER S" ANSWER" EVALUATE + ; DEEP', 84),
]
TEXT = 0x9a000


def evaluate(cpu, source):
    cpu.mem[TEXT:TEXT+len(source)+1] = source.encode()+b'\0'
    return cpu.call('_forth_eval', (TEXT,), budget=10**9) & MASK


def pop(cpu):
    return cpu.call('_fvm_pop', (), budget=10**6) & MASK


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('elf', nargs='?', default=str(ROOT/'build/vios-resident.elf'))
    elf = Path(parser.parse_args().elf)
    failures = 0
    for engine, label in ((2, 'DIRECT'), (1, 'TOKEN'), (0, 'C')):
        cpu = ThreadedCPU(TargetImage(elf), elf)
        cpu.r['sp'] = 0xaeff0
        cpu.call('_forth_init', (), budget=10**8)
        cpu.call('_forth_set_engine', (engine,), budget=10**6)
        assert evaluate(cpu, DEFINITIONS) == 1, label
        for source, expected in CASES:
            ok = evaluate(cpu, source)
            got = pop(cpu) if ok else None
            if got != expected:
                failures += 1
                print(f'{label:6s} FAIL {source!r}: got {got} expected {expected}')
        print(f'{label:6s} {len(CASES)} cases checked')
    print('engines agree' if not failures else f'{failures} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())

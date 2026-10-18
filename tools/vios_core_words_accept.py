#!/usr/bin/env python3
"""Core-word differential acceptance on the Agon: DIRECT, TOKEN and C engines.

Each case leaves one value; every engine must print the expected number.
Defines temporary RAM words; restores ENGINE DIRECT. No SD writes.
"""
import argparse
import json
from pathlib import Path
import time
try:
    from .vios_cli import RPCClient
except ImportError:
    from vios_cli import RPCClient

CASES = [('3 2 >', -1), ('2 3 >', 0), ('-1 0<', -1), ('5 0>', -1), ('4 4 <>', 0), ('1 2 3 ROT', 1),
         ('1 2 3 -ROT DROP DROP', 3), ('1 2 NIP', 2), ('-7 ABS', 7), ('3 9 MAX', 9), ('-5 2 MIN', -5),
         ('1 2 3 4 2SWAP DROP', 1), ('1 2 3 4 2OVER + + + + +', 13), ('21 2*', 42), ('5 0 10 WITHIN', -1),
         ('10 0 10 WITHIN', 0), ('CHAR a', 97), ('ACC-T @ ACC-T CELL+ @ +', 16), ('S" xyz" DROP COUNT NIP', 120),
         ('ACC-L1', 63), ('ACC-L2', 42), ('ACC-L3', 4), ('ACC-L4', 0), ('17 5 /MOD DROP', 2), ('6 7 3 */', 14),
         ('ACC-M', 65)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    r = {'status': 'running', 'engines': {}}
    try:
        with RPCClient(timeout=120) as c:
            c.initialize()

            def ev(s):
                out = c.tool('evaluate', {'source': s, 'timeout': 60})
                if not out['ok']:
                    raise AssertionError(f'{s!r}: {out["output"][-200:]}')
                return out['output']
            ev('CREATE ACC-T 7 , 9 ,')
            ev(': ACC-G ." core-" [CHAR] ! EMIT ;')
            # Loop words: nested J sum, +LOOP up/down, LEAVE, ?DO skip; MOVE/FILL.
            ev(': ACC-L1 0 3 0 DO 2 0 DO J 10 * I + + LOOP LOOP ;')
            ev(': ACC-L2 0 10 0 DO I + 2 +LOOP 0 10 DO I + -3 +LOOP ;')
            ev(': ACC-L3 0 10 0 DO I 4 = IF LEAVE THEN 1+ LOOP ;')
            ev(': ACC-L4 0 5 5 ?DO 1+ LOOP ;')
            ev('CREATE ACC-B 8 ALLOT : ACC-M ACC-B 8 65 FILL ACC-B ACC-B 1+ 3 MOVE ACC-B 3 + C@ ;')
            for engine in ('DIRECT', 'TOKEN', 'C'):
                ev('ENGINE ' + engine)
                results = {}
                t = time.perf_counter()
                for src, expect in CASES:
                    got = int(ev(src + ' .').split('\r\n')[1].split()[-1])
                    results[src] = got
                    if got != expect:
                        raise AssertionError(f'{engine}: {src} -> {got}, expected {expect}')
                greet = ev('ACC-G CR').split('\r\n')[1]
                if greet != 'core-!':
                    raise AssertionError(f'{engine}: ."/[CHAR] printed {greet!r}')
                r['engines'][engine] = {'cases': len(results), 'seconds': round(time.perf_counter() - t, 2)}
            ev('ENGINE DIRECT')
        r['status'] = 'passed'
    except BaseException as error:
        r.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        a.output.write_text(json.dumps(r, indent=2) + '\n')
        print(json.dumps(r))


if __name__ == '__main__':
    main()

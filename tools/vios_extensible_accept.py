#!/usr/bin/env python3
"""Extensible-compiler acceptance on the Agon (docs/superpowers/specs/2026-10-21-
vios-extensible-compiler.md): DIRECT, TOKEN and C agree on DOES> children
(called, inlined, EXECUTEd), POSTPONE, [ ] LITERAL, COMPILE, and EVALUATE
nested in running words; EVALUATE four deep; a defining word is refused in a
background task; MARKER removes the test words and gives every pool back.

Everything is defined after a MARKER and removed by it. Restores ENGINE
DIRECT. No SD writes.
"""
import argparse
import json
from pathlib import Path
import re
import time
try:
    from .vios_cli import RPCClient
    from .vios_accept_autorun import pause_autorun, resume_autorun
except ImportError:
    from vios_cli import RPCClient
    from vios_accept_autorun import pause_autorun, resume_autorun

DEFINITIONS = [
    ': XC-CONST CREATE , DOES> @ ; 42 XC-CONST XC-ANSWER',
    ': XC-FIELD CREATE , DOES> @ + ; 6 XC-FIELD XC-.Y',
    ': XC-ARRAY CREATE CELLS ALLOT DOES> SWAP CELLS + ; 4 XC-ARRAY XC-AR',
    ': XC-UNLESS POSTPONE 0= POSTPONE IF ; IMMEDIATE',
    ': XC-CDUP [\'] DUP COMPILE, ; IMMEDIATE',
    ': XC-SQ S" DUP *" EVALUATE ; IMMEDIATE',
    ': XC-USE XC-ANSWER 1+ ;',
    ': XC-T XC-UNLESS 1 ELSE 2 THEN ;',
    ': XC-FOLD [ 2 3 + ] LITERAL ;',
    ': XC-Q 3 XC-CDUP + ;',
    ': XC-CUBE DUP XC-SQ * ;',
    ': XC-OUTER 1 S" 2" EVALUATE + 10 * ;',
    ': XC-LP 0 3 0 DO S" I" EVALUATE + LOOP ;',
    ': XC-E1 S" 1" EVALUATE ; : XC-E2 S" XC-E1" EVALUATE ; : XC-E3 S" XC-E2" EVALUATE ; : XC-E4 S" XC-E3" EVALUATE ;',
    ': XC-MAKER CREATE ;',
]
CASES = [('XC-ANSWER', 42), ('XC-USE', 43), ("' XC-ANSWER EXECUTE", 42), ("' XC-ANSWER >BODY @", 42),
         ('100 XC-.Y', 106), ('99 2 XC-AR ! 2 XC-AR @', 99), ('0 XC-T 10 * 5 XC-T +', 12), ('XC-FOLD', 5),
         ('XC-Q', 6), ('3 XC-CUBE', 27), ('XC-OUTER', 30), ('XC-LP', 3), ('XC-E4', 1), ('STATE @', 0)]


def resources(text):
    m = re.search(r'words (\d+)/\d+ token-bytes (\d+) direct-bytes (\d+) data-bytes (\d+) source-bytes (\d+)', text)
    if not m:
        raise AssertionError(f'no RESOURCES line in {text!r}')
    return dict(zip(('words', 'token', 'direct', 'data', 'source'), map(int, m.groups())))


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    r = {'status': 'running', 'engines': {}}
    try:
        with RPCClient(timeout=120) as c:
            c.initialize()
            c.tool('connect', {})
            r['autorun_paused'] = pause_autorun(c)   # ENGINE and markers are refused while tasks live

            def ev(s, ok=True):
                out = c.tool('evaluate', {'source': s, 'timeout': 60})
                if out['ok'] != ok:
                    raise AssertionError(f'{s!r} ok={out["ok"]}: {out["output"][-200:]}')
                return out['output']
            before = resources(ev('RESOURCES'))
            ev('MARKER XC-MARK')
            t = time.perf_counter()
            for d in DEFINITIONS:
                ev(d)
            r['define_seconds'] = round(time.perf_counter() - t, 2)
            for engine in ('DIRECT', 'TOKEN', 'C'):
                ev('ENGINE ' + engine)
                t = time.perf_counter()
                for src, expect in CASES:
                    got = int(ev(src + ' .').split('\r\n')[1].split()[-1])
                    if got != expect:
                        raise AssertionError(f'{engine}: {src} -> {got}, expected {expect}')
                r['engines'][engine] = {'cases': len(CASES), 'seconds': round(time.perf_counter() - t, 2)}
            ev('ENGINE DIRECT')
            nested = ev(': XC-E5 S" XC-E4" EVALUATE ; XC-E5', ok=False)
            if 'EVALUATE nested too deep' not in nested:
                raise AssertionError(f'fifth EVALUATE level: {nested[-200:]}')
            r['evaluate_depth_limit'] = 'refused at 5'
            # A defining word in a background task: the task fails, the dictionary is unchanged.
            ev("' XC-MAKER SPAWN DROP 200 MS")
            tasks = ev('TASKS')
            if 'dictionary word unavailable in background task' not in bytes.fromhex(''.join(re.findall(r'error ([0-9A-F]+)', tasks))).decode(errors='replace'):
                raise AssertionError(f'task refusal not reported: {tasks[-400:]}')
            r['task_refusal'] = 'passed'
            ev('XC-MARK')
            after = resources(ev('RESOURCES'))
            if after != before:
                raise AssertionError(f'MARKER left {after}, expected {before}')
            ev('XC-ANSWER', ok=False)
            r['marker'] = {'restored': after}
        r['status'] = 'passed'
    except BaseException as error:
        r.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        resume_autorun(r.get('autorun_paused'))
        a.output.write_text(json.dumps(r, indent=2) + '\n')
        print(json.dumps(r))


if __name__ == '__main__':
    main()

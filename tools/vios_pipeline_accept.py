#!/usr/bin/env python3
"""Task pipeline acceptance on the Agon (producer stdout -> consumer KEY).

Temporary RAM words; all tasks it starts finish or fail. No SD writes.
"""
import argparse
import json
from pathlib import Path
import re
import time
try:
    from .vios_cli import RPCClient
except ImportError:
    from vios_cli import RPCClient


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--bytes', type=int, default=5000)
    a = p.parse_args()
    r = {'status': 'running', 'checks': {}}

    def check(name, ok, detail=None):
        r['checks'][name] = {'ok': bool(ok), 'detail': detail}
        if not ok:
            raise AssertionError(f'{name}: {detail}')
    try:
        with RPCClient(timeout=150) as c:
            c.initialize()

            def ev(s):
                out = c.tool('evaluate', {'source': s, 'timeout': 60})
                if not out['ok']:
                    raise AssertionError(f'{s!r}: {out["output"][-200:]}')
                return out['output']
            val = lambda s: int(ev(s + ' .').split('\r\n')[-4].split()[-1])

            seen = []

            def wait_idle(limit=120):
                deadline = time.monotonic() + limit
                while time.monotonic() < deadline:
                    listed = c.tool('tasks')
                    seen.append(listed.get('output', ''))  # console-task output arrives here
                    if not any(t['state'] in ('ready', 'sleeping') for t in listed['tasks']):
                        return True
                    time.sleep(0.25)
                return False
            drops = lambda: int(re.search(r'console-dropped (\d+)', ev('VDP-INFO'))[1])
            r['dropped_before'] = drops()
            ev(': PSAY S" hello pipes" TYPE CR ; : PUP BEGIN KEY DUP -1 <> WHILE DUP 97 123 WITHIN IF 32 - THEN EMIT REPEAT DROP ;')
            ev("' PSAY ' PUP PIPELINE 2DROP")
            check('upper_done', wait_idle(30))
            seen.append(ev('VDP-INFO'))
            text = ''.join(seen)
            check('upper_output_on_console', 'HELLO PIPES' in text, text[-300:])
            ev(f'VARIABLE PGOT : PBIG {a.bytes} 0 DO 65 EMIT LOOP ; : PCNT 0 PGOT ! BEGIN KEY -1 <> WHILE 1 PGOT +! REPEAT ;')
            t = time.perf_counter()
            ev("' PBIG ' PCNT PIPELINE 2DROP")
            check('bulk_done', wait_idle(300))
            elapsed = time.perf_counter() - t
            got = int(ev('PGOT @ .').split('\r\n')[1].split()[0])
            r['bulk'] = {'bytes': got, 'seconds': round(elapsed, 2), 'bytes_per_s': round(got / elapsed)}
            check('bulk_exact', got == a.bytes, got)
            tasks = c.tool('tasks')['tasks']
            check('no_output_drops', all(t['output_dropped'] == 0 for t in tasks), tasks)
            ev(': PQUIT ; ' + "' PBIG ' PQUIT PIPELINE 2DROP")
            check('quit_done', wait_idle(60))
            tasks = c.tool('tasks')['tasks']
            check('broken_pipe_reported', any(t['state'] == 'failed' and t['error'] == 'broken pipe' for t in tasks), tasks)
            handles = c.tool('handles')['handles']
            check('no_leaked_handles', handles == [], handles)
            r['dropped_after'] = drops()
            check('no_console_drops', r['dropped_after'] == r['dropped_before'], (r['dropped_before'], r['dropped_after']))
        r['status'] = 'passed'
    except BaseException as error:
        r.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        a.output.write_text(json.dumps(r, indent=2) + '\n')
        print(json.dumps({k: r.get(k) for k in ('status', 'bulk', 'error')}))


if __name__ == '__main__':
    main()

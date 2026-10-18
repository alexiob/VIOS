#!/usr/bin/env python3
"""Stage 3 handle/pipe/device acceptance on the Agon through the shared owner.

Defines temporary RAM words, writes and then removes /VIOS/IOTEST.TXT on SD
(no module, package or boot-slot files are touched), and stops its tasks.
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

MASK = 0xffffff


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    r = {'status': 'running', 'checks': {}}

    def check(name, ok, detail=None):
        r['checks'][name] = {'ok': bool(ok), 'detail': detail}
        if not ok:
            raise AssertionError(f'{name}: {detail}')
    try:
        with RPCClient(timeout=150) as c:
            c.initialize()
            def ev(s, t=60):
                out = c.tool('evaluate', {'source': s, 'timeout': t})
                if s.startswith((':', 'S"')) or 'CONSTANT' in s or 'VARIABLE' in s:
                    assert out['ok'], out['output']
                return out

            def val(s):
                out = ev(s + ' .')
                assert out['ok'], out['output']
                return int(out['output'].split('\r\n')[1].split()[-1])
            signed = lambda v: v - (1 << 24) if v & 0x800000 else v
            drops = lambda: int(re.search(r'console-dropped (\d+)', ev('VDP-INFO')['output'])[1])
            r['dropped_before'] = drops()
            ev('S" ................................................................" DROP CONSTANT IOB VARIABLE IOH VARIABLE IOR VARIABLE IOW VARIABLE IOSUM VARIABLE IOCNT')
            # SD file through a handle: 3 x 64-byte writes, chunked read back.
            ev('S" /VIOS/IOTEST.TXT" 2 OPEN IOH !')
            check('file_open_write', val('IOH @') >= 1)
            t = time.perf_counter()
            check('file_write_all', all(val('IOB 64 IOH @ WRITE-ALL') == 0 for _ in range(3)))
            check('file_close', val('IOH @ CLOSE') == 0)
            ev('S" /VIOS/IOTEST.TXT" 1 OPEN IOH !')
            chunks = [val('IOB 50 IOH @ READ') for _ in range(5)]
            r['file_ms'] = round((time.perf_counter() - t) * 1000)
            check('file_chunked_read', chunks == [50, 50, 50, 42, 0], chunks)
            check('file_reread_close', val('IOH @ CLOSE') == 0)
            check('file_removed', val('S" /VIOS/IOTEST.TXT" FILE-REMOVE') == 0)
            check('missing_file_error', val('S" /VIOS/IOTEST.TXT" 1 OPEN') == -5)
            # Producer/consumer tasks over a 128-byte pipe; the producer's exit
            # closes its write end, so the consumer sees EOF and finishes.
            ev(': IOP PIPE IOW ! IOR ! ;')
            ev(': PROD 200 0 DO IOB 50 IOW @ WRITE-ALL DROP LOOP IOW @ CLOSE DROP ;')
            ev(': CONS BEGIN IOB 64 IOR @ READ-WAIT DUP 0 SWAP < WHILE IOCNT @ + IOCNT ! REPEAT DROP IOR @ CLOSE DROP ;')
            ev('0 IOCNT ! IOP')
            t = time.perf_counter()
            ids = [int(x) for x in ev("' CONS SPAWN . ' PROD SPAWN .")['output'].split('\r\n')[1].split()]
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline:
                states = {x['id']: x for x in c.tool('tasks')['tasks']}
                if all(states[i]['state'] in ('done', 'failed', 'killed') for i in ids):
                    break
                time.sleep(0.5)
            elapsed = time.perf_counter() - t
            states = {x['id']: x for x in c.tool('tasks')['tasks']}
            total = val('IOCNT @')
            r['pipe'] = {'bytes': total, 'seconds': round(elapsed, 2), 'bytes_per_s': round(total / elapsed),
                         'tasks': [states[i] for i in ids]}
            check('pipe_tasks_done', all(states[i]['state'] == 'done' for i in ids), r['pipe']['tasks'])
            check('pipe_bytes_exact', total == 10000, total)
            # /dev/tasks snapshot and handle cleanup.
            ev('S" /dev/tasks" 1 OPEN IOH !')
            n = val('IOB 64 IOH @ READ')
            check('dev_tasks_readable', n > 0, n)
            check('dev_tasks_close', val('IOH @ CLOSE') == 0)
            listed = c.tool('handles')
            r['handles_after'] = listed['handles']
            check('no_leaked_handles', listed['handles'] == [], listed['handles'])
            r['dropped_after'] = drops()
            check('no_console_drops', r['dropped_after'] == r['dropped_before'], (r['dropped_before'], r['dropped_after']))
        r['status'] = 'passed'
    except BaseException as error:
        r.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        a.output.write_text(json.dumps(r, indent=2) + '\n')
        print(json.dumps({k: r.get(k) for k in ('status', 'file_ms', 'pipe', 'error')}, indent=1)[:2000])


if __name__ == '__main__':
    main()

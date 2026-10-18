#!/usr/bin/env python3
"""Mixed-load soak on the Agon through the shared owner.

Each iteration: REPL arithmetic, a structured snapshot, a 1000-byte task
pipeline, an editor <-> console round trip, and every tenth iteration an SD
file round trip (/VIOS/SOAKTEST.TXT, removed again). Records per-iteration
latency and stops at the first failure without retrying it. Temporary RAM
words; no module, package or boot-slot changes.
"""
import argparse
import json
from pathlib import Path
import re
import statistics
import time
try:
    from .vios_cli import RPCClient
except ImportError:
    from vios_cli import RPCClient


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--minutes', type=float, default=45)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    r = {'status': 'running', 'iterations': 0, 'latency_ms': [], 'failures': []}
    started = time.time()

    def save():
        lat = r['latency_ms']
        r['summary'] = {'iterations': r['iterations'], 'minutes': round((time.time() - started) / 60, 1),
                        'iteration_ms_median': round(statistics.median(lat)) if lat else None,
                        'iteration_ms_max': max(lat) if lat else None}
        a.output.write_text(json.dumps({k: v for k, v in r.items() if k != 'latency_ms'} | {'latency_ms_tail': lat[-50:]}, indent=2) + '\n')
    try:
        with RPCClient(timeout=200) as c:
            c.initialize()

            def ev(s, t=120):
                out = c.tool('evaluate', {'source': s, 'timeout': t})
                if not out['ok']:
                    raise AssertionError(f'{s!r}: {out["output"][-200:]}')
                return out['output']
            drops = lambda: int(re.search(r'console-dropped (\d+)', ev('VDP-INFO'))[1])
            r['dropped_before'] = drops()
            ev('VARIABLE SKG : SKP 1000 0 DO 66 EMIT LOOP ; : SKC 0 SKG ! BEGIN KEY -1 <> WHILE 1 SKG +! REPEAT ;')
            ev('S" ................................................................" DROP CONSTANT SKB VARIABLE SKH')
            deadline = time.monotonic() + a.minutes * 60
            while time.monotonic() < deadline:
                t = time.perf_counter()
                i = r['iterations']
                if '42 ' not in ev('6 7 * .'):
                    raise AssertionError('arithmetic result missing')
                snap = c.tool('snapshot')
                if not snap.get('ok'):
                    raise AssertionError('snapshot failed')
                ev("' SKP ' SKC PIPELINE 2DROP")
                for _ in range(200):
                    if not any(x['state'] in ('ready', 'sleeping') for x in c.tool('tasks')['tasks']):
                        break
                    time.sleep(0.2)
                else:
                    raise AssertionError('pipeline did not finish')
                got = int(ev('SKG @ .').split('\r\n')[1].split()[0])
                if got != 1000:
                    raise AssertionError(f'pipeline delivered {got} bytes')
                ev('WB-OPEN')
                ev('WB-CLOSE')
                if i % 10 == 0:
                    ev('S" /VIOS/SOAKTEST.TXT" 2 OPEN SKH !')
                    if '0 ' not in ev('SKB 64 SKH @ WRITE-ALL . SKH @ CLOSE .'):
                        raise AssertionError('SD write failed')
                    ev('S" /VIOS/SOAKTEST.TXT" 1 OPEN SKH !')
                    if '64 ' not in ev('SKB 64 SKH @ READ .'):
                        raise AssertionError('SD read failed')
                    ev('SKH @ CLOSE DROP S" /VIOS/SOAKTEST.TXT" FILE-REMOVE DROP')
                if c.tool('handles')['handles']:
                    raise AssertionError('handle leak')
                r['latency_ms'].append(round((time.perf_counter() - t) * 1000))
                r['iterations'] += 1
                if r['iterations'] % 20 == 0:
                    r['dropped_now'] = drops()
                    save()
                    print(r['summary'], 'drops', r['dropped_now'], flush=True)
            r['dropped_after'] = drops()
            if r['dropped_after'] != r['dropped_before']:
                raise AssertionError(f'console drops {r["dropped_before"]} -> {r["dropped_after"]}')
        r['status'] = 'passed'
    except BaseException as error:
        r.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        save()
        print(json.dumps({'status': r['status'], 'summary': r['summary'], 'error': r.get('error')}))


if __name__ == '__main__':
    main()

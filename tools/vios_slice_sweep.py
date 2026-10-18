#!/usr/bin/env python3
"""Measure background-task slice sizes on the Agon (REPL latency vs task
throughput and MS wake accuracy). Temporary RAM words; restores 2048."""
import argparse
import json
from pathlib import Path
import statistics
import time
try:
    from .vios_cli import RPCClient
except ImportError:
    from vios_cli import RPCClient


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--slices', default='128,256,512,1024,2048,4096')
    a = p.parse_args()
    rows = []
    with RPCClient(timeout=150) as c:
        c.initialize()

        def ev(s):
            out = c.tool('evaluate', {'source': s, 'timeout': 60})
            if not out['ok']:
                raise AssertionError(out['output'][-200:])
            return out['output']
        val = lambda s: int(ev(s + ' .').split('\r\n')[1].split()[-1])
        ev('VARIABLE SA VARIABLE SB : SCA BEGIN SA @ 1+ SA ! AGAIN ; : SCB BEGIN SB @ 1+ SB ! 10 MS AGAIN ;')
        ids = [int(x) for x in ev("' SCA SPAWN . ' SCB SPAWN .").split('\r\n')[1].split()]
        try:
            for n in [int(x) for x in a.slices.split(',')]:
                ev(f'TASK-SLICE {n}')
                time.sleep(0.5)
                t0, a0, b0 = time.perf_counter(), val('SA @'), val('SB @')
                lat = []
                for _ in range(15):
                    t = time.perf_counter(); ev('1 2 + .'); lat.append((time.perf_counter() - t) * 1000)
                time.sleep(2)
                t1, a1, b1 = time.perf_counter(), val('SA @'), val('SB @')
                span = t1 - t0
                rows.append({'slice': n, 'latency_median_ms': round(statistics.median(lat), 1), 'latency_max_ms': round(max(lat), 1),
                             'busy_per_s': round(((a1 - a0) & 0xffffff) / span), 'sleeper_wakes_per_s': round((b1 - b0) / span, 1)})
                print(rows[-1], flush=True)
        finally:
            ev(' '.join(f'{i} TASK-KILL' for i in ids) + ' TASK-SLICE 1024')
    a.output.write_text(json.dumps({'rows': rows}, indent=2) + '\n')


if __name__ == '__main__':
    main()

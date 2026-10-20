#!/usr/bin/env python3
"""Stage 2 background-task acceptance on the Agon through the shared owner.

Defines temporary RAM words (TA/TB/TCA/TCB/TPR/TKK), so preserve live work
first; kills every task it starts. No SD writes, module or boot changes.
"""
import argparse
import json
from pathlib import Path
import re
import statistics
import time
try:
    from .vios_cli import RPCClient
    from .vios_accept_autorun import pause_autorun, resume_autorun
except ImportError:
    from vios_cli import RPCClient
    from vios_accept_autorun import pause_autorun, resume_autorun


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--samples', type=int, default=30)
    a = p.parse_args()
    r = {'status': 'running', 'checks': {}}

    def check(name, ok, detail=None):
        r['checks'][name] = {'ok': bool(ok), 'detail': detail}
        if not ok:
            raise AssertionError(f'{name}: {detail}')
    try:
        with RPCClient(timeout=150) as c:
            c.initialize()
            ev = lambda s, t=30: c.tool('evaluate', {'source': s, 'timeout': t})
            val = lambda s: int(re.search(r'\r\n(-?\d+) \r\n', ev(s + ' .')['output'])[1])

            def latency():
                times = []
                for _ in range(a.samples):
                    t = time.perf_counter()
                    out = ev('1 2 + .')
                    times.append((time.perf_counter() - t) * 1000)
                    assert '3 ' in out['output']
                return {'median_ms': round(statistics.median(times), 2), 'max_ms': round(max(times), 2)}
            r['connect'] = {k: v for k, v in c.tool('connect').items() if k in ('connected', 'identity')}
            r['autorun_paused'] = pause_autorun(c)   # HELLO's heartbeat would count as a live task
            tasks = c.tool('tasks')['tasks']
            check('no_live_tasks_before', not any(t['state'] in ('ready', 'sleeping') for t in tasks), tasks)
            drops = lambda: int(re.search(r'console-dropped (\d+)', ev('VDP-INFO')['output'])[1])
            r['dropped_before'] = drops()
            r['latency_no_tasks'] = latency()
            ev('VARIABLE TA VARIABLE TB : TCA BEGIN TA @ 1+ TA ! AGAIN ; : TCB BEGIN TB @ 1+ TB ! 10 MS AGAIN ;')
            workers = [int(x) for x in ev("' TCA SPAWN . ' TCB SPAWN .")['output'].split('\r\n')[1].split()]
            t0, a0, b0 = time.perf_counter(), val('TA @'), val('TB @')
            time.sleep(5)
            t1, a1, b1 = time.perf_counter(), val('TA @'), val('TB @')
            span = t1 - t0
            r['throughput'] = {'seconds': round(span, 2), 'busy_task_increments_per_s': round(((a1 - a0) & 0xffffff) / span),
                               'sleeping_task_wakes_per_s': round((b1 - b0) / span, 1)}
            check('busy_task_progresses', a1 != a0, r['throughput'])
            check('sleeping_task_progresses', b1 > b0, r['throughput'])
            r['latency_two_tasks'] = latency()
            listed = c.tool('tasks')['tasks']
            r['tasks_running'] = listed[:2]
            check('engine_refused_while_live', not ev('ENGINE TOKEN')['ok'])
            ev('1234')  # foreground marker: background faults below must not clear it
            ev(': TPR 72 EMIT 105 EMIT CR ; : TKK KEY ;')
            spawned = ev("' TPR SPAWN . ' TKK SPAWN .")['output'].split('\r\n')[1]
            ids = [int(x) for x in spawned.split()]
            time.sleep(0.5)
            states = {t['id']: t for t in c.tool('tasks')['tasks']}
            check('printer_done', states[ids[0]]['state'] == 'done', states[ids[0]])
            check('printer_output_isolated', c.tool('task_output', {'id': ids[0]})['text'] == 'Hi\r\n')
            check('key_task_fails_alone', states[ids[1]]['state'] == 'failed' and 'KEY' in states[ids[1]]['error'], states[ids[1]])
            ev(' '.join(f'{i} TASK-KILL' for i in workers))
            check('all_stopped', not any(t['state'] in ('ready', 'sleeping') for t in c.tool('tasks')['tasks']))
            stack = c.tool('stacks')['data_stack']
            check('foreground_stack_preserved', stack['values_text'] == ['1234'], stack)
            ev('DROP')
            check('engine_switch_allowed_after', ev('ENGINE DIRECT')['ok'])
            r['latency_after'] = latency()
            r['dropped_after'] = drops()
            check('no_console_drops', r['dropped_after'] == r['dropped_before'], (r['dropped_before'], r['dropped_after']))
        r['status'] = 'passed'
    except BaseException as error:
        r.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        resume_autorun(r.get('autorun_paused'))
        a.output.write_text(json.dumps(r, indent=2) + '\n')
        print(json.dumps({k: r.get(k) for k in ('status', 'latency_no_tasks', 'latency_two_tasks', 'throughput', 'error')}, indent=1))


if __name__ == '__main__':
    main()

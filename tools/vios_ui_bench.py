#!/usr/bin/env python3
"""GUI Stage 0 baseline: where console/editor time goes today (no behaviour change).

Uses the ESP instrumentation through the running harness (127.0.0.1:8765):
  B1 console flood: VGA glyphs/s, USB console bytes, time the VDP parser
     spent blocked on USB console writes (design finding D2).
  B2 typing: key-to-glyph latency at the ESP, keys injected at the PS/2
     boundary at 20 and 100 characters/s into the console input line.
  B3 the same typing while two background tasks compute.
Typed characters are erased afterwards; temporary words FL/BUSY remain in RAM.
"""
import argparse
import json
from pathlib import Path
import time
try:
    from .vios_cli import RPCClient
except ImportError:
    from vios_cli import RPCClient


def typing(c, cps, count=40, char='q'):
    c.tool('esp_probe', {'char': char})
    for _ in range(count):
        c.tool('esp_inject', {'text': char})
        time.sleep(1 / cps)
    time.sleep(0.5)
    probe = c.tool('esp_probe')
    for _ in range(count // 8):
        c.tool('esp_inject', {'text': '\b' * 8})
        time.sleep(0.1)
    time.sleep(0.5)
    return probe


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--lines', type=int, default=1000)
    a = p.parse_args()
    report = {'status': 'running'}
    try:
        with RPCClient(timeout=180) as c:
            c.initialize()
            c.tool('connect', {})

            def ev(source, timeout=120):
                out = c.tool('evaluate', {'source': source, 'timeout': timeout})
                if not out['ok']:
                    raise AssertionError(f'{source!r}: {out["output"][-200:]}')
                return out['output']
            ev('WB-CONSOLE')  # the VGA console must be the visible view
            ev(': FL ( n -- ) 0 DO I . LOOP CR ;')
            c.tool('esp_stats', {'reset': True})
            t = time.perf_counter()
            ev(f'{a.lines} FL')
            returned = time.perf_counter() - t
            # The VGA console keeps drawing after the REPL returns; wait until only the
            # blinking cursor (a cell or two per 0.5 s) is still being drawn.
            last, stats = -100, c.tool('esp_stats')
            while stats['glyphs'] - last >= 4 and time.perf_counter() - t < 120:
                last = stats['glyphs']
                time.sleep(0.5)
                stats = c.tool('esp_stats')
            seconds = time.perf_counter() - t - 0.5
            c.tool('esp_stats', {'reset': True})
            report['B1_console_flood'] = {
                'lines': a.lines, 'repl_seconds': round(returned, 3), 'display_settled_seconds': round(seconds, 3), 'stats': stats,
                'glyphs_per_s': round(stats['glyphs'] / seconds),
                'usb_console_bytes_per_s': round(stats['console_bytes'] / seconds),
                'parser_blocked_on_usb_share': round(stats['usb_block_us'] / 1e6 / seconds, 3),
                'parser_busy_share': round(stats['parser_us'] / 1e6 / seconds, 3)}
            c.tool('events')
            report['B2_typing_idle'] = {f'{cps}cps': typing(c, cps) for cps in (20, 100)}
            report['B2_stats'] = c.tool('esp_stats', {'reset': True})
            ev(': BUSY BEGIN 0 UNTIL ;')
            ev("' BUSY SPAWN DROP ' BUSY SPAWN DROP")
            try:
                report['B3_typing_two_tasks'] = {f'{cps}cps': typing(c, cps) for cps in (20, 100)}
            finally:
                tasks = c.tool('tasks')['tasks']
                for task in tasks:
                    if task.get('name') == 'BUSY' or task['state'] in ('ready', 'sleeping'):
                        ev(f"{task['id']} TASK-KILL")
            report['events_after'] = c.tool('events')
            report['handles_after'] = c.tool('handles')['handles']
        report['status'] = 'measured'
    except BaseException as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        a.output.write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({k: v for k, v in report.items() if k != 'events_after'}, indent=1))


if __name__ == '__main__':
    main()

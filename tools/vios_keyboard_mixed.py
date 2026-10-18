#!/usr/bin/env python3
"""Mixed host traffic while a person types REPL lines on the physical keyboard.

Through the shared owner only. Sends a read-only probe line in a loop and
checks every result is exactly its own: a merged keystroke prefix would make
the probe fail or lose its frame. Lines typed at the keyboard (for example
`7 6 * .`) execute whole and appear as extra output between host frames; the
report counts them. Never sends source on an uncertain outcome; uses resync
only at a quiet idle prompt and records it as a failure.
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

PROBE = '11 31 + .'


def dropped(c):
    out = c.tool('evaluate', {'source': 'VDP-INFO'})['output']
    return int(re.search(r'console-dropped (\d+)', out)[1])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--seconds', type=float, default=180)
    p.add_argument('--marker', default='42', help='number the typed line prints, e.g. 7 6 * . -> 42')
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    report = {'probe': PROBE, 'seconds': a.seconds, 'probes': 0, 'probe_failures': [], 'resyncs': 0,
              'keyboard_lines': [], 'status': 'running'}
    try:
        with RPCClient(timeout=60) as c:
            c.initialize()
            report['dropped_before'] = dropped(c)
            end = time.monotonic() + a.seconds
            while time.monotonic() < end:
                try:
                    r = c.tool('evaluate', {'source': PROBE, 'timeout': 10})
                except Exception as error:  # noqa: BLE001 - record and attempt guarded recovery
                    report['probe_failures'].append({'t': round(a.seconds - (end - time.monotonic()), 1), 'error': str(error)})
                    try:
                        c.tool('resync', {'timeout': 10})
                        report['resyncs'] += 1
                    except Exception as again:  # noqa: BLE001
                        report['probe_failures'].append({'error': 'resync: ' + str(again)})
                        break
                    continue
                report['probes'] += 1
                out = r['output']
                expected = PROBE + '\r\n42 \r\n'
                if not r.get('ok') or expected not in out:
                    report['probe_failures'].append({'output': out})
                extra = out.replace(expected, '', 1)
                for line in re.findall(r'([ -~]+)\r\n', extra):
                    if line not in ('ok', 'vios> ', '') and not line.startswith('@'):
                        report['keyboard_lines'].append(line)
            report['dropped_after'] = dropped(c)
        report['status'] = 'passed' if not report['probe_failures'] else 'failed'
    finally:
        a.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('status', 'probes', 'resyncs', 'dropped_before', 'dropped_after')}),
          len(report['keyboard_lines']), 'keyboard output lines')


if __name__ == '__main__':
    main()

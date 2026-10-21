#!/usr/bin/env python3
"""Run the on-board acceptance suite on the Agon, then optionally a soak.

Every check runs through the harness on 127.0.0.1:8765 (start it first:
`tools/vios_harness.py --backend resident --tcp 127.0.0.1:8765`). Each tool
writes its own JSON report into OUTDIR; this runner writes OUTDIR/suite.json
with one status line per check and exits non-zero if any failed.

Default checks (none changes the boot slots or the workbench draft):
  gui        display server: windows, menus, widgets, frame budget
  gui_tools  the GUI development tools (browser, inspector, workspace, manager)
  console    the VGA console view (switches to the text console and back)
  audio      tones, tunes, loops, STOP, BEEP, VOLUME (ESP engine state; the
             closing melody is for a person to listen to)
  core_words DIRECT/TOKEN/C engines agree on the core words
  tasks      background tasks
  pipeline   producer stdout -> consumer KEY
  handles    handles, pipes, devices (writes and removes a /VIOS temp file)
  tools      source maps, SD file service, TYPE (writes and removes a temp file)
  extensible the extensible compiler: DOES>, POSTPONE, EVALUATE, MARKER on all engines
Not by default: incremental (replaces the workbench draft; --with-incremental).

  vios_board_suite.py OUTDIR [--only gui,audio] [--soak-minutes 30]
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CHECKS = ['gui', 'gui_tools', 'console', 'audio', 'core_words', 'tasks', 'pipeline', 'handles', 'tools', 'extensible']


def run(name, outdir):
    report = outdir / f'{name}.json'
    started = time.time()
    result = subprocess.run([sys.executable, str(ROOT / f'tools/vios_{name}_accept.py'), '--output', str(report)],
                            capture_output=True, text=True)
    status = 'failed'
    try:
        status = json.loads(report.read_text()).get('status', status)
    except (OSError, ValueError):
        pass
    if result.returncode and status == 'passed':
        status = 'failed'
    tail = (result.stdout + result.stderr).strip().splitlines()[-1:] if status != 'passed' else []
    return {'check': name, 'status': status, 'seconds': round(time.time() - started, 1), 'detail': tail}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    p.add_argument('outdir', type=Path)
    p.add_argument('--only', help='comma-separated subset of: ' + ','.join(CHECKS + ['incremental']))
    p.add_argument('--with-incremental', action='store_true')
    p.add_argument('--soak-minutes', type=float, default=0)
    a = p.parse_args(argv)
    checks = a.only.split(',') if a.only else CHECKS + (['incremental'] if a.with_incremental else [])
    unknown = [c for c in checks if c not in CHECKS + ['incremental']]
    if unknown:
        p.error('unknown checks: ' + ','.join(unknown))
    a.outdir.mkdir(parents=True, exist_ok=True)
    results = []
    for name in checks:
        results.append(run(name, a.outdir))
        print(f"{results[-1]['status']:7s} {name} ({results[-1]['seconds']} s) {' '.join(results[-1]['detail'])}", flush=True)
    if a.soak_minutes:
        report = a.outdir / 'soak.json'
        subprocess.run([sys.executable, str(ROOT / 'tools/vios_soak.py'), '--minutes', str(a.soak_minutes), '--output', str(report)],
                       capture_output=True, text=True)
        try:
            soak = json.loads(report.read_text())
        except (OSError, ValueError):
            soak = {'status': 'failed'}
        results.append({'check': 'soak', 'status': soak.get('status'), 'summary': soak.get('summary'), 'detail': [soak.get('error')] if soak.get('error') else []})
        print(f"{results[-1]['status']:7s} soak {soak.get('summary')}", flush=True)
    passed = all(r['status'] == 'passed' for r in results)
    (a.outdir / 'suite.json').write_text(json.dumps({'passed': passed, 'results': results}, indent=1) + '\n')
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())

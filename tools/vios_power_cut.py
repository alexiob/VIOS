#!/usr/bin/env python3
"""Power-loss test of SAVE and BOOT-TRIAL with the owner pulling the plug.

Stage 7 item 1 (docs/superpowers/specs/2026-10-19-vios-stage6-7-proposals.md).
The Agon Light 2 takes power and USB serial through one cable, so a power cut
also drops the serial port; this script runs its own harness and reconnects
when the port comes back. Each round:

  1. boot and connect; check the boot state is sane and record which version
     of the test module PCUT survived (verify phase);
  2. print UNPLUG NOW, then repeat the mutation back to back until the link
     drops, so the cut lands inside a write with good odds:
       save:  stage and activate a new version of the test module PCUT and
              SAVE its package (content-addressed files under /VIOS);
       trial: BOOT-TRIAL with the modules already active (a bundle and boot
              records are written; the boot after it confirms the same
              modules, so the boot configuration is unchanged);
  3. wait for the port to disappear and come back (plug the board back in).

Pass when every boot after a cut reaches the prompt with a consistent boot
record (the confirmed slot's manifest reads without error; no trial is left
pending) and, in save mode, every PCUT package saved so far, including the one
being written at the cut, either loads and passes its test or is refused
(a torn package must be detected, never run). SAVE writes a temporary file
and renames it, so a cut mid-write should leave only a stray temporary file.
Never writes flash; only the SD card (packages, bundles, boot records).

  vios_power_cut.py --mode save|trial [--rounds 5] [--port /dev/cu.usbserial-210] [--output report.json]
"""
import argparse
import glob
import json
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vios_cli import RPCClient  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def harness(port):
    return subprocess.Popen([sys.executable, str(ROOT / 'tools/vios_harness.py'), '--backend', 'resident',
                             '--port', port, '--tcp', '127.0.0.1:8765'],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def wait_port(pattern, present, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        ports = glob.glob(pattern)
        if bool(ports) == present:
            return ports[0] if ports else None
        time.sleep(0.2)
    return None


def source(n):
    return f': PCUT-VALUE {n} ; : PCUT-TEST PCUT-VALUE {n} = ;'


def verify(c, packages):
    """What booted after the cut, and whether it is consistent."""
    ev = lambda s: c.tool('evaluate', {'source': s, 'timeout': 30})['output']
    status = ev('BOOT-STATUS')
    manifest = ev('BOOT-MANIFEST')
    problems = []
    confirmed = [l for l in manifest.split('\r\n') if l.startswith('manifest ') and ' confirmed' in l]
    if any(' error ' in l for l in confirmed):
        problems.append('confirmed slot manifest error')
    if 'state PENDING' in status:
        problems.append('trial still pending after a boot')
    loads = {}
    for path in packages:                      # save mode: every package written so far
        r = c.tool('module_load', {'path': path})
        if r.get('ok'):
            ok = '-1' in ev('PCUT-TEST .').split()
            loads[path] = 'loaded, test passed' if ok else 'loaded, TEST FAILED'
            if not ok:
                problems.append(f'{path} loaded but its test failed')
        else:
            loads[path] = 'refused: ' + r.get('output', '').strip().split('\r\n')[-3:][0][:80]
    return {'boot_status': status.split('\r\n')[1] if '\r\n' in status else status,
            'confirmed': confirmed, 'packages': loads, 'problems': problems}


def mutate(c, mode, start, packages, limit=None):
    """Repeat the mutation until the link drops; returns how many completed."""
    n = start
    done = 0
    try:
        while True:
            n += 1
            if mode == 'save':
                version = f'r{n}'
                r = c.tool('module_stage', {'name': 'PCUT', 'version': version, 'source': source(n), 'test_word': 'PCUT-TEST'})
                if not r.get('ok'):
                    raise RuntimeError(f'stage refused: {r}')
                c.tool('module_activate', {'name': 'PCUT'})
                saved = c.tool('module_save', {'name': 'PCUT'})
                if saved.get('path') and saved['path'] not in packages:
                    packages.append(saved['path'])
            else:
                version = f'trial {n}'
                c.tool('boot_trial')
            done += 1
            print(f'  {mode} {version} done', flush=True)
            if limit is not None and done >= limit:
                raise RuntimeError('simulated cut (--simulate)')
    except Exception as error:   # the cut: the harness loses the board mid-request
        return done, n, f'{type(error).__name__}: {str(error)[:120]}'


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    p.add_argument('--mode', choices=('save', 'trial'), required=True)
    p.add_argument('--rounds', type=int, default=5)
    p.add_argument('--port', default='/dev/cu.usbserial-210')
    p.add_argument('--output', type=Path, default=Path('docs/bring-up/vios-power-cut.json'))
    p.add_argument('--simulate', type=int, metavar='N', help='dry run without unplugging: stop after N mutations and reset by reopening the port')
    a = p.parse_args(argv)
    pattern = a.port.rsplit('-', 1)[0] + '-*'
    report = {'mode': a.mode, 'rounds': []}
    counter = int(time.time()) % 100000 * 100
    packages = []                         # save mode: package paths written so far
    for round_ in range(a.rounds + 1):    # the last round only verifies
        port = wait_port(pattern, True, 600)
        if not port:
            report['error'] = 'board did not come back within 10 minutes'
            break
        h = harness(port)
        time.sleep(3)
        entry = {'round': round_, 'port': port}
        try:
            with RPCClient(timeout=200) as c:
                c.initialize()
                entry['connect'] = str(c.tool('connect', {}))[:120]
                entry['verify'] = verify(c, packages)
                print(f'round {round_}: booted {entry["verify"]["boot_status"]!r}, packages {len(entry["verify"]["packages"])}, '
                      f'problems {entry["verify"]["problems"] or "none"}', flush=True)
                if round_ == a.rounds:
                    break
                print('\n*** UNPLUG NOW (pull the USB-C cable), then plug it back in ***\n' if not a.simulate else '  (simulated cut)', flush=True)
                entry['completed'], counter, entry['cut'] = mutate(c, a.mode, counter, packages, a.simulate)
        except Exception as error:
            entry['error'] = f'{type(error).__name__}: {str(error)[:200]}'
            print(f'round {round_}: {entry["error"]}', flush=True)
        finally:
            h.terminate()
            try:
                h.wait(5)
            except subprocess.TimeoutExpired:
                h.kill()
            report['rounds'].append(entry)
        if round_ < a.rounds and not a.simulate:
            wait_port(pattern, False, 120)   # the cut removes the device
    verified = [r for r in report['rounds'] if 'verify' in r]
    report['passed'] = bool(verified) and len(verified) == len(report['rounds']) and not any(r['verify']['problems'] for r in verified)
    a.output.write_text(json.dumps(report, indent=1) + '\n')
    print('PASSED' if report['passed'] else 'FAILED', '-', a.output, flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())

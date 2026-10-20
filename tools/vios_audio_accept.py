#!/usr/bin/env python3
"""On-board acceptance for the lean audio engine (spec 2026-10-20-vios-audio).

Runs through the harness (127.0.0.1:8765). Sound cannot be heard by the
harness, so every check reads the ESP engine state (control command AS via
the esp_audio tool) next to the CPU's view (SOUND?):
  tone      TONE starts voice 0 at the right frequency, SOUND? sees it, it ends;
  tune      a stored tune plays on voice 1: notes advance, the cursor moves;
  error     an invalid tune is rejected with its slot and offset;
  loop      a `*` tune keeps playing; -1 STOP silences everything;
  compat    BEEP still gets its 0x85 acknowledgement; VOLUME sets the master;
  memory    internal and PSRAM free heap as reported by the ESP.
Ends with a short melody for a person to listen to.
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

FIELDS = re.compile(r'(\w+)=(\S+)')


def parse(status):
    fields = dict(FIELDS.findall(status))
    voices = {k: v for k, v in fields.items() if re.fullmatch(r'v\d', k)}
    return fields, voices


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    r = {'status': 'running', 'checks': {}, 'samples': []}

    def check(name, ok, detail=None):
        r['checks'][name] = {'ok': bool(ok), 'detail': detail}
        if not ok:
            raise AssertionError(f'{name}: {detail}')
    try:
        with RPCClient(timeout=120) as c:
            c.initialize()
            c.tool('connect', {})

            def audio():
                status = c.tool('esp_audio')['status']
                r['samples'].append(status)
                return parse(status)

            def result(source):
                out = c.tool('evaluate', {'source': source, 'timeout': 60})
                if not out['ok']:
                    raise AssertionError(f'{source!r}: {out["output"][-200:]}')
                lines = out['output'].split('\r\n')
                for i, line in enumerate(lines):
                    if line.strip().endswith(source.strip()):
                        # The answer follows the echo; skip a background task's line.
                        rest = [l.strip() for l in lines[i + 1:] if l.strip() and not re.match(r'^[a-z][\w-]*: ', l)]
                        if rest:
                            return rest[0]
                return lines[-1].strip()

            result('-1 STOP 127 VOLUME')
            f, v = audio()
            r['memory_idle'] = {k: f.get(k) for k in ('internal_free', 'internal_min', 'psram_free')}
            check('idle', f['busy'] == '0' and f['master'] == '127', f)

            busy = result('440 400 TONE SOUND? .')
            f, v = audio()
            check('tone', busy.startswith('1') and v['v0'].startswith('440Hz/') and f['busy'] == '1', (busy, f))
            time.sleep(0.6)
            check('tone-ends', result('SOUND? .').startswith('0'), audio()[0]['busy'])

            result('S" t150 l8 o4 cdefgab>c" 0 TUNE 0 1 PLAY')
            f1, v1 = audio()
            time.sleep(0.5)
            f2, v2 = audio()
            pos1, pos2 = int(v1['v1'].split('@')[1].split('/')[0]), int(v2['v1'].split('@')[1].split('/')[0])
            check('tune', '/s0@' in v1['v1'] and pos2 > pos1 and int(f2['notes']) > int(f1['notes']), (v1['v1'], v2['v1']))
            time.sleep(1.8)
            check('tune-ends', result('SOUND? .').startswith('0'), audio()[0])

            result('S" cdz" 2 TUNE')
            f, v = audio()
            check('error', f['err'] == '2@2' and f['slots'].split(',')[2] == '0', f)

            result('S" t200 l16 o5 c e g e *" 3 TUNE 3 2 PLAY')
            time.sleep(2.0)
            check('loop', result('SOUND? .').startswith('4'), audio()[0])
            result('-1 STOP')
            check('stop', result('SOUND? .').startswith('0'), audio()[0])

            check('beep', result('440 25 BEEP .').startswith('1'))
            result('64 VOLUME')
            check('volume', audio()[0]['master'] == '64')
            result('127 VOLUME')

            f, v = audio()
            r['memory_end'] = {k: f.get(k) for k in ('internal_free', 'internal_min', 'psram_free')}
            # A short melody (two voices) for a person to listen to.
            result('S" t120 l8 o4 e e r e r c e r g4 r4 <g4" 4 TUNE S" t120 l8 o3 w1 v10 c c r c r c c r >g4 r4 <g4" 5 TUNE 4 0 PLAY 5 1 PLAY')
            time.sleep(3)
        r['status'] = 'passed'
    except Exception as e:
        r['status'] = 'failed'
        r['error'] = str(e)
    finally:
        # Never leave a sound playing, whatever happened above (a failed check
        # once left the looping test tune running).
        try:
            with RPCClient(timeout=60) as c:
                c.initialize()
                c.tool('connect', {})
                c.tool('evaluate', {'source': '-1 STOP 127 VOLUME', 'timeout': 20})
        except Exception as e:
            r['stop_error'] = str(e)
    a.output.write_text(json.dumps(r, indent=2) + '\n')
    print(json.dumps({k: r[k] for k in r if k != 'samples'}, indent=2))
    return 0 if r['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())

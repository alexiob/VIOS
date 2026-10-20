#!/usr/bin/env python3
"""On-board acceptance for source maps, the SD file service and native TYPE.

Defines temporary RAM words; writes, reads back and removes /VIOS/SVCTEST.TXT.
No module, package or boot-slot changes.
"""
import argparse
import hashlib
import json
from pathlib import Path
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
    a = p.parse_args()
    r = {'status': 'running', 'checks': {}}

    def check(name, ok, detail=None):
        r['checks'][name] = {'ok': bool(ok), 'detail': detail}
        if not ok:
            raise AssertionError(f'{name}: {detail}')
    try:
        with RPCClient(timeout=150) as c:
            c.initialize()
            c.tool('connect', {})
            r['autorun_paused'] = pause_autorun(c)   # ENGINE switches are refused while tasks live

            def ev(s):
                out = c.tool('evaluate', {'source': s, 'timeout': 60})
                if not out['ok']:
                    raise AssertionError(f'{s!r}: {out["output"][-200:]}')
                return out['output']
            # Source maps on every engine.
            ev(': TSQ DUP * ; : TT 3 TSQ 1+ ;')
            for engine in ('DIRECT', 'TOKEN', 'C'):
                ev('ENGINE ' + engine)
                steps = c.tool('trace', {'name': 'TT', 'steps': 50})['steps']
                tokens = [s['source_token'] for s in steps]
                r[f'trace_{engine}'] = tokens
                check(f'trace_tokens_{engine}', tokens[:6] == ['3', 'TSQ', 'DUP', '*', ';', '1+'], tokens)
            ev('ENGINE DIRECT')
            ev(': TIN 1 0 / ; : TOUT 5 TIN 2 ;')
            c.tool('evaluate', {'source': 'TOUT', 'timeout': 30})
            e = c.tool('last_error')['last_error']
            r['last_error'] = e
            check('error_source_token', e['word'] == 'TIN' and e['source_token'] == '/' and e['frames'][0]['source_token'] == 'TIN', e)
            # SD file service.
            text = ''.join(f'line {i:03d} of the VIOS file service test\n' for i in range(40))
            t = time.perf_counter()
            put = c.tool('sd_put', {'path': '/VIOS/SVCTEST.TXT', 'text': text})
            got = c.tool('sd_get', {'path': '/VIOS/SVCTEST.TXT'})
            r['file_service'] = {'bytes': put['bytes'], 'seconds': round(time.perf_counter() - t, 2)}
            check('sd_roundtrip', got['text'] == text and got['sha256'] == hashlib.sha256(text.encode()).hexdigest(), got['bytes'])
            listed = c.tool('sd_list', {'path': '/VIOS'})['entries']
            check('sd_list_has_file', any(x['name'] == 'SVCTEST.TXT' and x['size'] == len(text) for x in listed), listed)
            ev('S" /VIOS/SVCTEST.TXT" FILE-REMOVE DROP')
            check('sd_removed', not any(x['name'] == 'SVCTEST.TXT' for x in c.tool('sd_list', {'path': '/VIOS'})['entries']))
            # Native TYPE resumes long strings through a pipeline.
            ev('VARIABLE TGOT : TCNT 0 TGOT ! BEGIN KEY -1 <> WHILE 1 TGOT +! REPEAT ;')
            ev('CREATE TLB 600 ALLOT TLB 600 90 FILL : TLT TLB 600 TYPE ;')
            ev("' TLT ' TCNT PIPELINE 2DROP")
            for _ in range(200):
                if not any(x['state'] in ('ready', 'sleeping') for x in c.tool('tasks')['tasks']):
                    break
                time.sleep(0.2)
            got = int(ev('TGOT @ .').split('\r\n')[1].split()[0])
            check('type_pipeline_exact', got == 600, got)
            dump = ev('TLB 4 DUMP')
            check('dump_hex', '5A 5A 5A 5A' in dump.replace('\r\n', ' ').replace('  ', ' ') or '5A 5A' in dump, dump[-120:])
            check('no_leaked_handles', c.tool('handles')['handles'] == [])
        r['status'] = 'passed'
    except BaseException as error:
        r.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        resume_autorun(r.get('autorun_paused'))
        a.output.write_text(json.dumps(r, indent=2) + '\n')
        print(json.dumps({k: r.get(k) for k in ('status', 'file_service', 'error')}))


if __name__ == '__main__':
    main()

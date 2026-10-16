#!/usr/bin/env python3
"""Reproduce the host-only differential review; no hardware or source mutations."""
from pathlib import Path
import hashlib
import json
import re
import shlex
import subprocess
import tarfile
import tempfile
from datetime import datetime, timezone

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
ARCHIVE = ROOT / 'artifacts/vios-workbench-2026-10-16/source-code.tar.gz'
LOG = HERE / 'latest-run.txt'
SOURCES = ['kernel/workbench.c', 'kernel/workbench_runtime.c',
           'tests/workbench_native.c', 'drivers/agon-vdp/text_screen.c']

def sha(data):
    return hashlib.sha256(data).hexdigest()

with LOG.open('w') as log, tempfile.TemporaryDirectory(prefix='vios-review-') as temp:
    temp = Path(temp)
    def say(message):
        print(message, flush=True)
        log.write(message + '\n')
        log.flush()
    def run(argv, expected=0):
        say('$ ' + shlex.join(str(a) for a in argv))
        result = subprocess.run([str(a) for a in argv], cwd=ROOT, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        say(result.stdout.rstrip())
        say(f'exit={result.returncode}; expected={expected}')
        assert result.returncode == expected
        return result
    with tarfile.open(ARCHIVE) as archive:
        baseline = archive.extractfile('kernel/workbench.c').read()
    metadata = {
        'utc': datetime.now(timezone.utc).isoformat(),
        'baseline_archive': str(ARCHIVE.relative_to(ROOT)),
        'baseline_archive_sha256': sha(ARCHIVE.read_bytes()),
        'baseline_member': 'kernel/workbench.c',
        'baseline_member_sha256': sha(baseline),
        'reviewed_source_sha256': {s: sha((ROOT/s).read_bytes()) for s in SOURCES},
        'harness_sha256': sha((HERE/'workbench_differential.c').read_bytes()),
        'initial_draft_bytes': 4096, 'random_operations': 30000, 'seed': 7781,
        'compare': 'all 4800 character and attribute cells every third operation',
    }
    (HERE/'review-manifest.json').write_text(json.dumps(metadata, indent=2)+'\n')
    say(json.dumps(metadata, indent=2))
    run(['cc', '--version'])
    old = temp/'baseline.c'
    old.write_bytes(baseline)
    names = sorted(set(re.findall(r'\b(vios_workbench_\w+)\s*\(',
                      (ROOT/'include/vios/workbench.h').read_text())))
    cc = ['cc', '-std=c99', '-g', '-fsanitize=address,undefined',
          '-fno-sanitize-recover=all', '-I'+str(ROOT/'include')]
    old_object = temp/'baseline.o'
    run(cc+['-D'+n+'=old_'+n for n in names]+['-c', old, '-o', old_object])
    common = [HERE/'workbench_differential.c', old_object]+[ROOT/s for s in
             ['forth/system.c','forth/modules.c','forth/vm_reference.c','forth/vm_memory.c']]
    binary = temp/'current-differential'
    run(cc+common+[ROOT/'kernel/workbench.c','-o',binary])
    run([binary, '--cursor-row'])
    run([binary, '--cursor-column'])
    run([binary])
    # Reconstruct just the originally defective predicate against latest code.
    # This is diagnostic negative control, not an archived historical source.
    current = (ROOT/'kernel/workbench.c').read_text()
    predicate = '(view.damage||view.cursor!=wb.cursor||view.focus!=wb.focus)'
    assert current.count(predicate) == 1
    defective = temp/'missing-cursor-damage.c'
    defective.write_text(current.replace(predicate,
                         '(view.cursor!=wb.cursor||view.focus!=wb.focus)'))
    bad_binary = temp/'negative-control'
    run(cc+common+[defective,'-o',bad_binary])
    run([bad_binary,'--cursor-row'], expected=2)
    run([bad_binary,'--cursor-column'], expected=2)
    say('PASS: latest source matches immutable baseline; both reconstructed negative controls reproduce the original cursor defects.')

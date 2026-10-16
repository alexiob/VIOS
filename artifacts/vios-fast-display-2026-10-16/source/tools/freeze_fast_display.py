#!/usr/bin/env python3
"""Freeze paired VIOS fast-display images and evidence; never accesses hardware.

Run after final qualification and source/documentation changes have stopped:
    python3 tools/freeze_fast_display.py
    python3 tools/freeze_fast_display.py --verify-only

An existing destination is always refused. A failed freeze can leave an
incomplete directory without manifest.json; inspect it before removing it.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import tarfile

RELEASE = 'artifacts/vios-fast-display-2026-10-16'
BACKUP = 'backups/esp32/pre-vios-fast-display-app0-2026-10-16.bin'
BACKUP_SHA256 = 'e0ed0d20b2438e26e47404a152e9aeeeb98268f7c52113fcedec6854a8c845ae'
RATES = (384000, 1152000)
VARIANT_FILES = ('vios-resident.bin', 'vios-resident.baud', 'vios-resident.elf',
                 'vios-resident.map', 'firmware.bin', 'build-manifest.json')
EXCLUDED = {'artifacts', 'backups', '.git', '.tools', '.venv', 'build', 'docs',
            '.pio', '__pycache__', '.DS_Store', '.pytest_cache'}
SOURCE_DIRS = ('arch', 'boards', 'drivers', 'forth', 'include', 'kernel',
               'bench', 'tools', 'tests', 'firmware')
REQUIRED_EVIDENCE = (
    'docs/superpowers/plans/2026-10-16-vios-fast-display.md',
    'docs/bring-up/vios-fast-display-review.md',
    'docs/bring-up/vios-fast-display-host-tests.txt',
    'docs/bring-up/vios-fast-display-engine-tests.txt',
    'docs/benchmarks/fast-display-2026-10-16.md',
    'docs/bring-up/vios-fast-display-restored.json',
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def regular(root, relative):
    """Keep manifest and command-line paths inside their owning tree."""
    relative = Path(relative)
    require(not relative.is_absolute() and '..' not in relative.parts, f'Expected a safe relative path: {relative}')
    path = root / relative
    require(not path.is_symlink(), f'Refusing symlink: {path}')
    require(path.resolve().is_relative_to(root.resolve()), f'Path escapes tree: {path}')
    require(path.is_file(), f'Missing required file: {path}')
    return path


def describe(path, base):
    return {'path': path.relative_to(base).as_posix(), 'size': path.stat().st_size,
            'sha256': sha(path)}


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def read_json(path):
    return json.loads(path.read_text())


def source_paths(root):
    paths = set()
    for directory in SOURCE_DIRS:
        for path in (root / directory).rglob('*'):
            relative = path.relative_to(root)
            if path.is_file() and not any(part in EXCLUDED for part in relative.parts) and path.suffix != '.pyc':
                regular(root, relative)
                paths.add(relative.as_posix())
    for name in ('Makefile', 'README.md', '.gitignore'):
        if (root / name).is_file():
            regular(root, name)
            paths.add(name)
    require(paths, 'No source files found')
    return sorted(paths)


def evidence_paths(root, extra):
    paths = set(REQUIRED_EVIDENCE)
    restore_procedure = 'docs/bring-up/firmware-and-sd-change.md'
    if (root / restore_procedure).is_file():
        paths.add(restore_procedure)
    patterns = ('docs/bring-up/*fast-display*', 'docs/benchmarks/*fast-display*',
                'docs/benchmarks/render-display-only*',
                'docs/benchmarks/render-display-scroll-prototype*',
                'build/*fast-display*', 'build/verification/fast-display-review/*')
    for pattern in patterns:
        paths.update(p.relative_to(root).as_posix() for p in root.glob(pattern) if p.is_file())
    for name in extra:
        path = root / name
        if path.is_dir():
            paths.update(p.relative_to(root).as_posix() for p in path.rglob('*') if p.is_file())
        else:
            paths.add(name)
    for name in paths:
        regular(root, name)
    # Failed investigation logs are preserved too; their presence is never
    # interpreted as acceptance. Qualification is recorded by the parent.
    return sorted(paths)


def check_variant(root, rate):
    directory = root / 'build/fast-display' / str(rate)
    for name in VARIANT_FILES:
        regular(root, (directory / name).relative_to(root))
    manifest = read_json(directory / 'build-manifest.json')
    require(int((directory / 'vios-resident.baud').read_text().strip()) == rate,
            f'{rate}: CPU baud sidecar mismatch')
    require(manifest.get('internal_baud') == rate and
            manifest.get('embedded_vios', {}).get('internal_baud') == rate,
            f'{rate}: CPU/ESP manifest baud mismatch')
    require(manifest.get('usb_baud') == 115200, f'{rate}: USB must stay at 115200')
    require(manifest.get('app0_offset') == 0x10000 and manifest.get('app0_limit') == 0x140000,
            f'{rate}: unexpected app0 layout')
    for name, metadata in (('firmware.bin', manifest), ('vios-resident.bin', manifest['embedded_vios'])):
        path = directory / name
        require(path.stat().st_size == metadata.get('size') and sha(path) == metadata.get('sha256'),
                f'{rate}: size/hash mismatch for {name}')
    require((directory / 'firmware.bin').stat().st_size <= manifest['app0_limit'],
            f'{rate}: firmware exceeds app0')
    stage0 = regular(root, 'build/agon-stage0.bin')
    require(sha(stage0) == manifest.get('native_stage0', {}).get('sha256') and
            stage0.stat().st_size == manifest['native_stage0'].get('size'),
            f'{rate}: native stage0 differs from build manifest')
    if rate == 1152000:
        hashes = manifest.get('build_source_sha256', {})
        require(hashes, 'Default firmware manifest has no build source hashes')
        for name, expected in hashes.items():
            require(sha(regular(root, name)) == expected,
                    f'Default firmware source changed since build: {name}')
    return manifest


def archive(root, names, destination):
    with tarfile.open(destination, 'w:gz') as output:
        for name in names:
            output.add(root / name, arcname=name, recursive=False)


def verify_archive(path, expected):
    seen = set()
    with tarfile.open(path, 'r:gz') as source:
        for entry in source:
            require(entry.isfile() and entry.name in expected and entry.name not in seen,
                    f'Unexpected archive entry: {path}: {entry.name}')
            stream = source.extractfile(entry)
            require(stream is not None, f'Unreadable archive entry: {entry.name}')
            digest = hashlib.sha256()
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(block)
            require(digest.hexdigest() == expected[entry.name], f'Archive hash mismatch: {entry.name}')
            seen.add(entry.name)
    require(seen == set(expected), f'Incomplete archive: {path}')


def verify(output):
    manifest_path = regular(output, 'manifest.json')
    checksum = regular(output, 'manifest.sha256').read_text().split()
    require(len(checksum) == 2 and checksum[1] == 'manifest.json' and checksum[0] == sha(manifest_path),
            'Release manifest SHA-256 mismatch')
    manifest = read_json(manifest_path)
    require(manifest.get('schema') == 1 and manifest.get('default_internal_baud') == 1152000,
            'Unrecognized fast-display release manifest')
    names = set()
    for entry in manifest['files']:
        path = regular(output, entry['path'])
        require(path.stat().st_size == entry['size'] and sha(path) == entry['sha256'],
                f'Release file size/hash mismatch: {entry["path"]}')
        require(entry['path'] not in names, f'Duplicate file: {entry["path"]}')
        names.add(entry['path'])
    actual = {p.relative_to(output).as_posix() for p in output.rglob('*') if p.is_file()}
    require(actual == names | {'manifest.json', 'manifest.sha256'}, 'Unexpected or missing release files')
    for prefix in ('source', 'evidence'):
        hashes = read_json(regular(output, prefix + '-sha256.json'))
        for name, expected in hashes.items():
            require(sha(regular(output, prefix + '/' + name)) == expected,
                    f'{prefix} snapshot mismatch: {name}')
        verify_archive(output / (prefix + '-code.tar.gz' if prefix == 'source' else 'documentation-evidence.tar.gz'), hashes)
    return manifest


def freeze(root, output, extra, acceptance):
    require(not output.exists() and not output.is_symlink(), f'Refusing existing release destination: {output}')
    variants = {str(rate): check_variant(root, rate) for rate in RATES}
    backup = regular(root, BACKUP)
    require(sha(backup) == BACKUP_SHA256, 'Original verified app0 restore backup hash mismatch')
    names = source_paths(root)
    evidence = evidence_paths(root, extra + ([acceptance] if acceptance else []))
    host_log = regular(root, REQUIRED_EVIDENCE[2]).read_text()
    summaries = list(re.finditer(r'Ran (\d+) tests? in [^\n]+\n\s*([^\n]+)', host_log))
    require(summaries and summaries[-1].group(2).strip() == 'OK',
            'Final host-test log does not record a successful final full suite')
    host = summaries[-1]
    engine = re.findall(r'PASS: (\d+) total (?:linked-disassembly execution )?probes',
                        regular(root, REQUIRED_EVIDENCE[3]).read_text())
    require(engine, 'Native engine log has no successful total probe result')
    require(read_json(regular(root, 'docs/bring-up/vios-fast-display-restored.json')).get('status') == 'passed',
            'Restored-state JSON status must be passed')
    if acceptance:
        require(read_json(regular(root, acceptance)).get('status') == 'passed', 'Acceptance JSON status must be passed')
    hashes = {name: sha(root / name) for name in names}
    evidence_hashes = {name: sha(root / name) for name in evidence}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.mkdir()  # Exclusive reservation: never overwrite an existing directory.

    def copy(source, relative):
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)

    for rate in RATES:
        for name in VARIANT_FILES:
            copy(root / 'build/fast-display' / str(rate) / name, f'variants/{rate}/{name}')
    for original, target in (('vios-resident.bin', 'vios-accepted.bin'),
                             ('vios-resident.baud', 'vios-accepted.baud'),
                             ('vios-resident.elf', 'vios-accepted.elf'),
                             ('vios-resident.map', 'vios-accepted.map'),
                             ('firmware.bin', 'firmware-accepted.bin'),
                             ('build-manifest.json', 'build-manifest-accepted.json')):
        copy(root / 'build/fast-display/1152000' / original, target)
    copy(root / 'build/agon-stage0.bin', 'agon-stage0.bin')
    copy(backup, 'restore/' + backup.name)
    for name in names:
        copy(root / name, 'source/' + name)
    for name in evidence:
        copy(root / name, 'evidence/' + name)
    write_json(output / 'source-sha256.json', hashes)
    write_json(output / 'evidence-sha256.json', evidence_hashes)
    archive(root, names, output / 'source-code.tar.gz')
    archive(root, evidence, output / 'documentation-evidence.tar.gz')
    verify_archive(output / 'source-code.tar.gz', hashes)
    verify_archive(output / 'documentation-evidence.tar.gz', evidence_hashes)
    # Re-read every input after snapshotting; concurrent edits must not produce
    # a successful manifest for a mixed source/evidence snapshot.
    for prefix, snapshot_hashes in (('source', hashes), ('evidence', evidence_hashes)):
        for name, expected in snapshot_hashes.items():
            require(sha(root / name) == expected, f'Input changed during freeze: {name}')
            require(sha(output / prefix / name) == expected, f'Copied snapshot hash mismatch: {name}')
    for rate in RATES:
        check_variant(root, rate)
        for name in VARIANT_FILES:
            require(sha(root / 'build/fast-display' / str(rate) / name) == sha(output / f'variants/{rate}/{name}'),
                    f'Variant changed during freeze: {rate}/{name}')
    for original, target in (('vios-resident.bin', 'vios-accepted.bin'),
                             ('vios-resident.baud', 'vios-accepted.baud'),
                             ('vios-resident.elf', 'vios-accepted.elf'),
                             ('vios-resident.map', 'vios-accepted.map'),
                             ('firmware.bin', 'firmware-accepted.bin'),
                             ('build-manifest.json', 'build-manifest-accepted.json')):
        require(sha(output / target) == sha(output / 'variants/1152000' / original),
                f'Accepted alias differs from default variant: {target}')
    require(sha(output / 'agon-stage0.bin') == variants['1152000']['native_stage0']['sha256'],
            'Copied native stage0 hash mismatch')
    require(sha(output / 'restore' / backup.name) == BACKUP_SHA256, 'Copied restore backup hash mismatch')
    (output / 'RESTORE.md').write_text(
        '# Fast-display release and restore\n\n'
        'Default: matched 1,152,000-baud CPU/ESP pair; USB stays at 115200.\n'
        'The 384,000-baud matched pair is retained under variants/384000/.\n'
        'Never mix CPU and ESP images from different variants.\n\n'
        'The original readback-verified app0 payload is under restore/. Its '\
        'SHA-256 is recorded in manifest.json. Restore only this app0 image at '\
        'offset 0x10000 using the established application-only restore procedure '\
        'in evidence/docs/bring-up/firmware-and-sd-change.md (or the repository '\
        'copy). Do not write eZ80 flash, the ESP bootloader/partition table, or SD '\
        'boot selections. The original app0 embeds its matching resident.\n\n'
        'Inspect source/ and evidence/ directly; matching compressed archives '\
        'and per-original-path SHA-256 maps are also included. manifest.sha256 '\
        'checks the manifest; manifest.json is written last. Verify with '\
        '`python3 tools/freeze_fast_display.py --verify-only`.\n')
    manifest = {
        'schema': 1, 'project': 'VIOS', 'milestone': 'fast display transport, compositor scroll and full-width editor',
        'created_utc': datetime.now(timezone.utc).isoformat(), 'status': 'frozen',
        'default_internal_baud': 1152000, 'usb_baud': 115200,
        'variants': {rate: {'path': 'variants/' + rate, 'cpu_sha256': value['embedded_vios']['sha256'],
                            'firmware_sha256': value['sha256']} for rate, value in variants.items()},
        'accepted_cpu': describe(output / 'vios-accepted.bin', output),
        'accepted_firmware': describe(output / 'firmware-accepted.bin', output),
        'previous_app0_backup': {**describe(output / 'restore' / backup.name, output),
                                 'original_path': BACKUP, 'app0_offset': 0x10000},
        'previous_release': 'artifacts/vios-source-rows-2026-10-16/manifest.json',
        'source_files': len(names), 'source_manifest': 'source-sha256.json',
        'evidence_files': len(evidence), 'evidence_manifest': 'evidence-sha256.json',
        'acceptance_report': acceptance,
        'verification': {'host_tests_passed': int(host.group(1)), 'native_engine_probes': int(engine[-1])},
        'files': [describe(p, output) for p in sorted(output.rglob('*')) if p.is_file()],
    }
    payload = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode()
    manifest_hash = hashlib.sha256(payload).hexdigest()
    (output / 'manifest.sha256').write_text(manifest_hash + '  manifest.json\n')
    (output / 'manifest.json').write_bytes(payload)  # Commit marker: always last.
    verify(output)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', default=RELEASE, help='release directory, relative to repository root')
    parser.add_argument('--evidence', action='append', default=[], metavar='PATH', help='additional repository-relative file/directory')
    parser.add_argument('--acceptance', help='optional final acceptance JSON; status must be passed')
    parser.add_argument('--verify-only', action='store_true', help='read/hash existing frozen release; write nothing')
    args = parser.parse_args()
    root = args.root.resolve()
    output = root / args.output
    try:
        require(not Path(args.output).is_absolute() and '..' not in Path(args.output).parts,
                'Output directory must be repository-relative without parent traversal')
        require(output.resolve().is_relative_to(root), 'Output directory must be inside the repository root')
        result = verify(output) if args.verify_only else freeze(root, output, args.evidence, args.acceptance)
    except (OSError, ValueError, KeyError, TypeError, tarfile.TarError) as error:
        print(f'Fast-display release refused: {error}', file=sys.stderr)
        return 1
    print(f'{"Verified" if args.verify_only else "Frozen"} {output}: {result["source_files"]} source files, '
          f'{result["evidence_files"]} evidence files; SHA-256 {sha(output / "manifest.json")}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

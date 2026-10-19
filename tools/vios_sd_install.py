#!/usr/bin/env python3
"""Install the untethered VIOS boot onto a mounted MOS SD card (U4).

docs/superpowers/specs/2026-10-19-vios-untethered-boot.md. The card gets:

  /mos/viosboot.bin    the boot stub (make viosboot), run by MOS as `viosboot`
  /vios/vios.bin       the resident (make vios-resident), the stub's SD copy
  /vios/vios.man       its 16-byte manifest, the same bytes as the ESP's E8
                       'M' packet: 'M' 1 192 size:u32 crc32:u32 gui baud:u32
  /autoexec.txt        gains one `viosboot` line; every other line is kept
                       (the original is saved once as autoexec.txt.vios-backup)

/vios/boot.cnt (the unfinished-boot counter) is removed: a fresh install
starts with a clean count. Files are written as temporaries and renamed, the
image before its manifest, so an interrupted install leaves a manifest that
does not match the image and the stub refuses it. Module bundles in /VIOS
(BOOT_A.VIB, ...) are not touched.

  vios_sd_install.py install CARD [--resident BIN] [--stub BIN] [--dry-run]
  vios_sd_install.py check CARD [--resident BIN]

`check` verifies an installed card (and, with --resident, that it carries that
build). Both print a JSON report and exit non-zero on a problem.
"""
import argparse
import json
from pathlib import Path
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
BLOCK = 192
IMAGE_MAX = 0x6F000
STUB_MAX = 0x8000
MOS_HEADER = b'MOS\x00\x01'
AUTOEXEC_LINE = 'viosboot'


def manifest(image, gui, baud):
    return (b'M' + bytes((1, BLOCK)) + len(image).to_bytes(4, 'little') + zlib.crc32(image).to_bytes(4, 'little')
            + bytes((gui,)) + baud.to_bytes(4, 'little'))


def parse_manifest(data):
    if len(data) != 16 or data[:3] != b'M\x01' + bytes((BLOCK,)):
        raise ValueError('not a version 1 VIOS boot manifest')
    return {'size': int.from_bytes(data[3:7], 'little'), 'crc32': int.from_bytes(data[7:11], 'little'),
            'gui': data[11], 'baud': int.from_bytes(data[12:16], 'little')}


def check_image(image):
    if not 0 < len(image) <= IMAGE_MAX or image[64:69] != MOS_HEADER:
        raise ValueError('resident must be 1..0x6F000 bytes with the ADL MOS header at offset 64')


def check_stub(stub):
    if not 0 < len(stub) <= STUB_MAX or stub[64:69] != MOS_HEADER:
        raise ValueError('boot stub must be a moslet (<= 32 KB) with the ADL MOS header')


def sidecar(resident, suffix, default):
    path = resident.with_suffix(suffix)
    return int(path.read_text().strip()) if path.exists() else default


def find(card, name):
    """FAT is case-insensitive: the existing entry for name, whatever its case."""
    folder, parts = card, Path(name).parts
    for i, part in enumerate(parts):
        match = next((e for e in folder.iterdir() if e.name.lower() == part.lower()), None) if folder.is_dir() else None
        if match is None:
            return folder.joinpath(*parts[i:])
        folder = match
    return folder


def autoexec_with_viosboot(text):
    lines = text.splitlines()
    if any(line.strip().lower() == AUTOEXEC_LINE for line in lines):
        return None
    body = '\r\n'.join(lines)
    return (body + '\r\n' if body else '') + AUTOEXEC_LINE + '\r\n'


def write_atomic(path, data, dry_run):
    if dry_run:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_bytes(data)
    temporary.replace(path)


def install(card, resident_path, stub_path, dry_run=False):
    if not card.is_dir():
        raise ValueError(f'{card} is not a mounted card directory')
    image, stub = resident_path.read_bytes(), stub_path.read_bytes()
    check_image(image)
    check_stub(stub)
    man = manifest(image, sidecar(resident_path, '.gui', 0), sidecar(resident_path, '.baud', 1152000))
    report = {'card': str(card), 'dry_run': dry_run, 'written': [], 'kept': []}
    for name, data in (('mos/viosboot.bin', stub), ('vios/vios.bin', image), ('vios/vios.man', man)):
        target = find(card, name)
        write_atomic(target, data, dry_run)
        report['written'].append({'file': str(target.relative_to(card)), 'bytes': len(data)})
    autoexec = find(card, 'autoexec.txt')
    original = autoexec.read_text(encoding='latin-1') if autoexec.exists() else ''
    updated = autoexec_with_viosboot(original)
    if updated is None:
        report['kept'].append(str(autoexec.relative_to(card)))
    else:
        backup = autoexec.with_name(autoexec.name + '.vios-backup')
        if autoexec.exists() and not backup.exists() and not dry_run:
            backup.write_bytes(autoexec.read_bytes())
        write_atomic(autoexec, updated.encode('latin-1'), dry_run)
        report['written'].append({'file': str(autoexec.relative_to(card)), 'bytes': len(updated)})
    counter = find(card, 'vios/boot.cnt')
    if counter.exists():
        if not dry_run:
            counter.unlink()
        report['removed'] = str(counter.relative_to(card))
    report['manifest'] = parse_manifest(man)
    return report


def check(card, resident_path=None):
    problems = []
    report = {'card': str(card), 'problems': problems}
    stub, image, man = (find(card, name) for name in ('mos/viosboot.bin', 'vios/vios.bin', 'vios/vios.man'))
    try:
        check_stub(stub.read_bytes())
    except (OSError, ValueError) as error:
        problems.append(f'stub: {error}')
    try:
        data = image.read_bytes()
        check_image(data)
        info = parse_manifest(man.read_bytes())
        report['manifest'] = info
        if info['size'] != len(data) or info['crc32'] != zlib.crc32(data):
            problems.append('vios.bin does not match vios.man (interrupted install?)')
        if resident_path is not None and resident_path.read_bytes() != data:
            problems.append(f'the card does not carry {resident_path}')
    except (OSError, ValueError) as error:
        problems.append(f'image: {error}')
    autoexec = find(card, 'autoexec.txt')
    if not autoexec.exists() or autoexec_with_viosboot(autoexec.read_text(encoding='latin-1')) is not None:
        problems.append('autoexec.txt does not run viosboot')
    counter = find(card, 'vios/boot.cnt')
    if counter.exists():
        report['unfinished_boots'] = counter.read_bytes()[:1][0] if counter.stat().st_size else 0
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)
    for name in ('install', 'check'):
        p = sub.add_parser(name)
        p.add_argument('card', type=Path)
        p.add_argument('--resident', type=Path, default=ROOT/'build/vios-resident.bin' if name == 'install' else None)
        if name == 'install':
            p.add_argument('--stub', type=Path, default=ROOT/'build/viosboot.bin')
            p.add_argument('--dry-run', action='store_true')
    a = parser.parse_args(argv)
    try:
        report = install(a.card, a.resident, a.stub, a.dry_run) if a.command == 'install' else check(a.card, a.resident)
    except (OSError, ValueError) as error:
        print(json.dumps({'error': str(error)}))
        return 2
    print(json.dumps(report, indent=2))
    return 1 if report.get('problems') else 0


if __name__ == '__main__':
    sys.exit(main())

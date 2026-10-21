#!/usr/bin/env python3
"""Package a VIOS release from a finished build (docs/RELEASING.md).

Run after `make vios-resident viosboot` and the ESP build. Writes
dist/vios-<version>/ and dist/vios-<version>.zip:

  esp32/firmware.bin          the ESP32 app (embeds the CPU image), for app0/app1
  esp32/build-manifest.json   its build identity
  sd/                         the SD card tree: /mos/viosboot.bin, /vios/vios.bin,
                              /vios/vios.man, /autoexec.txt (copy onto the card)
  tools/                      vios_firmware_ab.py and vios_sd_install.py
  CHANGELOG.md                this version's section
  LICENSE                     the MIT License
  SHA256SUMS                  every file above

  vios_release.py version              print VERSION
  vios_release.py sync                 rewrite include/vios/version.h from VERSION
  vios_release.py notes [VERSION]      print that version's CHANGELOG section
  vios_release.py package [--out dist]
"""
import argparse
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION_RE = re.compile(r'^(\d+)\.(\d+)\.(\d+)$')


def version():
    text = (ROOT / 'VERSION').read_text().strip()
    if not VERSION_RE.match(text):
        raise SystemExit(f'VERSION must be MAJOR.MINOR.PATCH, not {text!r}')
    return text


COPYRIGHT = '(c) 2026 Alessandro Iob'


def version_header(v):
    major, minor, patch = VERSION_RE.match(v).groups()
    return ('/* Generated from VERSION by tools/vios_release.py sync (make does it); do not edit. */\n'
            '#ifndef VIOS_VERSION_H\n#define VIOS_VERSION_H\n'
            f'#define VIOS_VERSION "{v}"\n'
            f'#define VIOS_VERSION_SHORT "{major}.{minor}"           /* the banner: VIOS v{major}.{minor} */\n'
            f'#define VIOS_VERSION_MAJOR {major}\n#define VIOS_VERSION_MINOR {minor}\n#define VIOS_VERSION_PATCH {patch}\n'
            f'#define VIOS_BANNER "VIOS v{major}.{minor}"\n'
            f'#define VIOS_COPYRIGHT "{COPYRIGHT}"\n'
            '#endif\n')


def sync():
    path = ROOT / 'include/vios/version.h'
    text = version_header(version())
    if not path.exists() or path.read_text() != text:
        path.write_text(text)
    return path


def notes(wanted):
    """The CHANGELOG section '## [wanted] ...' up to the next '## ['."""
    lines, out = (ROOT / 'CHANGELOG.md').read_text().splitlines(), None
    for line in lines:
        if line.startswith('## ['):
            if out is not None:
                break
            if line.startswith(f'## [{wanted}]'):
                out = []
                continue
        if out is not None:
            out.append(line)
    if out is None:
        raise SystemExit(f'CHANGELOG.md has no "## [{wanted}]" section')
    return '\n'.join(out).strip() + '\n'


def package(out_dir):
    v = version()
    need = {'build/vios-resident.bin': 'make vios-resident', 'build/viosboot.bin': 'make viosboot',
            'firmware/esp32-vios/.pio/build/esp32-vios/firmware.bin': 'the ESP build (pio run)'}
    for path, how in need.items():
        if not (ROOT / path).exists():
            raise SystemExit(f'{path} is missing: run {how} first')
    dest = out_dir / f'vios-{v}'
    if dest.exists():
        shutil.rmtree(dest)
    (dest / 'esp32').mkdir(parents=True)
    (dest / 'tools').mkdir()
    shutil.copy(ROOT / 'firmware/esp32-vios/.pio/build/esp32-vios/firmware.bin', dest / 'esp32')
    shutil.copy(ROOT / 'firmware/esp32-vios/build-manifest.json', dest / 'esp32')
    # The installer writes the exact card tree; its GUI and baud sidecars sit next to build/vios-resident.bin.
    (dest / 'sd').mkdir()
    subprocess.run([sys.executable, str(ROOT / 'tools/vios_sd_install.py'), 'install', str(dest / 'sd'),
                    '--resident', str(ROOT / 'build/vios-resident.bin'), '--stub', str(ROOT / 'build/viosboot.bin')],
                   check=True, stdout=subprocess.DEVNULL)
    for tool in ('vios_firmware_ab.py', 'vios_sd_install.py'):
        shutil.copy(ROOT / 'tools' / tool, dest / 'tools')
    (dest / 'CHANGELOG.md').write_text(f'# VIOS {v}\n\n' + notes(v))
    shutil.copy(ROOT / 'LICENSE', dest / 'LICENSE')
    files = sorted(p for p in dest.rglob('*') if p.is_file())
    sums = ''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(dest).as_posix()}\n' for p in files)
    (dest / 'SHA256SUMS').write_text(sums)
    archive = out_dir / f'vios-{v}.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
        for p in sorted(dest.rglob('*')):
            if p.is_file():
                z.write(p, Path(dest.name) / p.relative_to(dest))
    print(archive)
    return archive


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = p.add_subparsers(dest='command', required=True)
    sub.add_parser('version')
    sub.add_parser('sync')
    n = sub.add_parser('notes')
    n.add_argument('version', nargs='?')
    k = sub.add_parser('package')
    k.add_argument('--out', type=Path, default=ROOT / 'dist')
    a = p.parse_args(argv)
    if a.command == 'version':
        print(version())
    elif a.command == 'sync':
        print(sync().relative_to(ROOT))
    elif a.command == 'notes':
        sys.stdout.write(notes(a.version or version()))
    else:
        package(a.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Refresh the boot files on the board's SD card through the running resident.

docs/superpowers/specs/2026-10-19-vios-untethered-boot.md §16.10. For a board
without the wire, whose card cannot be taken to the host. Same files and bytes
as `vios_sd_install.py install` writes, over the harness's SD handles (OPEN /
HANDLE-PUT, 100 bytes per command):

  /vios/vios.bin, /vios/vios.man     always
  /mos/viosboot.bin                  with --stub

The resident has no rename, so the installer's temporary-and-rename step
becomes: overwrite vios.man with 16 zero bytes first (the stub refuses that
manifest), then write vios.bin, then the real vios.man. An interrupted refresh
therefore leaves a card whose SD copy the stub refuses, never a mismatched one
it would run. Afterwards vios.man is read back in full and vios.bin's first
64 KiB (sd_get's limit) are compared; the fallback boot is the end-to-end check.

  vios_sd_refresh.py REPORT.json [--resident BIN] [--stub BIN [--stub-only]] [--port DEV]

Opening the port resets the board; it must boot to BOOT READY by itself
(user mode). Never writes flash.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vios_resident_board import ResidentBoard  # noqa: E402
from vios_sd_install import check_image, check_stub, manifest, parse_manifest, sidecar  # noqa: E402

PORT = '/dev/cu.usbserial-224130'
CHUNK = 100


def put(board, path, data, progress=None):
    """One handle, the whole file: sd_put without its 64 KiB cap."""
    with board.lock:
        handle = board._handle(path, 2)
        try:
            for offset in range(0, len(data), CHUNK):
                chunk = data[offset:offset + CHUNK]
                out = board._module_commands([f'HANDLE-PUT {handle} {chunk.hex().upper()}'])
                m = re.search(r'put (\d+)', out.get('output', ''))
                if not out.get('ok') or not m or int(m[1]) != len(chunk):
                    raise IOError(f'{path}: short write at {offset}: ' + out.get('output', '')[-120:])
                if progress and offset % (CHUNK * 100) == 0:
                    progress(path, offset, len(data))
        finally:
            board._module_commands([f'{handle} CLOSE DROP'])
    return {'path': path, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def readback(board, path, expected):
    got = board.sd_get(path)
    data = bytes.fromhex(got['hex']) if 'hex' in got else got.get('data', b'')
    n = min(len(expected), 65536)
    return {'path': path, 'read': len(data), 'compared': n, 'match': data[:n] == expected[:n]}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('report', type=Path)
    parser.add_argument('--resident', type=Path, default=Path(__file__).resolve().parents[1] / 'build/vios-resident.bin')
    parser.add_argument('--stub', type=Path)
    parser.add_argument('--stub-only', action='store_true', help='write only /mos/viosboot.bin (needs --stub)')
    parser.add_argument('--port', default=PORT)
    args = parser.parse_args()
    image = args.resident.read_bytes()
    check_image(image)
    man = manifest(image, sidecar(args.resident, '.gui', 0), sidecar(args.resident, '.baud', 1152000))
    stub = args.stub.read_bytes() if args.stub else None
    if stub is not None:
        check_stub(stub)
    report = {'resident': str(args.resident), 'manifest': parse_manifest(man), 'written': [], 'readback': []}
    started = time.time()

    def progress(path, offset, size):
        print(f'  {path}: {offset}/{size} ({time.time() - started:.0f} s)', flush=True)

    board = ResidentBoard(port=args.port, log=args.report.with_suffix('.serial.jsonl'), fast_console=True)
    try:
        report['connect'] = board.connect()
        report['before'] = {d: board.sd_list(d).get('entries') for d in ('/vios', '/mos')}
        old = board.sd_get('/mos/viosboot.bin')   # kept so a stub experiment can be undone
        report['stub_before'] = {'sha256': hashlib.sha256(bytes.fromhex(old['hex'])).hexdigest(), 'hex': old['hex']}
        old = board.sd_get('/vios/vios.man')
        report['manifest_before'] = old.get('hex')
        if not args.stub_only:
            report['written'].append(put(board, '/vios/vios.man', bytes(16)))
            report['written'].append(put(board, '/vios/vios.bin', image, progress))
            report['written'].append(put(board, '/vios/vios.man', man))
        if stub is not None:
            report['written'].append(put(board, '/mos/viosboot.bin', stub))
        report['readback'].append(readback(board, '/vios/vios.man', man))
        report['readback'].append(readback(board, '/vios/vios.bin', image))
        if stub is not None:
            report['readback'].append(readback(board, '/mos/viosboot.bin', stub))
        report['after'] = {d: board.sd_list(d).get('entries') for d in ('/vios', '/mos')}
        sizes = {e['name'].upper(): e['size'] for e in report['after']['/vios'] or []}
        report['size_ok'] = sizes.get('VIOS.BIN') == len(image) and sizes.get('VIOS.MAN') == 16
        report['ok'] = report['size_ok'] and all(r['match'] for r in report['readback'])
    finally:
        report['seconds'] = round(time.time() - started, 1)
        board.close()
        args.report.write_text(json.dumps(report, indent=1, default=str) + '\n')
    print(json.dumps({k: report[k] for k in ('manifest', 'readback', 'ok', 'seconds') if k in report}, indent=1))
    return 0 if report.get('ok') else 1


if __name__ == '__main__':
    sys.exit(main())

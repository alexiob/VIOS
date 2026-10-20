#!/usr/bin/env python3
"""Update the eZ80 flash (MOS) over ZDI through the VIOS ESP firmware.

docs/superpowers/specs/2026-10-19-vios-untethered-boot.md §16. Needs the ZDI
wire (dev mode) and exclusive use of the USB port (stop the harness first).

  vios_ez80_flash.py dry-run IMAGE     stage IMAGE in eZ80 RAM, check its CRC
                                       and report the current flash CRC (MD);
                                       nothing is written to flash
  vios_ez80_flash.py write IMAGE --approved
                                       the same, then erase and program the
                                       flash (MF) and verify it by readback

IMAGE is MOS.bin (at most 128 KB). Both commands halt the CPU, and restart
the ESP at the end, which reloads VIOS over ZDI (--no-restart keeps it halted).
A JSON report goes to stdout and, with --report, to a file.
"""
import argparse
import json
from pathlib import Path
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
FLASH_SIZE = 0x20000
IMAGE_ADDRESS = 0x050000
CHUNK = 256


def control(session, command, timeout=10, tries=5):
    for attempt in range(tries):
        try:
            return session.control(command, timeout=timeout).decode('ascii', 'replace').strip()
        except TimeoutError:
            if attempt == tries - 1:
                raise
            time.sleep(1)


def stage(session, image, log):
    """Halt the CPU and write image to IMAGE_ADDRESS in CRC-checked chunks."""
    halted = control(session, b'H')
    if not halted.startswith('REGS'):
        raise RuntimeError('halt failed: ' + halted)
    for offset in range(0, len(image), CHUNK):
        chunk = image[offset:offset + CHUNK]
        command = b'W%06X%04X%08X' % (IMAGE_ADDRESS + offset, len(chunk), zlib.crc32(chunk)) + chunk.hex().upper().encode()
        reply = control(session, command)
        if not reply.startswith('WROTE'):
            raise RuntimeError(f'RAM write at {IMAGE_ADDRESS + offset:06X} failed: {reply}')
    log['staged'] = len(image)


def run(session, image, write, restart=True):
    if not 0 < len(image) <= FLASH_SIZE:
        raise ValueError('image must be 1..128 KB')
    crc = zlib.crc32(image)
    log = {'image_bytes': len(image), 'image_crc32': f'{crc:08X}', 'write': write}
    boot = control(session, b'B')
    deadline = time.monotonic() + 60       # opening the port restarts the ESP: VIOS reloads first
    while boot == 'BOOT STARTING' and time.monotonic() < deadline:
        time.sleep(1)
        boot = control(session, b'B')
    log['boot'] = boot
    if boot != 'BOOT READY':
        raise RuntimeError('the board must have booted VIOS over the wire (BOOT READY), not ' + boot)
    stage(session, image, log)
    dry = control(session, b'MD%06X%08X' % (len(image), crc), timeout=60)
    log['md'] = dry
    if not dry.startswith('MD RAM ok'):
        raise RuntimeError('staged image CRC mismatch: ' + dry)
    log['flash_crc_before'] = dry.split()[-1]
    if write:
        result = control(session, b'MF%06X%08X' % (len(image), crc), timeout=180, tries=1)
        log['mf'] = result
        log['ok'] = result.startswith('MF OK')
    else:
        log['ok'] = True
    if restart:
        log['restart'] = control(session, b'Z')
    return log


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('command', choices=('dry-run', 'write'))
    parser.add_argument('image', type=Path)
    parser.add_argument('--approved', action='store_true', help='required for write: the owner approved this flash write')
    parser.add_argument('--port', default='/dev/cu.usbserial-224130')
    parser.add_argument('--report', type=Path)
    parser.add_argument('--no-restart', action='store_true')
    a = parser.parse_args(argv)
    if a.command == 'write' and not a.approved:
        parser.error('write needs --approved (an owner-approved flash write)')
    sys.path.insert(0, str(ROOT/'tools'))
    from serial_monitor import Session
    from vios_resident_transport import ResidentSession
    image = a.image.read_bytes()
    raw = Session(a.port, ROOT/'build/ez80-flash.jsonl')
    try:
        log = run(ResidentSession(raw), image, a.command == 'write', not a.no_restart)
    except (RuntimeError, ValueError, TimeoutError) as error:
        log = {'ok': False, 'error': str(error)}
    finally:
        raw.close()
    log['image'] = str(a.image)
    text = json.dumps(log, indent=2)
    print(text)
    if a.report:
        a.report.write_text(text + '\n')
    return 0 if log.get('ok') else 1


if __name__ == '__main__':
    sys.exit(main())

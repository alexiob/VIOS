#!/usr/bin/env python3
"""Load/read back the C monitor in eZ80 RAM and exercise its serial console."""
import argparse
import hashlib
import json
from pathlib import Path
import time
import zlib

from serial_monitor import validate_image
from zdi_backup import ask, load_probe, read_block


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='/dev/cu.usbserial-22430')
    parser.add_argument('--image', type=Path, default=Path('build/monitor-raw.bin'))
    parser.add_argument('--probe-image', type=Path, default=Path('build/zdi-loader.bin'))
    parser.add_argument('--log', type=Path, default=Path('docs/bring-up/monitor-hardware.jsonl'))
    args = parser.parse_args()
    data = args.image.read_bytes()
    validate_image(data)
    session = load_probe(args.port, args.probe_image, args.log)
    evidence = {'started_unix': time.time(), 'image': str(args.image),
                'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    try:
        identity = ask(session, b'I')
        print(identity.decode(), flush=True)
        if not identity.startswith(b'ID 0007 '):
            raise IOError('eZ80F92 not identified')
        evidence['identity'] = identity.decode()
        halted = ask(session, b'H')
        if not halted.startswith(b'REGS 98 '):
            raise IOError(f'CPU did not halt in ADL/MADL: {halted!r}')
        mapping = ask(session, b'D')
        if not mapping.startswith(b'CS0 04 0B 08 01'):
            raise IOError(f'unexpected RAM mapping: {mapping!r}')
        evidence['ram_mapping'] = mapping.decode()
        # Exercise the target's guards before any payload transfer.
        if ask(session, b'V0000000001') != b'ERR RAM range':
            raise IOError('target RAM read guard failed')
        if ask(session, b'W000000000100000000') != b'ERR RAM range':
            raise IOError('target RAM write guard failed')
        # Wrong CRC must reject even a safe RAM address, before a write.
        if ask(session, b'W04000000010000000000') != b'ERR CRC':
            raise IOError('target RAM write CRC guard failed')
        readback = bytearray()
        for offset in range(0, len(data), 256):
            block = data[offset:offset+256]
            address, count = 0x40000 + offset, len(block)
            crc = zlib.crc32(block)
            header = f'W{address:06X}{count:04X}{crc:08X}'.encode()
            response = ask(session, header + block.hex().upper().encode())
            expected = f'WROTE {address:06X} {count:04X} {crc:08X}'.encode()
            if response != expected:
                raise IOError(f'RAM write failed: {response!r}')
            got = read_block(session, address, count, operation='V')
            if got != block:
                raise IOError(f'RAM readback differs at {address:06x}')
            readback.extend(got)
        evidence['ram_readback_sha256'] = hashlib.sha256(readback).hexdigest()
        print('RAM readback verified:', evidence['ram_readback_sha256'], flush=True)
        session.write(b'X')
        boot = session.until(b'agon> ', timeout=10)
        evidence['boot'] = boot.decode('ascii', 'replace')
        print(boot.decode('ascii', 'replace'), end='', flush=True)
        for command, expected in [
            ('help', b'help | ping | info | echo TEXT | reboot\r\n'),
            ('ping', b'\r\npong\r\n'),
            ('info', b'raw UART0 115200 / freestanding C'),
            ('echo hardware-control-ok', b'\r\nhardware-control-ok\r\n'),
            ('echo agon> ', b'\r\nagon> \r\n'),
            ('x'*90, b'line too long; discarded\r\n'),
        ]:
            response = session.run_command(command)
            if expected not in response:
                raise IOError(f'wrong response for {command!r}: {response!r}')
            print(response.decode('ascii', 'replace'), end='', flush=True)
        session.write(b'pinx\x08g\r\n')
        response = session.until(b'\r\npong\r\nagon> ', timeout=5)
        evidence['backspace_crlf'] = response.decode('ascii', 'replace')
        session.command('reboot')
        restart = session.until(b'agon-monitor ', timeout=5)
        restart += session.until(b'agon> ', timeout=5) if not restart.endswith(b'agon> ') else b''
        evidence['restart'] = restart.decode('ascii', 'replace')
        if b'rebooting\r\n' not in restart:
            raise IOError(f'RAM restart response missing: {restart!r}')
        response = session.run_command('ping')
        if b'\r\npong\r\n' not in response:
            raise IOError('monitor did not answer after RAM restart')
        evidence['verified'] = True
        print('\nVerified real eZ80 execution, serial commands, editing/overflow and RAM restart.', flush=True)
    except BaseException:
        # Three Ctrl-] bytes halt the CPU and return the loader to debug mode.
        session.write(b'\x1d'*3)
        print('Debug recovery:', repr(session.listen(0.5)), flush=True)
        raise
    finally:
        evidence['finished_unix'] = time.time()
        args.log.with_suffix('.summary.json').write_text(json.dumps(evidence, indent=2)+'\n')
        session.close()


if __name__ == '__main__':
    main()

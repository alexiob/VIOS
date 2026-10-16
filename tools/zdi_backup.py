#!/usr/bin/env python3
"""Load the ESP32 RAM probe and independently read eZ80F92 flash twice.

One serial connection is kept open throughout to avoid modem-line reset pulses.
This host tool has no flash write/erase operation. Every 256-byte response has
address/count and IEEE CRC32 checks. The CPU is left halted after the backup.
"""
import argparse
import hashlib
import json
from pathlib import Path
import time
import zlib

from serial_monitor import Session


def ask(session, command):
    session.write(command)
    return session.until(b'\r\n', timeout=5).strip()


def read_block(session, address, count, operation='R'):
    command = f'{operation}{address:06X}{count:04X}'.encode('ascii')
    raw = ask(session, command)
    fields = raw.split()
    if len(fields) != 5 or fields[0] != b'MEM':
        raise IOError(f'unexpected response: {raw!r}')
    got_address, got_count, crc = (int(x, 16) for x in fields[1:4])
    data = bytes.fromhex(fields[4].decode('ascii'))
    if (got_address, got_count, len(data)) != (address, count, count):
        raise IOError('flash response address/count mismatch')
    if zlib.crc32(data) != crc:
        raise IOError(f'flash response CRC mismatch at {address:06x}')
    return data


def load_probe(port, image, log):
    import esptool
    esp = esptool.detect_chip(port, 115200, connect_attempts=2)
    try:
        if bytes(esp.read_mac()) != bytes.fromhex('0c8b95aa174c'):
            raise IOError('connected ESP32 is not the identified Agon board')
        esptool.cmds.load_ram(esp, str(image))
    except BaseException:
        esp._port.close()
        raise
    # Reuse esptool's open port without touching DTR/RTS or reopening it.
    session = Session.__new__(Session)
    session.port = esp._port
    session.port.timeout = 0.1
    session.log = Path(log).open('a', encoding='utf-8')
    session.received = bytearray()
    session.listen(0.2)
    return session


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='/dev/cu.usbserial-22430')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--probe-image', type=Path, default=Path('build/zdi-probe.bin'))
    parser.add_argument('--base', type=lambda v: int(v, 0), default=0)
    args = parser.parse_args()
    if args.base & 0x1ffff or not 0 <= args.base <= 0xfe0000:
        parser.error('flash base must be aligned to 128 KiB in 24-bit space')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    outputs = [args.output, args.output.with_suffix('.verify.bin')]
    if any(p.exists() for p in outputs):
        parser.error('output already exists; preserve it and choose a fresh name')
    session = load_probe(args.port, args.probe_image, args.output.with_suffix('.serial.jsonl'))
    metadata = {'flash_base': args.base, 'size': 0x20000,
                'started_unix': time.time(), 'passes': []}
    try:
        identity = ask(session, b'I').decode('ascii')
        if not identity.startswith('ID 0007 '):
            raise IOError(f'CPU identity mismatch: {identity}')
        metadata['identity'] = identity
        metadata['halt_snapshot'] = ask(session, b'H').decode('ascii')
        mapping = ask(session, b'F').decode('ascii')
        metadata['mapping'] = mapping
        parts = mapping.split()
        if (len(parts) != 7 or parts[:2] != ['MAP', 'FLASH']
                or int(parts[2], 16) << 16 != args.base or not int(parts[3], 16) & 8):
            raise IOError(f'unexpected flash mapping: {mapping}')
        metadata['registers_before'] = ask(session, b'S').decode('ascii')
        if not metadata['registers_before'].startswith('REGS 98 '):
            raise IOError('require the previously halted ADL/MADL CPU')
        for pass_number, path in enumerate(outputs, 1):
            digest = hashlib.sha256()
            with path.open('xb') as out:
                for offset in range(0, 0x20000, 256):
                    data = read_block(session, args.base + offset, 256)
                    out.write(data)
                    digest.update(data)
                    if offset % 0x4000 == 0:
                        print(f'Pass {pass_number}: {offset // 1024}/128 KiB', flush=True)
            metadata['passes'].append({'path': str(path), 'size': path.stat().st_size,
                                       'sha256': digest.hexdigest()})
            print(f'Pass {pass_number}: 128/128 KiB, SHA256 {digest.hexdigest()}', flush=True)
        if outputs[0].read_bytes() != outputs[1].read_bytes():
            raise IOError('independent flash reads differ; backup is not verified')
        metadata['registers_after'] = ask(session, b'S').decode('ascii')
        if metadata['registers_before'] != metadata['registers_after']:
            raise IOError('CPU register snapshot changed during memory reads')
        metadata['verified'] = True
        args.output.with_suffix('.bin.sha256').write_text(
            metadata['passes'][0]['sha256'] + '  ' + args.output.name + '\n')
        print('Verified: both complete flash reads are identical; CPU remains halted.', flush=True)
    finally:
        metadata['finished_unix'] = time.time()
        args.output.with_suffix('.json').write_text(json.dumps(metadata, indent=2) + '\n')
        session.close()


if __name__ == '__main__':
    main()

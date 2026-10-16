#!/usr/bin/env python3
"""Bounded RAM upload and console control; never writes board flash or the SD."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import secrets
import time

RAM_START = 0x040000
IMAGE_LIMIT = 0x0AF000  # Reserve the top 4 KiB for the stack.


def validate_image(data, start=RAM_START):
    if not data or start < RAM_START or start + len(data) > IMAGE_LIMIT:
        raise ValueError('image overlaps firmware/loader/stack or is empty')
    if data[64:69] != b'MOS\x00\x01':
        raise ValueError('missing ADL MOS bootstrap header')


def record(address, kind, data):
    raw = bytes((len(data), address >> 8, address & 255, kind)) + data
    return b':' + (raw + bytes((-sum(raw) & 255,))).hex().upper().encode('ascii')


def ihex_records(data, start=RAM_START, chunk_size=16):
    if not 1 <= chunk_size <= 255:
        raise ValueError('invalid Intel HEX chunk size')
    upper = None
    offset = 0
    while offset < len(data):
        address = start + offset
        page = address >> 16
        if page != upper:
            upper = page
            yield record(0, 4, page.to_bytes(2, 'big'))
        count = min(chunk_size, len(data) - offset, 0x10000 - (address & 65535))
        yield record(address & 65535, 0, data[offset:offset + count])
        offset += count
    yield record(0, 1, b'')


def command_nonce():
    return ('sync-' + secrets.token_hex(8)).encode('ascii')


class Session:
    def __init__(self, port, log):
        import serial
        self.log = open(log, 'a', encoding='utf-8')
        self.port = serial.Serial(port=None, baudrate=115200, timeout=0.1,
                                  write_timeout=3, exclusive=True)
        self.port.port = port
        self.port.dtr = False
        self.port.rts = False
        self.port.rtscts = False
        self.port.dsrdtr = False
        try:
            self.port.open()
        except BaseException:
            self.log.close()
            raise
        self.received = bytearray()

    def event(self, direction, data):
        self.log.write(json.dumps({'time': time.time(), 'direction': direction,
                                   'hex': data.hex(),
                                   'text': data.decode('utf-8', 'replace')}) + '\n')
        self.log.flush()

    def write(self, data):
        self.event('tx', data)
        if self.port.write(data) != len(data):
            raise IOError('short serial write')
        self.port.flush()

    def read(self, count=None):
        data = self.port.read(count if count is not None else max(1, self.port.in_waiting))
        if data:
            self.event('rx', data)
            self.received.extend(data)
        return data

    def exact(self, count, timeout=3):
        end = time.monotonic() + timeout
        data = bytearray()
        while len(data) < count and time.monotonic() < end:
            data.extend(self.read(count - len(data)))
        if len(data) != count:
            raise TimeoutError(f'expected {count} serial bytes, received {bytes(data)!r}')
        return bytes(data)

    def until(self, marker, timeout=10):
        end = time.monotonic() + timeout
        data = bytearray()
        while time.monotonic() < end:
            data.extend(self.read())
            if marker in data:
                return bytes(data)
        raise TimeoutError(f'no {marker!r}; received {bytes(data)!r}')

    def listen(self, seconds):
        end = time.monotonic() + seconds
        data = bytearray()
        while time.monotonic() < end:
            data.extend(self.read())
        return bytes(data)

    def command(self, text):
        if '\r' in text or '\n' in text:
            raise ValueError('send one command at a time')
        # Pace keyboard injection for older VDP versions.
        for byte in text.encode('ascii') + b'\r':
            self.write(bytes((byte,)))
            time.sleep(0.003)

    def run_command(self, text, timeout=10):
        # A prompt can occur in echoed input or output. A fresh echo marker
        # proves the preceding command and its response have both completed.
        nonce = command_nonce()
        self.command(text)
        self.command('echo ' + nonce.decode('ascii'))
        data = self.until(b'\r\n' + nonce + b'\r\nagon> ', timeout)
        return data.rsplit(b'echo ' + nonce + b'\r\n', 1)[0]

    def close(self):
        self.port.close()
        self.log.close()


def upload(session, data, start=RAM_START):
    validate_image(data, start)
    session.command('hexload vdp')
    session.until(b'Receiving Intel HEX records', 10)
    session.listen(0.3)  # Consume the receiver banner before the binary CRC reply.
    before = len(session.received)
    for line in ihex_records(data, start):
        session.write(line)
        # This sender targets the pinned legacy VDP 1.04 protocol. Its host
        # link has no flow control; allow the CPU/VDP handshake to finish.
        time.sleep(0.03)
    session.until(b'VDP done', 10)
    tail = bytes(session.received[before:])
    if re.search(rb'ERROR|error\(s\)|Aborted|Invalid', tail):
        raise IOError(f'loader reported failure: {tail!r}')
    print('Uploaded', len(data), 'bytes; SHA256', hashlib.sha256(data).hexdigest(),
          'verification: legacy Intel HEX/CPU checksums (not CRC32)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='/dev/cu.usbserial-22430')
    parser.add_argument('--log', default='docs/bring-up/serial-session.jsonl')
    parser.add_argument('--listen', type=float, default=0)
    parser.add_argument('--upload', type=Path)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--command', action='append', default=[])
    args = parser.parse_args()
    data = args.upload.read_bytes() if args.upload else None
    if data is not None:
        validate_image(data)
    session = Session(args.port, args.log)
    try:
        if args.listen:
            print(session.listen(args.listen).decode('utf-8', 'replace'))
        if data is not None:
            upload(session, data)
        if args.run:
            session.command('run &40000')
            print(session.until(b'agon> ', 5).decode('utf-8', 'replace'))
        for command in args.command:
            if command == 'reboot':
                session.command(command)
                result = session.listen(3)
            else:
                result = session.run_command(command)
            print(result.decode('utf-8', 'replace'))
    finally:
        session.close()


if __name__ == '__main__':
    main()

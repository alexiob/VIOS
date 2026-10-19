#!/usr/bin/env python3
"""ESP32 firmware A/B over esptool: status, install to the inactive slot, rollback.

The ESP flash holds two app slots (app0/app1) and an otadata partition of two
4 KB sectors; the bootloader runs the slot named by the valid entry with the
highest sequence number. `install` backs up, writes the new image to the slot
that is not running, verifies it, then adds one otadata entry in the sector
that does not hold the active entry, so the old entry stays the fallback.

The entry is VALID with a "VIOS-TRIAL" marker in its unused label field, never
NEW: this board's rollback bootloader erases NEW/PENDING_VERIFY entries
without rewriting them (observed 2026-10-19), but never writes VALID ones.
The new firmware treats the marker as a trial: it clears it once the CPU it
boots reports health, otherwise marks itself INVALID and the old slot boots.
`install` boots the board by opening the serial port (which resets it) and
waits for that verdict on the USB event channel.

Every esptool call and every port open resets the board.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import zlib

ESPTOOL = Path(__file__).resolve().parents[1] / '.venv' / 'bin' / 'esptool'
PARTITION_TABLE = 0x8000
SECTOR = 0x1000
UNDEFINED = 0xFFFFFFFF
STATES = {0: 'new', 1: 'pending', 2: 'valid', 3: 'invalid', 4: 'aborted', UNDEFINED: 'undefined'}
NEW, VALID = 0, 2
TRIAL_LABEL = b'VIOS-TRIAL'


def parse_partitions(data):
    """Return {label: (type, subtype, offset, size)} from a partition table."""
    table = {}
    for i in range(0, len(data) - 31, 32):
        entry = data[i:i + 32]
        if entry[:2] != b'\xaa\x50':
            break
        kind, sub, offset, size = struct.unpack('<BBII', entry[2:12])
        table[entry[12:28].rstrip(b'\0').decode('ascii')] = (kind, sub, offset, size)
    return table


def app_slots(table):
    """OTA app slots in ota index order (subtype 0x10 + n)."""
    slots = sorted((sub - 0x10, label) for label, (kind, sub, _, _) in table.items() if kind == 0 and 0x10 <= sub < 0x20)
    if [n for n, _ in slots] != list(range(len(slots))) or len(slots) < 2:
        raise ValueError(f'expected consecutive OTA slots, got {slots}')
    return [label for _, label in slots]


def entry_crc(seq):
    return zlib.crc32(struct.pack('<I', seq), 0xFFFFFFFF)


def decode_otadata(data):
    """Both sectors' entries: seq, label, state, crc and whether the bootloader accepts it."""
    entries = []
    for sector in range(2):
        seq, label, state, crc = struct.unpack('<I20sII', data[sector * SECTOR:sector * SECTOR + 32])
        valid = seq != UNDEFINED and crc == entry_crc(seq) and state not in (3, 4)
        entries.append({'sector': sector, 'seq': seq, 'state': STATES.get(state, hex(state)),
                        'crc_ok': crc == entry_crc(seq), 'valid': valid, 'trial': label.startswith(TRIAL_LABEL)})
    return entries


def active_entry(entries):
    valid = [e for e in entries if e['valid']]
    return max(valid, key=lambda e: e['seq']) if valid else None


def selected_slot(entries, slot_count):
    """OTA index the bootloader boots: highest valid seq, (seq-1) mod slots; none -> slot 0."""
    active = active_entry(entries)
    return 0 if active is None else (active['seq'] - 1) % slot_count


def select_otadata(data, slot, slot_count, trial):
    """New otadata bytes selecting `slot`, written into the non-active sector.

    Returns (sector, new 8 KB image). The active entry is left untouched, so it
    stays the fallback the bootloader uses if the new entry is ever invalid.
    """
    entries = decode_otadata(data)
    active = active_entry(entries)
    top = max([e['seq'] for e in entries if e['crc_ok'] and e['seq'] != UNDEFINED] or [0])
    seq = top + 1
    while (seq - 1) % slot_count != slot:
        seq += 1
    sector = 0 if active is None else 1 - active['sector']
    label = (TRIAL_LABEL if trial else b'').ljust(20, b'\xff')
    entry = struct.pack('<I20sII', seq, label, VALID, entry_crc(seq))
    image = bytearray(data)
    image[sector * SECTOR:(sector + 1) * SECTOR] = entry + b'\xff' * (SECTOR - 32)
    return sector, bytes(image)


def check_image(data, size):
    if len(data) < 24 or data[0] != 0xE9:
        raise ValueError('not an ESP32 app image (magic 0xE9)')
    if len(data) > size:
        raise ValueError(f'image {len(data)} B exceeds slot {size} B')


class Flash:
    def __init__(self, port, baud):
        self.port, self.baud = port, baud

    def _run(self, *args):
        command = [str(ESPTOOL), '--chip', 'esp32', '--port', self.port, '--baud', str(self.baud), '--after', 'hard-reset', *args]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f'esptool {args[0]} failed:\n{result.stdout[-800:]}{result.stderr[-800:]}')
        return result.stdout

    def read(self, offset, size, chunk=0x10000, attempts=3):
        """Read in chunks; the USB link corrupts long reads. esptool checks
        each read's digest, so a bad chunk fails loudly and only reads retry."""
        data = bytearray()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'read.bin'
            for at in range(offset, offset + size, chunk):
                n = min(chunk, offset + size - at)
                for attempt in range(attempts):
                    try:
                        self._run('read-flash', hex(at), hex(n), str(path))
                        break
                    except RuntimeError:
                        if attempt == attempts - 1:
                            raise
                        print(f'read {at:#x} failed; retrying read', file=sys.stderr)
                data += path.read_bytes()
        return bytes(data)

    def write(self, offset, data, readback=True):
        """Write, require esptool's flash MD5 check, optionally read back; never retried.

        readback=False is for otadata: a second esptool call would reset the
        board inside the new image's trial window.
        """
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'write.bin'
            path.write_bytes(data)
            out = self._run('write-flash', hex(offset), str(path))
        if 'Hash of data verified.' not in out:
            raise RuntimeError(f'esptool did not verify the write at {offset:#x}; stopping (no retry)')
        if readback and self.read(offset, len(data)) != data:
            raise RuntimeError(f'read-back mismatch at {offset:#x}; stopping (no retry)')


def layout(flash):
    table = parse_partitions(flash.read(PARTITION_TABLE, 0xC00))
    if 'otadata' not in table:
        raise ValueError('no otadata partition')
    return table, app_slots(table)


def status(flash):
    table, slots = layout(flash)
    otadata = flash.read(table['otadata'][2], 2 * SECTOR)
    entries = decode_otadata(otadata)
    return {'slots': {s: {'offset': hex(table[s][2]), 'size': table[s][3]} for s in slots},
            'otadata': entries, 'boots': slots[selected_slot(entries, len(slots))]}


def watch_events(port, seconds):
    """Return the firmware's verdict from USB channel 3 events, or None on timeout."""
    import serial
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from vios_wire import Decoder
    link = serial.Serial(port=None, baudrate=115200, timeout=0.2, exclusive=True)
    link.port, link.dtr, link.rts = port, False, False
    link.open()
    decoder, seen, deadline = Decoder(), [], time.monotonic() + seconds
    try:
        while time.monotonic() < deadline:
            for packet in decoder.feed(link.read(512)):
                if packet.channel != 3:
                    continue
                line = packet.payload.decode('ascii', 'replace').strip()
                seen.append(line)
                print('event:', line, flush=True)
                if line == 'FIRMWARE confirmed' or line.startswith('FIRMWARE rollback') or line.startswith('ERR FIRMWARE'):
                    return line, seen
    finally:
        link.close()
    return None, seen


def install(flash, image_path, backup_dir, wait):
    image = Path(image_path).read_bytes()
    table, slots = layout(flash)
    otadata_offset = table['otadata'][2]
    otadata = flash.read(otadata_offset, 2 * SECTOR)
    current = selected_slot(decode_otadata(otadata), len(slots))
    target = (current + 1) % len(slots)
    _, offset, size = table[slots[target]][1:]
    check_image(image, size)
    backup_dir = Path(backup_dir)
    backup_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime('%Y%m%d-%H%M%S')
    (backup_dir / f'otadata-{stamp}.bin').write_bytes(otadata)
    previous = flash.read(offset, size)
    (backup_dir / f'{slots[target]}-{stamp}.bin').write_bytes(previous)
    report = {'running_before': slots[current], 'target': slots[target], 'image': str(image_path),
              'image_sha256': hashlib.sha256(image).hexdigest(), 'backup_dir': str(backup_dir), 'stamp': stamp}
    flash.write(offset, image)
    sector, new = select_otadata(otadata, target, len(slots), trial=True)
    # Readback is safe (a VALID entry is never rewritten by the bootloader) and
    # leaves the board running the trial image's first boot.
    flash.write(otadata_offset + sector * SECTOR, new[sector * SECTOR:(sector + 1) * SECTOR])
    report['otadata_sector'] = sector
    report['otadata_entry'] = decode_otadata(new)[sector]
    if wait:
        verdict, events = watch_events(flash.port, wait)
        report.update(verdict=verdict, events=events)
    return report


def rollback(flash, slot_label=None, restore=None):
    table, slots = layout(flash)
    otadata_offset = table['otadata'][2]
    if restore:
        data = Path(restore).read_bytes()
        if len(data) != 2 * SECTOR:
            raise ValueError('otadata backup must be 8192 bytes')
        flash.write(otadata_offset, data)
        return {'restored': str(restore), 'otadata': decode_otadata(data)}
    otadata = flash.read(otadata_offset, 2 * SECTOR)
    target = slots.index(slot_label) if slot_label else (selected_slot(decode_otadata(otadata), len(slots)) + 1) % len(slots)
    sector, new = select_otadata(otadata, target, len(slots), trial=False)
    flash.write(otadata_offset + sector * SECTOR, new[sector * SECTOR:(sector + 1) * SECTOR])
    return {'selected': slots[target], 'otadata': decode_otadata(new)}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--port', default='/dev/cu.usbserial-224130')
    p.add_argument('--baud', type=int, default=115200)  # faster rates hit serial noise on this board
    sub = p.add_subparsers(dest='command', required=True)
    sub.add_parser('status', help='read partition table and otadata (resets the board)')
    i = sub.add_parser('install', help='write FIRMWARE to the inactive slot and boot it on trial')
    i.add_argument('firmware')
    i.add_argument('--backup-dir', required=True)
    i.add_argument('--wait', type=int, default=180, help='seconds to wait for the firmware verdict (0: do not wait)')
    r = sub.add_parser('rollback', help='select a slot (default: the other one), or restore an otadata backup')
    r.add_argument('--slot')
    r.add_argument('--restore', help='exact 8 KB otadata backup to write back')
    a = p.parse_args()
    flash = Flash(a.port, a.baud)
    if a.command == 'status':
        result = status(flash)
    elif a.command == 'install':
        result = install(flash, a.firmware, a.backup_dir, a.wait)
    else:
        result = rollback(flash, a.slot, a.restore)
    print(json.dumps(result, indent=2))
    if a.command == 'install' and a.wait and result.get('verdict') != 'FIRMWARE confirmed':
        sys.exit(1)


if __name__ == '__main__':
    main()

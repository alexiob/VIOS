#!/usr/bin/env python3
"""Stamp a resident binary with its own build identity.

kernel/vios_boot.c holds `vios_build_stamp = {'STMP' marker, 0}`. This writes
the CRC-32 of the binary, taken with the stamp word still zero, into that word.
The boot-module cache is keyed by it (docs/superpowers/specs/
2026-10-19-vios-boot-speed.md, Phase 3), so a cache compiled by one build is
never restored into another. An ELF loaded without this step reads 0, which
the resident treats as "do not cache".

  vios_stamp.py build/vios-resident.bin
"""
import argparse
from pathlib import Path
import struct
import sys
import zlib

MARKER = struct.pack('<I', 0x504D5453)  # 'STMP'


def stamp(data):
    """Return (stamped bytes, stamp). Raise ValueError unless exactly one slot."""
    data = bytearray(data)
    found = [i for i in range(len(data) - 7) if data[i:i + 4] == MARKER]
    unstamped = [i for i in found if data[i + 4:i + 8] == bytes(4)]
    if len(found) != 1:
        raise ValueError(f'expected one STMP marker, found {len(found)}')
    at = found[0] + 4
    data[at:at + 4] = bytes(4)            # restamping gives the same value
    value = zlib.crc32(data) or 1         # 0 means unstamped
    data[at:at + 4] = struct.pack('<I', value)
    return bytes(data), value, not unstamped


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    p.add_argument('binary', type=Path)
    a = p.parse_args(argv)
    data, value, restamped = stamp(a.binary.read_bytes())
    a.binary.write_bytes(data)
    print(f'{a.binary}: build stamp {value:08X}' + (' (restamped)' if restamped else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Make a deliberately bad ESP image for the U6 fallback test (resident mismatch).

docs/superpowers/specs/2026-10-19-vios-untethered-boot.md §16.10. The input is
a built VIOS ESP application image; the output is the same image with the
embedded resident's CRC-32 constant (`vios_image_crc`, which the E8 manifest
reports) changed, so the manifest no longer matches the blocks it serves. Every
block still carries a correct block CRC, so the stub loads the whole image and
only its whole-image CRC check fails: it must then refuse to jump and fall back
to /vios/vios.bin. Nothing else in the image changes. The ESP image checksum
and the appended SHA-256 are recomputed so the bootloader still runs it.

  vios_esp_fault_image.py IN.bin OUT.bin --resident build/vios-resident.bin

Refuses unless the resident's CRC occurs exactly once in the image.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib

MAGIC = 0xE9
CHECKSUM_SEED = 0xEF


def segments_end(image):
    """Offset just past the last segment (24-byte extended header after 8)."""
    if image[0] != MAGIC:
        raise ValueError('not an ESP application image')
    count, offset = image[1], 24
    for _ in range(count):
        _, size = struct.unpack_from('<II', image, offset)
        offset += 8 + size
    return offset


def checksum(image):
    count, offset, value = image[1], 24, CHECKSUM_SEED
    for _ in range(count):
        _, size = struct.unpack_from('<II', image, offset)
        for byte in image[offset + 8:offset + 8 + size]:
            value ^= byte
        offset += 8 + size
    return value


def fault(image, crc):
    data = bytearray(image)
    needle = struct.pack('<I', crc)
    at = data.find(needle)
    if at < 0 or data.find(needle, at + 1) >= 0:
        raise ValueError(f'the resident CRC {crc:08x} must occur exactly once in the image')
    end = segments_end(data)
    if at >= end:
        raise ValueError('the CRC is outside the image segments')
    bad = crc ^ 0x00000001
    data[at:at + 4] = struct.pack('<I', bad)
    # The checksum byte is the last byte of the 16-byte-aligned padding after the segments.
    check_at = (end + 16) & ~15
    check_at -= 1
    data[check_at] = checksum(data)
    if data[23] == 1:  # hash appended: SHA-256 of everything before it
        data[check_at + 1:check_at + 33] = hashlib.sha256(bytes(data[:check_at + 1])).digest()
    return bytes(data), at, bad


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--resident', type=Path, required=True)
    args = parser.parse_args()
    resident = args.resident.read_bytes()
    crc = zlib.crc32(resident)
    image = args.input.read_bytes()
    if checksum(image) != image[((segments_end(image) + 16) & ~15) - 1]:
        raise SystemExit('input image checksum does not verify; refusing')
    out, at, bad = fault(image, crc)
    args.output.write_bytes(out)
    print(json.dumps({'input': str(args.input), 'output': str(args.output), 'resident_crc32': f'{crc:08x}',
                      'manifest_crc32': f'{bad:08x}', 'patched_offset': at, 'size': len(out),
                      'sha256': hashlib.sha256(out).hexdigest()}, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())

"""Resident build stamp (tools/vios_stamp.py): the boot-module cache key."""
import pathlib
import struct
import sys
import unittest
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
from vios_stamp import MARKER, stamp  # noqa: E402


class StampTests(unittest.TestCase):
    def test_stamp_is_the_crc_with_the_slot_zero_and_is_stable(self):
        image = b'\x00code' + MARKER + bytes(4) + b'data\x01'
        stamped, value, restamped = stamp(image)
        self.assertFalse(restamped)
        self.assertEqual(value, zlib.crc32(image))
        at = image.index(MARKER) + 4
        self.assertEqual(stamped[at:at + 4], struct.pack('<I', value))
        self.assertEqual(stamped[:at] + stamped[at + 4:], image[:at] + image[at + 4:])
        again, value2, restamped = stamp(stamped)
        self.assertEqual((again, value2, restamped), (stamped, value, True))

    def test_needs_exactly_one_marker(self):
        for image in (b'no marker here', MARKER + bytes(4) + MARKER + bytes(4)):
            with self.assertRaises(ValueError):
                stamp(image)


if __name__ == '__main__':
    unittest.main()

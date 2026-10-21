"""ESP firmware A/B otadata logic against the board's real backup. No serial."""
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import vios_firmware_ab as ab

BOARD_OTADATA = ROOT / 'backups' / 'ab-2026-10-19' / 'otadata-before.bin'
PARTITIONS = ROOT / '.tools' / 'firmware' / 'olimex-vdp-1.04' / 'partitions.bin'


def entry(seq, state=ab.UNDEFINED, crc=None):
    crc = ab.entry_crc(seq) if crc is None else crc
    return struct.pack('<I20sII', seq, b'\xff' * 20, state, crc) + b'\xff' * (ab.SECTOR - 32)


class FirmwareABTest(unittest.TestCase):
    @unittest.skipUnless(PARTITIONS.exists(), 'the vendor firmware backup in .tools/ is local only (not in CI)')
    def test_partition_table_slots(self):
        table = ab.parse_partitions(PARTITIONS.read_bytes())
        self.assertEqual(ab.app_slots(table), ['app0', 'app1'])
        self.assertEqual(table['app1'][2:], (0x150000, 0x140000))
        self.assertEqual(table['otadata'][2:], (0xE000, 0x2000))

    def test_board_otadata_decodes_and_boots_app0(self):
        data = BOARD_OTADATA.read_bytes()
        entries = ab.decode_otadata(data)
        self.assertTrue(entries[0]['crc_ok'] and entries[0]['valid'] and entries[0]['seq'] == 1)
        self.assertEqual(ab.selected_slot(entries, 2), 0)

    def test_install_entry_goes_to_inactive_sector_and_selects_app1(self):
        data = BOARD_OTADATA.read_bytes()
        sector, new = ab.select_otadata(data, 1, 2, trial=True)
        active = ab.active_entry(ab.decode_otadata(data))['sector']
        self.assertNotEqual(sector, active)
        self.assertEqual(new[active * ab.SECTOR:(active + 1) * ab.SECTOR], data[active * ab.SECTOR:(active + 1) * ab.SECTOR])
        entries = ab.decode_otadata(new)
        # Never NEW/PENDING: this board's bootloader erases those without rewriting.
        self.assertEqual((entries[sector]['state'], entries[sector]['trial']), ('valid', True))
        self.assertEqual(ab.selected_slot(entries, 2), 1)
        # The trial image marking itself invalid falls back to the untouched entry.
        invalid = bytearray(new)
        invalid[sector * ab.SECTOR + 24:sector * ab.SECTOR + 28] = struct.pack('<I', 3)
        self.assertEqual(ab.selected_slot(ab.decode_otadata(bytes(invalid)), 2), 0)

    def test_rollback_selects_other_slot_with_higher_seq(self):
        data = entry(1) + entry(2, ab.VALID)          # app1 active (seq 2)
        sector, new = ab.select_otadata(data, 0, 2, trial=False)
        self.assertEqual(sector, 0)
        entries = ab.decode_otadata(new)
        self.assertEqual((entries[0]['seq'], ab.selected_slot(entries, 2), entries[0]['trial']), (3, 0, False))

    def test_bad_crc_and_blank_entries(self):
        blank = b'\xff' * (2 * ab.SECTOR)
        self.assertEqual(ab.selected_slot(ab.decode_otadata(blank), 2), 0)
        sector, new = ab.select_otadata(blank, 1, 2, trial=True)
        self.assertEqual((sector, ab.decode_otadata(new)[0]['seq']), (0, 2))
        corrupt = entry(5, crc=0) + entry(1)
        self.assertEqual(ab.active_entry(ab.decode_otadata(corrupt))['seq'], 1)

    def test_board_after_self_rollback(self):
        # Observed on the board: seq 2 marked INVALID by app1 -> app0 boots; next install selects app1 with seq 4.
        data = entry(1) + entry(2, 3)
        self.assertEqual(ab.selected_slot(ab.decode_otadata(data), 2), 0)
        sector, new = ab.select_otadata(data, 1, 2, trial=True)
        self.assertEqual((sector, ab.decode_otadata(new)[1]['seq'], ab.selected_slot(ab.decode_otadata(new), 2)), (1, 4, 1))

    def test_image_checks(self):
        with self.assertRaises(ValueError):
            ab.check_image(b'\0' * 64, 1 << 20)
        with self.assertRaises(ValueError):
            ab.check_image(b'\xe9' + b'\0' * 99, 50)
        ab.check_image(b'\xe9' + b'\0' * 99, 100)


if __name__ == '__main__':
    unittest.main()

"""tools/vios_ez80_flash.py against a fake ESP control channel (never opens hardware)."""
from pathlib import Path
import sys
import unittest
import zlib
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import vios_ez80_flash as tool


class FakeBoard:
    def __init__(self, boot='BOOT READY', flash=b'\x11' * 0x20000):
        self.ram, self.flash, self.boot = bytearray(0x100000), bytearray(flash), boot
        self.commands, self.halted = [], False
    def control(self, command, timeout=10):
        self.commands.append(command[:2])
        text = command.decode()
        if text == 'B':
            return self.boot.encode() + b'\r\n'
        if text == 'H':
            self.halted = True
            return b'REGS 98 000000\r\n'
        if text[0] == 'W':
            assert self.halted
            address, count, crc = int(text[1:7], 16), int(text[7:11], 16), int(text[11:19], 16)
            data = bytes.fromhex(text[19:])
            assert len(data) == count <= 256 and zlib.crc32(data) == crc
            self.ram[address:address+count] = data
            return b'WROTE\r\n'
        if text[:2] in ('MD', 'MF'):
            n, want = int(text[2:8], 16), int(text[8:16], 16)
            ram = zlib.crc32(bytes(self.ram[0x50000:0x50000+n]))
            if text[:2] == 'MD':
                return b'MD RAM %s %08X FLASH %08X\r\n' % (b'ok' if ram == want else b'BAD', ram, zlib.crc32(bytes(self.flash)))
            self.flash[:] = b'\xff' * 0x20000
            self.flash[:n] = self.ram[0x50000:0x50000+n]
            return b'MF OK 4000 ms FLASH %08X REST erased\r\n' % zlib.crc32(bytes(self.flash[:n]))
        if text == 'Z':
            return b'RESTARTING\r\n'
        raise AssertionError(text)


class Ez80FlashToolTests(unittest.TestCase):
    image = bytes((i * 31) & 255 for i in range(108490))

    def test_dry_run_stages_and_checks_but_never_writes(self):
        board = FakeBoard()
        log = tool.run(board, self.image, write=False)
        self.assertTrue(log['ok'])
        self.assertNotIn(b'MF', board.commands)
        self.assertEqual(bytes(board.flash), b'\x11' * 0x20000)
        self.assertEqual(bytes(board.ram[0x50000:0x50000+len(self.image)]), self.image)
        self.assertEqual(log['flash_crc_before'], f'{zlib.crc32(b"\x11" * 0x20000):08X}')
        self.assertEqual(board.commands[-1], b'Z')

    def test_write_programs_after_a_good_stage(self):
        board = FakeBoard()
        log = tool.run(board, self.image, write=True)
        self.assertTrue(log['ok'], log)
        self.assertEqual(bytes(board.flash[:len(self.image)]), self.image)

    def test_refuses_without_a_wired_boot_and_bad_sizes(self):
        with self.assertRaises(RuntimeError):
            tool.run(FakeBoard(boot='BOOT USER waiting for the MOS stub'), self.image, write=True)
        with self.assertRaises(ValueError):
            tool.run(FakeBoard(), b'\0' * 0x20001, write=False)

    def test_write_needs_approval_on_the_command_line(self):
        with self.assertRaises(SystemExit):
            tool.main(['write', str(ROOT/'README.md')])


if __name__ == '__main__':
    unittest.main()

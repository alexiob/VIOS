"""Execute the built boot CRC stage's real instruction bytes; no hardware."""
from pathlib import Path
import os
import subprocess
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]


def planes():
    """Reflected CRC32 table split into four 256-byte planes (byte k of each entry)."""
    table = []
    for n in range(256):
        c = n
        for _ in range(8):
            c = (c >> 1) ^ (0xEDB88320 if c & 1 else 0)
        table.append(c)
    return [bytes((t >> (8 * k)) & 0xFF for t in table) for k in range(4)]


class CrcMachine:
    """Small ADL interpreter for exactly the opcodes the stage uses; anything else fails."""
    def __init__(self, code, base, image, table_base):
        self.base, self.end = base, base + len(code)
        self.memory = bytearray(1 << 24)
        self.memory[base:base + len(code)] = code
        self.memory[0x40000:0x40000 + len(image)] = image
        for k, plane in enumerate(planes()):
            self.memory[table_base + 256 * k:table_base + 256 * (k + 1)] = plane
        self.meta = base + 0x100
        blocks = len(image) // 256
        self.memory[self.meta + 3:self.meta + 5] = blocks.to_bytes(2, 'little')
        self.pc, self.ix, self.iy, self.hl = base, 0x40000, self.meta, table_base
        self.a = self.b = self.c = self.d = self.e = 0x5A
        self.z = self.cy = False
        self.stores = []

    def fetch(self):
        assert self.base <= self.pc < self.end, 'Execution left the SRAM program'
        value = self.memory[self.pc]
        self.pc += 1
        return value

    def disp(self):
        d = self.fetch()
        return d - 256 if d & 0x80 else d

    def store(self, address, value):
        assert self.meta <= address < self.meta + 16, f'store outside metadata {address:06X}'
        self.memory[address] = value
        self.stores.append(address - self.meta)

    def run(self, budget):
        h = lambda: (self.hl >> 8) & 0xFF
        for _ in range(budget):
            start, op = self.pc, self.fetch()
            if op in (0x01, 0x11):                      # LD BC/DE,nn (24-bit)
                lo, mid, _up = self.fetch(), self.fetch(), self.fetch()
                if op == 0x01:
                    self.b, self.c = mid, lo
                else:
                    self.d, self.e = mid, lo
            elif op == 0xDD:
                sub = self.fetch()
                if sub == 0x7E:                         # LD A,(IX+d)
                    self.a = self.memory[(self.ix + self.disp()) & 0xFFFFFF]
                elif sub == 0x23:                       # INC IX
                    self.ix = (self.ix + 1) & 0xFFFFFF
                elif sub == 0x7D:                       # LD A,IXL
                    self.a = self.ix & 0xFF
                else:
                    raise AssertionError(f'DD {sub:02X}')
            elif op == 0xFD:
                sub, d = self.fetch(), self.disp()
                address = (self.iy + d) & 0xFFFFFF
                if sub == 0x7E:                         # LD A,(IY+d)
                    self.a = self.memory[address]
                elif sub == 0x77:                       # LD (IY+d),A
                    self.store(address, self.a)
                elif sub == 0xB6:                       # OR (IY+d)
                    self.a |= self.memory[address]
                    self.z, self.cy = self.a == 0, False
                else:
                    raise AssertionError(f'FD {sub:02X}')
            elif op in (0xAB, 0xAA, 0xA9, 0xA8):       # XOR E/D/C/B
                self.a ^= {0xAB: self.e, 0xAA: self.d, 0xA9: self.c, 0xA8: self.b}[op]
                self.z, self.cy = self.a == 0, False
            elif op == 0x6F:                            # LD L,A
                self.hl = (self.hl & 0xFFFF00) | self.a
            elif op == 0x7E:                            # LD A,(HL)
                self.a = self.memory[self.hl]
            elif op == 0x46:                            # LD B,(HL)
                self.b = self.memory[self.hl]
            elif op in (0x24, 0x25):                    # INC/DEC H (8-bit, no carry into HLU)
                value = h() + (1 if op == 0x24 else -1)
                assert 0 <= value <= 0xFF, 'table plane crossed a 64 KB boundary'
                self.hl = (self.hl & 0xFF00FF) | (value << 8)
            elif op in (0x5F, 0x57, 0x4F):              # LD E/D/C,A
                setattr(self, {0x5F: 'e', 0x57: 'd', 0x4F: 'c'}[op], self.a)
            elif op in (0x7B, 0x7A, 0x79, 0x78):       # LD A,E/D/C/B
                self.a = getattr(self, {0x7B: 'e', 0x7A: 'd', 0x79: 'c', 0x78: 'b'}[op])
            elif op == 0xB7:                            # OR A
                self.z, self.cy = self.a == 0, False
            elif op == 0xD6:                            # SUB n
                n = self.fetch()
                self.cy, self.a = self.a < n, (self.a - n) & 0xFF
                self.z = self.a == 0
            elif op == 0xDE:                            # SBC A,n
                n = self.fetch() + self.cy
                self.cy, self.a = self.a < n, (self.a - n) & 0xFF
                self.z = self.a == 0
            elif op == 0x2F:                            # CPL
                self.a ^= 0xFF
            elif op == 0x3E:                            # LD A,n
                self.a = self.fetch()
            elif op == 0x20:                            # JR NZ,d
                d = self.disp()
                if not self.z:
                    self.pc += d
            elif op == 0x18:                            # JR d: only the final self-loop
                d = self.disp()
                assert self.pc + d == start, 'stage must finish in its self-loop'
                return
            else:
                raise AssertionError(f'Unsupported opcode {op:02X} at {start:06X}')
        raise AssertionError('CRC stage exceeded its instruction budget')

    def result(self):
        m = self.memory[self.meta + 5:self.meta + 10]
        return int.from_bytes(m[:4], 'little'), m[4]


class CrcStageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(['make', 'build/agon-crc.bin'], cwd=ROOT, check=True, capture_output=True, text=True)
        cls.code = (ROOT / 'build/agon-crc.bin').read_bytes()

    def run_stage(self, image, base=0xB7E000):
        cpu = CrcMachine(self.code, base, image, base + 0x400)
        cpu.run(30 * len(image) + 100)
        return cpu

    def test_matches_zlib_on_patterns_and_block_counts(self):
        for image in (bytes(256), bytes(range(256)) * 3, os.urandom(256 * 5),
                      bytes([0xFF]) * 512 + os.urandom(300).ljust(512, b'\0')):
            with self.subTest(blocks=len(image) // 256):
                crc, done = self.run_stage(image).result()
                self.assertEqual((crc, done), (zlib.crc32(image), 0xA5))

    def test_more_than_255_blocks_borrows_into_the_high_count_byte(self):
        image = bytes((i * 7 + (i >> 8)) & 0xFF for i in range(256 * 258))
        crc, done = self.run_stage(image).result()
        self.assertEqual((crc, done), (zlib.crc32(image), 0xA5))

    def test_completion_written_last_and_only_metadata_written(self):
        cpu = self.run_stage(os.urandom(512), base=0xFFE000)
        writes = [s for s in cpu.stores if s >= 5]
        self.assertEqual(writes, [5, 6, 7, 8, 9])
        self.assertEqual(cpu.stores[-1], 9)


if __name__ == '__main__':
    unittest.main()

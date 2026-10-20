"""The linked eZ80 cpi/cpd scanners editor_row uses to bound changed columns.

Host builds compile the C fallback; this runs the target asm in the cycle
model's interpreter (which has cpi and cpd) against a Python oracle.
"""
import importlib.util
from pathlib import Path
import random
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('compose_cycles', ROOT/'tools/verification/compose_cycles.py')
compose_cycles = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compose_cycles)
A, B = 0x90010, 0x90100


class EditorDifferenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        image = compose_cycles.TargetImage()
        cls.cpu = compose_cycles.CycleCPU(image, ROOT/'build/vios-resident.elf')

    def check(self, shown, cached):
        cpu, width = self.cpu, len(shown)
        # Guard bytes equal on both sides would let an unbounded scan run on.
        cpu.mem[A-1:A+width+1] = b'\x01'+shown+b'\x02'
        cpu.mem[B-1:B+width+1] = b'\x03'+cached+b'\x04'
        different = [i for i in range(width) if shown[i] != cached[i]]
        first = cpu.call('_editor_first_difference', (A, B)) & compose_cycles.MASK
        last = cpu.call('_editor_last_difference', (A, B, width)) & compose_cycles.MASK
        self.assertEqual((first, last), (different[0], different[-1]))

    def test_edges_and_separated_runs(self):
        for width in (1, 54, 80):
            row = bytes(b'\\ WBM1P source row 07 retained exactly'.ljust(width)[:width])
            for columns in ([0], [width-1], [width//2], [0, width-1], [3, width-5], list(range(width))):
                columns = sorted({c for c in columns if 0 <= c < width})
                if not columns:
                    continue
                changed = bytearray(row)
                for c in columns:
                    changed[c] ^= 0x40
                with self.subTest(width=width, columns=columns[:4]):
                    self.check(bytes(changed), row)

    def test_random_rows(self):
        rng = random.Random(54)
        for _ in range(200):
            width = rng.choice((54, 80))
            cached = bytes(rng.choice(b' #:;\\0123456789ABCDEFrow') for _ in range(width))
            shown = bytearray(cached)
            for c in rng.sample(range(width), rng.randint(1, 4)):
                shown[c] = rng.choice([x for x in range(32, 127) if x != cached[c]])
            self.check(bytes(shown), cached)


if __name__ == '__main__':
    unittest.main()

"""arch/ez80/agon_flash.s on the eZ80 interpreter against a model of the
eZ80F92 flash controller; never opens hardware.

The model enforces what the real controller does: FLASH_PROT (0xFA) and
FLASH_FDIV (0xF9) only change right after the 0xB6, 0x49 unlock sequence on
FLASH_KEY (0xF5); a mass erase (FLASH_PGCTL 0xFF bit 0) needs every page
unprotected and sets all 128 KB to 0xFF, with its busy bit visible for a few
polls; programming a byte can only clear bits, and protected pages ignore
writes. The image must land exactly, the rest stay erased, the pages end up
protected again and the done flag set.
"""
from pathlib import Path
import os
import random
import re
import subprocess
from types import SimpleNamespace
import unittest
from test_uart0_rx_native import UARTCPU
from test_vios_native_c import ROOT, MASK

FLASH = 0x20000
STAGE, IMAGE, LENGTH, DONE = 0x040000, 0x050000, 0x070000, 0x070003


def build():
    prefix = str(Path(os.environ.get('AGONDEV', str(ROOT/'.tools/agondev')))/'bin/ez80-none-elf')
    out = ROOT/'build/verification/agon-flash'
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run([prefix+'-as', '-march=ez80+full', '-o', str(out/'flash.o'), str(ROOT/'arch/ez80/agon_flash.s')], check=True, capture_output=True)
    subprocess.run([prefix+'-ld', f'-Ttext=0x{STAGE:x}', '-e', '_agon_flash', '-o', str(out/'flash.elf'), str(out/'flash.o')], check=True, capture_output=True)
    subprocess.run([prefix+'-objcopy', '-O', 'binary', '-j', '.text', str(out/'flash.elf'), str(out/'flash.bin')], check=True, capture_output=True)
    listing = subprocess.check_output([prefix+'-objdump', '-d', str(out/'flash.elf')], text=True)
    ins = {}
    for line in listing.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s*((?:[0-9a-f]{2} )+)\s*([^\t]+)\s*$', line)
        if m:
            ins[int(m[1], 16)] = (len(m[2].split()), m[3].split(' <')[0].strip())
    return SimpleNamespace(raw=(out/'flash.bin').read_bytes(), syms={'_agon_flash': STAGE}, ins=ins)


class Controller:
    def __init__(self, flash):
        self.flash = flash                      # bytearray(FLASH): the array itself
        self.key = 0                            # unlock progress
        self.prot, self.fdiv = 0xFF, 0x00       # reset: all pages protected
        self.busy = 0
        self.erases = 0
        self.log = []
    def tick(self):
        pass
    def read(self, port):
        if port == 0xFF:
            if self.busy:
                self.busy -= 1
                return 0x01
            return 0x00
        raise AssertionError(f'unexpected IN0 {port:x}')
    def write(self, port, value):
        self.log.append((port, value))
        if port == 0xF5:
            self.key = 1 if value == 0xB6 else 2 if (value == 0x49 and self.key == 1) else 0
            return
        unlocked, self.key = self.key == 2, 0
        if port == 0xFA:
            if unlocked:
                self.prot = value
        elif port == 0xF9:
            if unlocked:
                self.fdiv = value
        elif port == 0xFF:
            if value & 1 and self.prot == 0:     # mass erase only when nothing is protected
                self.flash[:] = b'\xff' * FLASH
                self.busy = 5
                self.erases += 1
        else:
            raise AssertionError(f'unexpected OUT0 {port:x}')
    def program(self, address, value):
        if self.busy:
            self.busy -= 1                      # still erasing: the write is lost
            return
        if self.prot & (1 << (address // 0x4000)):
            return                              # protected page: ignored
        self.flash[address] &= value             # programming only clears bits


class Flasher(UARTCPU):
    def __init__(self, image, controller):
        super().__init__(SimpleNamespace(raw=b'', syms=image.syms, ins=image.ins), controller)
        self.mem[STAGE:STAGE+len(image.raw)] = image.raw
        self.controller = controller
    def step(self):
        size, text = self.ins[self.pc]
        if text == 'ldir':
            while True:
                de, hl = self.r['de'], self.r['hl']
                if de < FLASH:
                    self.controller.program(de, self.mem[hl])
                else:
                    self.mem[de] = self.mem[hl]
                self.r['de'] = (de+1) & MASK; self.r['hl'] = (hl+1) & MASK; self.r['bc'] = (self.r['bc']-1) & MASK
                if not self.r['bc']:
                    break
            self.pc += size
            return True
        if text.startswith('jr ') and self.ins[self.pc] == (2, text) and self.mem[self.pc:self.pc+2] == b'\x18\xfe':
            return False                         # the final spin: done
        if text == 'xor a,a':
            self.set('a', 0); self.z = True; self.pc += size
            return True
        return super().step()
    def run(self, budget=2_000_000):
        self.pc = STAGE
        for _ in range(budget):
            if not self.step():
                return True
        raise AssertionError('flasher instruction budget exceeded')


class AgonFlashTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = build()

    def flash(self, payload, old):
        store = bytearray(old)
        ctl = Controller(store)
        cpu = Flasher(self.image, ctl)
        cpu.mem[IMAGE:IMAGE+len(payload)] = payload
        cpu.wr(LENGTH, len(payload))
        cpu.mem[DONE] = 0xA5
        self.assertTrue(cpu.run())
        return store, ctl, cpu

    def test_programs_the_image_and_leaves_the_rest_erased(self):
        rng = random.Random(2)
        old = bytes(rng.randrange(256) for _ in range(FLASH))           # the old MOS, any content
        new = bytes(rng.randrange(256) for _ in range(108490))           # MOS 3.0.2's size
        store, ctl, cpu = self.flash(new, old)
        self.assertEqual(bytes(store[:len(new)]), new)
        self.assertEqual(bytes(store[len(new):]), b'\xff' * (FLASH - len(new)))
        self.assertEqual((ctl.erases, ctl.prot, ctl.fdiv, cpu.mem[DONE]), (1, 0xFF, 0x5F, 1))
        self.assertEqual(cpu.r['sp'], 0x0BFFFF)

    def test_a_full_128k_image(self):
        new = bytes(range(256)) * (FLASH // 256)
        store, ctl, cpu = self.flash(new, b'\0' * FLASH)
        self.assertEqual(bytes(store), new)

    def test_waits_for_the_erase_to_finish(self):
        store, ctl, cpu = self.flash(b'\x12\x34', b'\0' * FLASH)
        self.assertEqual(bytes(store[:2]), b'\x12\x34')   # a write during the erase would be lost
        self.assertEqual(ctl.busy, 0)


if __name__ == '__main__':
    unittest.main()

"""arch/ez80/start.s from its linked disassembly: the MOS boot stub's handoff
(HL = VIOS_BOOT_MAGIC) survives the BSS clear, and platform_restart then
arms the watchdog for a real CPU reset; any other boot restarts in RAM."""
from pathlib import Path
import os
import re
import subprocess
from types import SimpleNamespace
import unittest
from test_uart0_rx_native import UARTCPU
from test_vios_native_c import ROOT

MAGIC = 0x56420B
KERNEL_MAIN = 0x090000          # no code there: execution stops on arrival


def build():
    prefix = str(Path(os.environ.get('AGONDEV', str(ROOT/'.tools/agondev')))/'bin/ez80-none-elf')
    out = ROOT/'build/verification/start-restart'
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run([prefix+'-as', '-march=ez80+full', '--defsym', 'RAM_RESTART=1', '-o', str(out/'start.o'), str(ROOT/'arch/ez80/start.s')], check=True, capture_output=True)
    subprocess.run([prefix+'-ld', '-T', str(ROOT/'boards/agon-light-2/ram.ld'), '--defsym', f'_kernel_main=0x{KERNEL_MAIN:x}',
                    '-o', str(out/'start.elf'), str(out/'start.o')], check=True, capture_output=True)   # the resident's own layout
    subprocess.run([prefix+'-objcopy', '-O', 'binary', str(out/'start.elf'), str(out/'start.bin')], check=True, capture_output=True)
    symbols = subprocess.check_output([prefix+'-nm', '-n', str(out/'start.elf')], text=True)
    listing = subprocess.check_output([prefix+'-objdump', '-d', str(out/'start.elf')], text=True)
    syms = {v[2]: int(v[0], 16) for line in symbols.splitlines() if len(v := line.split()) == 3}
    ins = {}
    for line in listing.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s*((?:[0-9a-f]{2} )+)\s*([^\t]+)\s*$', line)
        if m:
            ins[int(m[1], 16)] = (len(m[2].split()), m[3].split(' <')[0].strip())
    return SimpleNamespace(raw=(out/'start.bin').read_bytes(), syms=syms, ins=ins)


class Ports:
    def __init__(self):
        self.writes = []
    def tick(self):
        pass
    def read(self, port):
        assert port == 0xc5, f'unexpected read {port:x}'
        return 0x60                               # transmitter empty
    def write(self, port, value):
        self.writes.append((port, value))


class Start(UARTCPU):
    def step(self):
        if self.pc == KERNEL_MAIN:
            return False
        size, text = self.ins[self.pc]
        if text in ('di', 'ei'):
            self.pc += size
            return True
        if text == 'ldir':
            while True:
                self.mem[self.r['de']] = self.mem[self.r['hl']]
                self.r['de'] += 1; self.r['hl'] += 1; self.r['bc'] -= 1
                if not self.r['bc']:
                    break
            self.pc += size
            return True
        return super().step()


class StartRestartTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = build()

    def boot(self, hl):
        ports = Ports()
        cpu = Start(self.image, ports)
        start, size = self.image.syms['__bss_start'], self.image.syms['__bss_size']
        self.assertTrue(start <= self.image.syms['_platform_boot_source'] < start+size, 'the handoff lives in the cleared BSS')
        cpu.mem[start:start+size] = b'\xa5' * size
        cpu.r['hl'] = hl
        cpu.pc = self.image.syms['_image_start']
        for _ in range(10000):
            if not cpu.step():
                break
        self.assertEqual(cpu.pc, KERNEL_MAIN)
        return cpu, ports

    def restart(self, cpu, ports, budget=200):
        cpu.pc = self.image.syms['_platform_restart']
        for _ in range(budget):
            if cpu.pc == self.image.syms['_image_start']:
                return 'ram'
            if (0x93, 0x83) in ports.writes:
                return 'watchdog'
            cpu.step()
        return 'spinning'

    def test_stub_boot_restarts_through_the_watchdog(self):
        cpu, ports = self.boot(MAGIC)
        source = self.image.syms['_platform_boot_source']
        self.assertEqual(cpu.rd(source), MAGIC)
        start, size = self.image.syms['__bss_start'], self.image.syms['__bss_size']
        rest = bytes(cpu.mem[start:start+size]).replace(MAGIC.to_bytes(3, 'little'), b'', 1)
        self.assertEqual(rest, b'\0' * (size-3))                       # cleared, then the handoff stored
        self.assertEqual(self.restart(cpu, ports), 'watchdog')
        for _ in range(12):
            cpu.step()
        self.assertEqual(ports.writes[-3:], [(0x93, 0x83), (0x94, 0xA5), (0x94, 0x5A)])

    def test_other_boots_restart_in_ram(self):
        for hl in (0, 0x123456, MAGIC ^ 1):
            cpu, ports = self.boot(hl)
            self.assertEqual(self.restart(cpu, ports), 'ram')
            self.assertNotIn(0x93, [p for p, _ in ports.writes])


if __name__ == '__main__':
    unittest.main()

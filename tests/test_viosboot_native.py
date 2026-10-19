"""The untethered boot stub (arch/ez80/viosboot.s) executed from its linked
disassembly against a model ESP on UART0 and a model MOS; never opens hardware.

The ESP model answers VDU 23,0,E8 exactly like firmware/esp32-vios/include/
vios_bootserve.h (cross-checked against the C header), streams through a
16-byte FIFO only while the stub asserts RTS (one character may finish after
RTS drops), interleaves keyboard packets and can corrupt, refuse or stay
silent. The MOS model implements mos_load/mos_save (rst.lil 08h) over an
in-memory SD card and putch (rst.lil 10h).
"""
from collections import deque
from pathlib import Path
import os
import random
import re
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
import zlib
from test_uart0_rx_native import UARTCPU
from test_vios_native_c import ROOT, MASK

STUB = 0x0B0000
IMAGE = 0x040000
ARGS = 0x0BF000
BLOCK = 192


def build(burst, timeout=1):
    prefix = str(Path(os.environ.get('AGONDEV', str(ROOT/'.tools/agondev')))/'bin/ez80-none-elf')
    out = ROOT/f'build/verification/viosboot-b{burst}-t{timeout}'
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run(['.venv/bin/python', 'tools/crc32_planes.py', str(out/'viosboot_crc.inc')], cwd=ROOT, check=True)
    subprocess.run([prefix+'-as', '-march=ez80+full', '-I', str(out), '--defsym', f'VIOS_BOOT_BURST={burst}',
                    '--defsym', f'VIOS_BOOT_TIMEOUT={timeout}', '-o', str(out/'viosboot.o'), str(ROOT/'arch/ez80/viosboot.s')],
                   check=True, capture_output=True)
    subprocess.run([prefix+'-ld', f'-Ttext=0x{STUB:x}', '-e', '_viosboot', '-o', str(out/'viosboot.elf'), str(out/'viosboot.o')],
                   check=True, capture_output=True)
    subprocess.run([prefix+'-objcopy', '-O', 'binary', '-j', '.text', str(out/'viosboot.elf'), str(out/'viosboot.bin')],
                   check=True, capture_output=True)
    symbols = subprocess.check_output([prefix+'-nm', '-n', str(out/'viosboot.elf')], text=True)
    listing = subprocess.check_output([prefix+'-objdump', '-d', str(out/'viosboot.elf')], text=True)
    syms = {v[2]: int(v[0], 16) for line in symbols.splitlines() if len(v := line.split()) == 3}
    ins = {}
    for line in listing.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s*((?:[0-9a-f]{2} )+)\s*([^\t]+)\s*$', line)
        if m:
            ins[int(m[1], 16)] = (len(m[2].split()), m[3].split(' <')[0].strip())
    return SimpleNamespace(raw=(out/'viosboot.bin').read_bytes(), syms=syms, ins=ins)


def manifest(image, gui=1, baud=1152000):
    return (b'M' + bytes((1, BLOCK)) + len(image).to_bytes(4, 'little') + zlib.crc32(image).to_bytes(4, 'little')
            + bytes((gui,)) + baud.to_bytes(4, 'little'))


def block(image, index):
    head = index.to_bytes(2, 'little')
    data = image[index*BLOCK:(index+1)*BLOCK]
    if index*BLOCK >= len(image):
        return b'E' + head
    return b'B' + head + data + zlib.crc32(data).to_bytes(4, 'little')


def make_image(size, seed=1):
    rng = random.Random(seed)
    data = bytearray(rng.randrange(256) for _ in range(size))
    data[64:69] = b'MOS\x00\x01'
    return bytes(data)


class Esp:
    """UART0 as the stub sees it, with the ESP behind it."""
    def __init__(self, image, *, period=16, silent=False, corrupt=(), refuse_manifest=False, keyboard=True, garbage=False):
        self.image, self.period, self.silent = image, period, silent
        self.corrupt, self.refuse_manifest = set(corrupt), refuse_manifest
        self.keyboard, self.garbage = keyboard, garbage
        self.out = deque()                 # bytes the ESP still has to send
        self.fifo = deque()                # the CPU's 16-byte RX FIFO
        self.mctl = 0
        self.inflight = False
        self.phase = self.overruns = 0
        self.error = False
        self.rx = bytearray()              # bytes the CPU sent
        self.requests = []
        self.sent_while_rts_low = 0
    def packet(self, payload):
        if self.keyboard:
            self.out.extend(b'\x81\x04a\x00\x00\x01')
        if self.garbage:
            self.out.extend(b'\x05\x06')   # stray bytes without the header bit
        self.out.extend(bytes((0x80 | 0x7B, len(payload))) + payload)
    def request(self, op, args):
        self.requests.append((op, args))
        if self.silent:
            return
        if op == 0:
            m = manifest(self.image)
            if self.refuse_manifest:
                m = m[:3] + (0x70000).to_bytes(4, 'little') + m[7:]
            self.packet(m)
        elif op == 1:
            index, count = args[0] | args[1] << 8, args[2]
            assert 1 <= count <= 32
            for i in range(index, index+count):
                b = bytearray(block(self.image, i))
                if i in self.corrupt:
                    self.corrupt.discard(i)
                    b[3+BLOCK//2] ^= 0x40
                self.packet(bytes(b))
                if b[0] != ord('B'):
                    break
    def tick(self):
        if not self.out or not (self.mctl & 2 or self.inflight):
            return
        self.phase += 1
        if self.phase < self.period:
            return
        self.phase = 0
        if not self.mctl & 2:
            self.sent_while_rts_low += 1
            self.inflight = False
        if len(self.fifo) == 16:
            self.overruns += 1
            self.error = True
            self.out.popleft()
        else:
            self.fifo.append(self.out.popleft())
    def read(self, port):
        if port == 0xc4:
            return self.mctl
        if port == 0xc5:
            result = 0x20 | bool(self.fifo) | (2 if self.error else 0)
            self.error = False
            return result
        if port == 0xc6:
            return 0x10                    # CTS: the ESP accepts input
        assert port == 0xc0 and self.fifo, f'unexpected UART read {port:x}'
        return self.fifo.popleft()
    def write(self, port, value):
        if port == 0xc4:
            if self.mctl & 2 and not value & 2:
                self.inflight = bool(self.out)
            self.mctl = value
            return
        assert port == 0xc0, f'unexpected UART write {port:x}'
        self.rx.append(value)
        while True:                        # VDU 23,0,E8,op[,lo,hi,n]
            if len(self.rx) >= 4 and self.rx[:3] == b'\x17\x00\xe8':
                need = 4 if self.rx[3] == 0 else 7
                if len(self.rx) < need:
                    return
                self.request(self.rx[3], bytes(self.rx[4:need]))
                del self.rx[:need]
                continue
            if self.rx and self.rx[0] != 0x17:
                del self.rx[0]             # MOS text etc.
                continue
            return


class Stub(UARTCPU):
    """The stub at 0x0B0000 with MOS services; stops at the jump into VIOS."""
    def __init__(self, image, esp, sd=None, sd_writable=True):
        super().__init__(SimpleNamespace(raw=b'', syms=image.syms, ins=image.ins), esp)
        self.mem[STUB:STUB+len(image.raw)] = image.raw
        self.sd = {} if sd is None else dict(sd)
        self.sd_writable = sd_writable
        self.console = bytearray()
        self.booted = False
        self.interrupts = True
        self.rx_steps = []
    def cstring(self, address):
        end = self.mem.index(0, address)
        return self.mem[address:end].decode()
    def step(self):
        if self.pc == IMAGE:
            self.booted = True
            return False
        if not self.pc:
            return False
        size, text = self.ins[self.pc]
        if text.startswith('rst.lil'):
            vector = int(text.split()[1], 0)
            if vector == 0x10:
                self.console.append(self.g('a'))
            else:
                assert vector == 0x08, text
                function = self.g('a')
                name, de, bc = self.cstring(self.r['hl']), self.r['de'], self.r['bc']
                if function == 1:          # mos_load
                    data = self.sd.get(name)
                    status = 4 if data is None else 19 if len(data) > bc else 0
                    if not status:
                        self.mem[de:de+len(data)] = data
                elif function == 2:        # mos_save
                    status = 0 if self.sd_writable else 7
                    if not status:
                        self.sd[name] = bytes(self.mem[de:de+bc])
                else:
                    raise AssertionError(f'MOS function {function}')
                self.set('a', status)
            self.pc += size
            return True
        if text in ('di', 'ei'):
            self.interrupts = text == 'ei'
            self.pc += size
            return True
        if text == 'scf':
            self.c = True
            self.pc += size
            return True
        if text == 'ldir':
            while True:
                self.mem[self.r['de']] = self.mem[self.r['hl']]
                self.r['de'] = (self.r['de']+1) & MASK
                self.r['hl'] = (self.r['hl']+1) & MASK
                self.r['bc'] = (self.r['bc']-1) & MASK
                if not self.r['bc']:
                    break
            self.pc += size
            return True
        m = re.fullmatch(r'(inc|dec) ([abdehl])', text)
        if m:                              # byte INC/DEC set Z (and S); carry unchanged
            value = (self.g(m[2]) + (1 if m[1] == 'inc' else -1)) & 255
            self.set(m[2], value)
            self.z, self.s = value == 0, bool(value & 0x80)
            self.pc += size
            return True
        return super().step()
    def run_stub(self, args='', budget=8_000_000):
        self.mem[ARGS:ARGS+len(args)+1] = args.encode() + b'\0'
        self.r.update(hl=ARGS, sp=0x0BFF00)
        self.push(0)
        self.pc = STUB
        for _ in range(budget):
            if not self.step():
                return 'vios' if self.booted else 'mos'
        raise AssertionError('stub instruction budget exceeded')


class ViosBootStubTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.stubs = {burst: build(burst) for burst in (1, 4)}
        cls.image = make_image(3000)
        cls.sd = {'/vios/vios.man': manifest(cls.image), '/vios/vios.bin': cls.image}

    def boot(self, esp, sd=None, args='', burst=1, **kw):
        cpu = Stub(self.stubs[burst], esp, sd, **kw)
        result = cpu.run_stub(args)
        return result, cpu

    def assert_vios(self, result, cpu, image=None):
        image = image or self.image
        self.assertEqual(result, 'vios', cpu.console.decode(errors='replace'))
        self.assertEqual(bytes(cpu.mem[IMAGE:IMAGE+len(image)]), image)
        self.assertFalse(cpu.interrupts, 'VIOS must start with interrupts off')

    def test_packets_match_the_c_server(self):
        image = make_image(1000, seed=3)
        with tempfile.TemporaryDirectory() as temp:
            source, binary = Path(temp)/'dump.cpp', Path(temp)/'dump'
            source.write_text('#include <cstdio>\n#include "vios_bootserve.h"\nint main(int, char **v) {\n'
                              '  static uint8_t image[4096]; FILE *f = fopen(v[1], "rb"); unsigned n = fread(image, 1, sizeof image, f);\n'
                              '  uint8_t out[256]; unsigned k = vios_boot_manifest(out, n, vios_boot_crc32(0, image, n), 1, 1152000);\n'
                              '  for (unsigned i = 0; i < k; ++i) printf("%02x", out[i]); printf("\\n");\n'
                              '  for (unsigned b = 0; b <= vios_boot_blocks(n); ++b) { k = vios_boot_block(out, image, n, b);\n'
                              '    for (unsigned i = 0; i < k; ++i) printf("%02x", out[i]); printf("\\n"); }\n}\n')
            (Path(temp)/'image.bin').write_bytes(image)
            subprocess.run(['c++', '-std=c++11', '-Wall', '-Werror', '-I'+str(ROOT/'firmware/esp32-vios/include'), str(source), '-o', str(binary)], check=True)
            lines = subprocess.check_output([str(binary), str(Path(temp)/'image.bin')], text=True).split()
        expected = [manifest(image).hex()] + [block(image, i).hex() for i in range(len(image)//BLOCK + 2)]
        self.assertEqual(lines, expected)

    def test_loads_the_esp_image_one_block_per_request(self):
        esp = Esp(self.image)
        result, cpu = self.boot(esp, sd={})
        self.assert_vios(result, cpu)
        blocks = (len(self.image)+BLOCK-1)//BLOCK
        self.assertEqual(esp.requests, [(0, b'')] + [(1, bytes((i, 0, 1))) for i in range(blocks)])
        self.assertEqual(esp.overruns, 0)
        self.assertLessEqual(esp.sent_while_rts_low, len(esp.requests), 'at most one character after each RTS drop')
        self.assertEqual(cpu.sd['/vios/boot.cnt'], b'\x01')
        self.assertEqual(cpu.console, b'')

    def test_bursts_and_odd_sizes(self):
        for size in (65, 192, 193, 1537):
            image = make_image(size, seed=size)
            esp = Esp(image)
            result, cpu = self.boot(esp, sd={}, burst=4)
            self.assert_vios(result, cpu, image)
            self.assertEqual(esp.overruns, 0)
            counts = [args[2] for op, args in esp.requests if op == 1]
            self.assertEqual(sum(counts), (size+BLOCK-1)//BLOCK)   # never asks past the end
            self.assertTrue(all(c == 4 for c in counts[:-1]))

    def test_a_corrupted_block_is_asked_for_again(self):
        esp = Esp(self.image, corrupt={2, 7}, garbage=True)
        result, cpu = self.boot(esp, sd={})
        self.assert_vios(result, cpu)
        indices = [args[0] for op, args in esp.requests if op == 1]
        self.assertEqual(indices.count(2), 2)
        self.assertEqual(indices.count(7), 2)

    def test_silent_esp_falls_back_to_the_sd_image(self):
        result, cpu = self.boot(Esp(self.image, silent=True), sd=self.sd)
        self.assert_vios(result, cpu)
        self.assertIn(b'no ESP image', cpu.console)

    def test_a_refused_manifest_falls_back_to_sd(self):
        result, cpu = self.boot(Esp(self.image, refuse_manifest=True), sd=self.sd)
        self.assert_vios(result, cpu)

    def test_sd_first_does_not_ask_the_esp(self):
        esp = Esp(self.image)
        result, cpu = self.boot(esp, sd=self.sd, args='sd')
        self.assert_vios(result, cpu)
        self.assertEqual(esp.requests, [])

    def test_a_corrupt_sd_image_is_never_run(self):
        bad = bytearray(self.image); bad[1000] ^= 1
        result, cpu = self.boot(Esp(self.image, silent=True), sd={'/vios/vios.man': manifest(self.image), '/vios/vios.bin': bytes(bad)})
        self.assertEqual(result, 'mos')
        self.assertIn(b'no bootable VIOS image', cpu.console)
        self.assertTrue(cpu.interrupts, 'MOS gets its interrupts back')
        unheadered = bytearray(self.image); unheadered[64] = 0
        result, cpu = self.boot(Esp(self.image, silent=True), sd={'/vios/vios.man': manifest(bytes(unheadered)), '/vios/vios.bin': bytes(unheadered)})
        self.assertEqual(result, 'mos')

    def test_failed_boots_are_counted_and_force_retries(self):
        sd = {'/vios/boot.cnt': b'\x02'}
        result, cpu = self.boot(Esp(self.image), sd=sd)
        self.assert_vios(result, cpu)
        self.assertEqual(cpu.sd['/vios/boot.cnt'], b'\x03')
        result, cpu = self.boot(Esp(self.image), sd=cpu.sd)
        self.assertEqual(result, 'mos')
        self.assertIn(b'3 boots did not finish', cpu.console)
        result, cpu = self.boot(Esp(self.image), sd=cpu.sd, args='force')
        self.assert_vios(result, cpu)
        self.assertEqual(cpu.sd['/vios/boot.cnt'], b'\x01')

    def test_no_sd_card_still_boots_from_the_esp(self):
        result, cpu = self.boot(Esp(self.image), sd={}, sd_writable=False)
        self.assert_vios(result, cpu)
        self.assertNotIn('/vios/boot.cnt', cpu.sd)

    def test_returns_to_mos_with_its_stack_and_registers(self):
        cpu = Stub(self.stubs[1], Esp(self.image, silent=True), {})
        cpu.r.update(ix=0x123456, iy=0xabcdef)
        self.assertEqual(cpu.run_stub(), 'mos')
        self.assertEqual((cpu.r['ix'], cpu.r['iy'], cpu.r['sp'], cpu.r['hl']), (0x123456, 0xabcdef, 0x0BFF00, 0))


if __name__ == '__main__':
    unittest.main()

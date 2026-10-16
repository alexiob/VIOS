"""Execute linked target C loops, with only I/O and compiler helpers intercepted.

The interpreter is a bounded semantic probe, not a cycle-accurate CPU emulator.
It loads the production ELF's disassembly, symbols and binary. No serial I/O.
"""
import ast
from pathlib import Path
import os
import re
import subprocess
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
MASK = 0xffffff

# Reuse only the CPU class. Importing probe.py itself would execute its VM suite.
probe = ROOT/'tools/verification/forth-engine/probe.py'
node = next(node for node in ast.parse(probe.read_text()).body
            if isinstance(node, ast.ClassDef) and node.name == 'CPU')
namespace = {'Path':Path, 're':re, 'MASK':MASK}
exec(compile(ast.Module(body=[node], type_ignores=[]), str(probe), 'exec'), namespace)
BaseCPU = namespace['CPU']


class LinkedC(BaseCPU):
    def __init__(self, image):
        self.mem = bytearray(1 << 24)
        self.mem[0x40000:0x40000+len(image.raw)] = image.raw
        self.syms, self.ins = image.syms, image.ins
        self.r = dict(hl=0, de=0, bc=0x123456, ix=0x654321, iy=0xabcdef, sp=0xafff0, a=0x5a)
        self.z = self.s = self.p = self.c = False
        self.pc = 0
        self.events = []
        self.output = bytearray()
        self.source = None
        self.reads = []
        self.hooks = {}
        self.tx_limits = []
        for name in ('__frameset', '__frameset0', '__lxor', '__lshru', '__land', '__lnot', '__iand',
                     '_send', '_poll', '_platform_console_flush', '_platform_console_putc',
                     '_ez80_uart0_tx_burst', '_ez80_uart0_rx_resume'):
            if name in self.syms:
                self.hooks[self.syms[name]] = name

    def rd(self, address, n=3):
        assert address != 0, 'NULL data dereference'
        if 0x90000 <= address < 0x92000:
            assert self.source is not None, 'unexpected source dereference'
            start, end = self.source
            assert start <= address and address+n <= end, f'source read outside [{start:x},{end:x}): {address:x}+{n}'
            self.reads.extend(range(address, address+n))
        return super().rd(address, n)

    def g(self, operand):
        if operand in ('iyl', 'iyh', 'ixl', 'ixh'):
            return (self.r[operand[:2]] >> (8 if operand.endswith('h') else 0)) & 255
        return super().g(operand)

    def set(self, operand, value):
        if operand in ('iyl', 'iyh', 'ixl', 'ixh'):
            pair = operand[:2]
            shift = 8 if operand.endswith('h') else 0
            self.r[pair] = (self.r[pair] & ~(255 << shift)) | ((value & 255) << shift)
        else:
            super().set(operand, value)

    def clobber(self):
        # A real C call may destroy these registers. Locals must survive that.
        self.r.update(hl=0xfedcba, de=0x765432, bc=0xabcdef, a=0x93)
        self.z = self.s = self.p = self.c = True

    def helper(self, name):
        if name in ('__frameset', '__frameset0'):
            return_address = self.pop()
            self.push(self.r['ix'])
            self.r['ix'] = self.r['sp']
            if name == '__frameset':
                self.r['sp'] = (self.r['sp'] + self.r['hl']) & MASK
            self.r['hl'] = return_address
            self.pc = return_address
            return
        if name == '_ez80_uart0_tx_burst':
            source, length = self.rd(self.r['sp']+3), self.rd(self.r['sp']+6)
            assert 0 < length <= 32, 'C must pass a bounded native TX chunk'
            count = min(length, self.tx_limits.pop(0)) if self.tx_limits else length
            self.output.extend(self.rd(source+i,1) for i in range(count))
            self.events.append('burst')
            self.clobber()
            self.r['hl'] = count
        elif name == '_ez80_uart0_rx_resume':
            self.events.append('resume')
            self.clobber()
        elif name in ('_send', '_platform_console_putc'):
            self.output.append(self.rd(self.r['sp']+3, 1))
            self.events.append('send')
            self.clobber()
        elif name in ('_poll', '_platform_console_flush'):
            self.events.append('poll' if name == '_poll' else 'flush')
            self.clobber()
        elif name in ('__lxor', '__land'):
            left = self.r['hl'] | self.g('e') << 24
            right = self.r['bc'] | self.r['a'] << 24
            result = left ^ right if name == '__lxor' else left & right
            self.set('hl', result)
            self.set('e', result >> 24)
        elif name == '__lshru':
            result = (self.r['bc'] | self.r['a'] << 24) >> self.g('l')
            self.set('bc', result)
            self.set('a', result >> 24)
        elif name == '__lnot':
            result = ~(self.r['hl'] | self.g('e') << 24) & 0xffffffff
            self.set('hl', result)
            self.set('e', result >> 24)
        elif name == '__iand':
            self.set('hl', self.r['hl'] & self.r['bc'])
        else:
            raise AssertionError(name)
        self.pc = self.pop()

    def step(self):
        if self.pc in self.hooks:
            self.helper(self.hooks[self.pc])
            return True
        if not self.pc:
            return False
        size, text = self.ins[self.pc]
        if text.startswith('jr '):
            args = [value.strip() for value in text[3:].split(',')]
            # The bundled objdump truncates displayed JR destinations to 16bits.
            # Decode the actual signed displacement, retaining all ADL PC bits.
            displacement = int.from_bytes(self.mem[self.pc+size-1:self.pc+size], 'little', signed=True)
            self.pc += size
            if len(args) == 1 or self.cond(args[0]):
                self.pc = (self.pc+displacement) & MASK
            return True
        if text.startswith('bit '):
            bit, register = (value.strip() for value in text[4:].split(','))
            self.z = not (self.g(register) & (1 << int(bit)))
            self.pc += size
            return True
        return super().step()

    def call(self, function, args=(), budget=1000000):
        original_sp, original_ix = self.r['sp'], self.r['ix']
        for argument in reversed(args):
            self.push(argument)
        self.push(0)
        self.pc = self.syms[function] if isinstance(function, str) else function
        for _ in range(budget):
            if not self.step():
                assert self.r['ix'] == original_ix, 'C frame pointer corrupted'
                assert self.r['sp'] == original_sp-3*len(args), 'native stack imbalance'
                for _ in args:
                    self.pop()
                return self.r['hl'] | (self.g('e') << 24)
        raise AssertionError('target C instruction budget exceeded')

    def data(self, content, address=0x90010):
        self.mem[address-1:address+len(content)+1] = b'\xa5'+content+b'\x5a'
        self.source = (address, address+len(content))
        return address


class TargetImage:
    def __init__(self, elf=None):
        if elf is None:
            subprocess.run(['make', 'build/vios-resident.bin'], cwd=ROOT, check=True, capture_output=True)
            elf = ROOT/'build/vios-resident.elf'
        prefix = str(Path(os.environ.get('AGONDEV', str(ROOT/'.tools/agondev')))/'bin/ez80-none-elf')
        output = ROOT/'build/verification/vios-native-c'
        output.mkdir(parents=True, exist_ok=True)
        symbols = subprocess.check_output([prefix+'-nm', '-n', str(elf)], text=True)
        listing = subprocess.check_output([prefix+'-objdump', '-d', str(elf)], text=True)
        (output/(elf.stem+'.sym')).write_text(symbols)
        (output/(elf.stem+'.dis')).write_text(listing)
        self.syms = {}
        self.copies = []
        for line in symbols.splitlines():
            parts = line.split()
            if len(parts) == 3:
                address, _, name = parts
                self.syms[name] = int(address, 16)
                if name == '_copy':
                    self.copies.append(int(address, 16))
        self.ins = {}
        for line in listing.splitlines():
            match = re.match(r'\s*([0-9a-f]+):\s*((?:[0-9a-f]{2} )+)\s*([^\t]+)\s*$', line)
            if match:
                self.ins[int(match[1], 16)] = (len(match[2].split()), match[3].split(' <')[0].strip())
        self.raw = elf.with_suffix('.bin').read_bytes()


class NativeCLoopsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = TargetImage()

    def test_vdp_send_preserves_first_last_and_every_binary_byte(self):
        for content in (b'\x17\x00\xe2\x00', b'\x17\x00\xe2\x01', b'\x80', bytes(range(256))):
            with self.subTest(content=content[:4], length=len(content)):
                cpu = LinkedC(self.image)
                start = cpu.data(content, 0x900fe)
                cpu.call('_platform_vdp_send', [start, len(content)])
                self.assertEqual(cpu.output, content)
                self.assertEqual(cpu.reads, list(range(start, start+len(content))))
                self.assertEqual(cpu.events, ['flush'] + ['poll', 'burst']*((len(content)+31)//32)+['poll'])

    def test_vdp_send_preserves_bytes_across_zero_and_partial_progress(self):
        cpu = LinkedC(self.image)
        content = bytes(range(80));start = cpu.data(content)
        cpu.tx_limits = [0,1,7,0,32,32,8]
        cpu.call('_platform_vdp_send', [start,len(content)])
        self.assertEqual(cpu.output,content)
        self.assertEqual(cpu.reads,list(range(start,start+len(content))))
        self.assertEqual(cpu.events,['flush','poll','burst','resume','poll','burst','poll','burst','poll','burst','resume','poll','burst','poll','burst','poll','burst','poll'])

    def test_vdp_send_zero_length_flushes_without_reading_or_transmitting(self):
        cpu = LinkedC(self.image)
        cpu.call('_platform_vdp_send', [0, 0])
        self.assertEqual(cpu.output, b'')
        self.assertEqual(cpu.reads, [])
        self.assertEqual(cpu.events, ['flush'])

    def test_actual_boot_helpers_send_exact_e2_frames(self):
        for name, expected in (('_platform_boot_ready', b'\x17\x00\xe2\x01'),
                               ('_platform_boot_trial', b'\x17\x00\xe2\x00')):
            with self.subTest(function=name):
                cpu = LinkedC(self.image)
                cpu.call(name)
                self.assertEqual(cpu.output, expected)
                self.assertEqual(cpu.events, ['flush', 'poll', 'burst', 'poll'])

    def test_runtime_crc_reads_exact_current_bytes_and_matches_ieee(self):
        for data in (b'', b'123456789', b'\0', bytes(range(256))):
            with self.subTest(length=len(data)):
                cpu = LinkedC(self.image)
                start = cpu.data(data)
                result = cpu.call('_vios_crc32', [start, len(data)])
                self.assertEqual(result, zlib.crc32(data))
                self.assertEqual(cpu.reads, list(range(start, start+len(data))))

    def test_all_linked_string_copy_loops_preserve_first_byte_and_terminator(self):
        self.assertTrue(self.image.copies, 'production string-copy helpers were not linked')
        for function in self.image.copies:
            for data in (b'\0', b'First Last\0'):
                with self.subTest(function=hex(function), data=data):
                    cpu = LinkedC(self.image)
                    start = cpu.data(data)
                    destination = 0xa0010
                    cpu.mem[destination-1:destination+len(data)+1] = b'\x5a'*(len(data)+2)
                    cpu.call(function, [destination, start])
                    self.assertEqual(cpu.mem[destination:destination+len(data)], data)
                    self.assertEqual(cpu.mem[destination-1], 0x5a)
                    self.assertEqual(cpu.mem[destination+len(data)], 0x5a)
                    self.assertEqual(cpu.reads, list(range(start, start+len(data))))

    def test_runtime_string_output_preserves_current_character(self):
        cpu = LinkedC(self.image)
        data = b'First and last\0'
        start = cpu.data(data)
        cpu.call('_vios_out', [start])
        self.assertEqual(cpu.output, data[:-1])
        self.assertEqual(cpu.reads, list(range(start, start+len(data))))


if __name__ == '__main__':
    unittest.main()

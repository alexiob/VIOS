"""Execute the built SRAM bootstrap's real instruction bytes; no hardware I/O."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Stage0Machine:
    """Small ADL interpreter, deliberately rejecting every unmodelled opcode.

    I/O reads come from executed OUT0 writes, with an optional bus-read fault.
    This checks instruction semantics and relocation, not electrical RAM decode.
    """
    def __init__(self, code, base, fault=None):
        self.base, self.pc, self.end = base, base, base+len(code)
        self.metadata = self.hl = base+0x100
        self.sp, self.a, self.interrupts_enabled = 0x913572, 0x5A, True
        self.memory = {base+i: value for i, value in enumerate(code)}
        self.io = {port: 0x39 for port in (0xA8, 0xA9, 0xAA, 0xF0)}
        self.fault, self.events, self.io_writes, self.io_reads = fault, [], [], []
        self.loop_pc = None

    def fetch(self):
        assert self.base <= self.pc < self.end, 'Execution left the SRAM program'
        value = self.memory[self.pc]
        self.pc = (self.pc+1) & 0xFFFFFF
        return value

    def run(self):
        for _ in range(100):
            start = self.pc
            opcode = self.fetch()
            if opcode == 0xF3:                         # DI
                self.interrupts_enabled = False
                self.events.append(('di', start))
            elif opcode == 0x3E:                       # LD A,n
                self.a = self.fetch()
            elif opcode == 0xED:
                operation, port = self.fetch(), self.fetch()
                if operation == 0x39:                 # OUT0 (n),A
                    self.io[port] = self.a
                    self.io_writes.append((port, self.a))
                    self.events.append(('write', port, self.a))
                elif operation == 0x38:               # IN0 A,(n)
                    self.a = self.fault[1] if self.fault and port == self.fault[0] else self.io.get(port, 0x39)
                    self.io_reads.append((port, self.a))
                    self.events.append(('read', port, self.a))
                else:
                    raise AssertionError(f'Unsupported ED opcode {operation:02X} at {start:06X}')
            elif opcode == 0x77:                       # LD (HL),A
                self.memory[self.hl] = self.a
                self.events.append(('store', self.hl, self.a))
            elif opcode == 0x23:                       # INC HL, 24-bit ADL
                self.hl = (self.hl+1) & 0xFFFFFF
            elif opcode == 0x18:                       # JR signed displacement
                displacement = self.fetch()
                self.pc = (self.pc+displacement-(256 if displacement & 0x80 else 0)) & 0xFFFFFF
                assert self.pc == start, 'Bootstrap must finish in its self-loop'
                self.loop_pc = start
                self.events.append(('loop', start))
                return
            else:
                raise AssertionError(f'Unsupported opcode {opcode:02X} at {start:06X}')
        raise AssertionError('Bootstrap exceeded the native instruction budget')

    def metadata_bytes(self):
        return bytes(self.memory.get(self.metadata+i, 0) for i in range(5))


class Stage0Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(['make', 'build/agon-stage0.bin'], cwd=ROOT, check=True,
                       capture_output=True, text=True)
        cls.code = (ROOT/'build/agon-stage0.bin').read_bytes()

    def machine(self, base, fault=None):
        machine = Stage0Machine(self.code, base, fault)
        machine.run()
        return machine

    def test_native_cs0_configuration_and_actual_readbacks_at_both_sram_bases(self):
        for base in (0xB7E000, 0xFFE000):
            with self.subTest(base=hex(base)):
                cpu = self.machine(base)
                self.assertEqual(cpu.io_writes, [(0xA9, 0x0B), (0xA8, 0x04),
                                                  (0xF0, 0x01), (0xAA, 0x08)])
                self.assertEqual(cpu.io_reads, [(0xA8, 0x04), (0xA9, 0x0B),
                                                 (0xAA, 0x08), (0xF0, 0x01)])
                self.assertEqual(cpu.metadata_bytes(), b'\x04\x0b\x08\x01\xa5')
                self.assertEqual(cpu.events[0], ('di', base))
                self.assertFalse(cpu.interrupts_enabled)
                self.assertEqual(cpu.loop_pc, base+len(self.code)-2)

    def test_completion_is_last_and_all_prior_io_readbacks_are_recorded(self):
        for base in (0xB7E000, 0xFFE000):
            with self.subTest(base=hex(base)):
                cpu = self.machine(base)
                stores = [event for event in cpu.events if event[0] == 'store']
                self.assertEqual(stores, [('store', cpu.metadata+i, value)
                                         for i, value in enumerate((4, 11, 8, 1, 0xA5))])
                magic = cpu.events.index(stores[-1])
                self.assertEqual([event for event in cpu.events[:magic] if event[0] == 'read'],
                                 [('read', port, value) for port, value in cpu.io_reads])
                self.assertEqual(cpu.events[magic+1:], [('loop', cpu.loop_pc)])

    def test_no_stack_or_flash_writes_and_bootstrap_code_stays_unchanged(self):
        for base in (0xB7E000, 0xFFE000):
            with self.subTest(base=hex(base)):
                cpu = self.machine(base)
                self.assertEqual(cpu.sp, 0x913572)
                writes = [address for kind, address, *_ in cpu.events if kind == 'store']
                self.assertEqual(writes, list(range(cpu.metadata, cpu.metadata+5)))
                self.assertTrue(all(base <= address < base+0x2000 for address in writes))
                self.assertEqual(bytes(cpu.memory[base+i] for i in range(len(self.code))), self.code)

    def test_faulted_io_read_is_reported_not_replaced_with_expected_constant(self):
        for base in (0xB7E000, 0xFFE000):
            with self.subTest(base=hex(base)):
                cpu = self.machine(base, fault=(0xAA, 0x39))
                self.assertEqual(cpu.io[0xAA], 8, 'OUT0 must still program the requested control value')
                self.assertEqual(cpu.metadata_bytes(), b'\x04\x0b\x39\x01\xa5')
                self.assertEqual(cpu.io_reads[2], (0xAA, 0x39))
                self.assertNotEqual(cpu.metadata_bytes(), b'\x04\x0b\x08\x01\xa5',
                                    'Firmware must reject this completed but mismatched mapping report')


if __name__ == '__main__':
    unittest.main()

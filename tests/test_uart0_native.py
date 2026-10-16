"""Execute production UART init machine bytes against banked UART registers.

This never opens hardware. The tiny executor rejects instructions outside the
straight-line initializer instead of interpreting source text or expected values.
"""
from pathlib import Path
import os
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


def assemble_uart(source, output, baud=None):
    output.mkdir(parents=True, exist_ok=True)
    prefix = str(Path(os.environ.get('AGONDEV', str(ROOT/'.tools/agondev')))/'bin/ez80-none-elf')
    subprocess.run([prefix+'-as', '-march=ez80+full'] + ([] if baud is None else ['--defsym', f'VIOS_UART0_BAUD={baud}']) + ['-o', str(output/'uart.o'), str(source)],
                   check=True, capture_output=True)
    subprocess.run([prefix+'-ld', '-Ttext=0x1000', '-e', '_ez80_uart0_raw_init',
                    '-o', str(output/'uart.elf'), str(output/'uart.o')], check=True, capture_output=True)
    subprocess.run([prefix+'-objcopy', '-O', 'binary', '-j', '.text', str(output/'uart.elf'),
                    str(output/'uart.bin')], check=True, capture_output=True)
    (output/'uart.dis').write_text(subprocess.check_output(
        [prefix+'-objdump', '-d', str(output/'uart.elf')], text=True))
    return (output/'uart.bin').read_bytes()


class BankedUART:
    def __init__(self, io, *, lcr=0, dll=1, dlm=0, ier=0, fifo=0, modem=0):
        self.io = dict(io)
        self.lcr, self.dll, self.dlm = lcr, dll, dlm
        self.ier, self.fifo, self.modem = ier, fifo, modem
        self.writes = []
        self.transmitted = []

    def ready(self, port):
        if port in (0xa3, 0xa4, 0xa5):
            assert not self.io[0xdb] & 0x80, 'Port D clock is disabled'
        if 0xc0 <= port <= 0xc6:
            assert not self.io[0xdb] & 1, 'UART0 clock is disabled'

    def read(self, port):
        self.ready(port)
        assert port in self.io, f'unexpected input port {port:02X}'
        return self.io[port]

    def write(self, port, value):
        self.ready(port)
        dlab = bool(self.lcr & 0x80)
        self.writes.append((port, value, dlab))
        if port == 0xc0:
            if dlab:
                self.dll = value
            else:
                self.transmitted.append(value)
        elif port == 0xc1:
            if dlab:
                self.dlm = value
            else:
                self.ier = value
        elif port == 0xc2:
            self.fifo = value
        elif port == 0xc3:
            self.lcr = value
        elif port == 0xc4:
            self.modem = value
        else:
            assert port in self.io, f'unexpected output port {port:02X}'
            self.io[port] = value

    def execute(self, code):
        pc, a = 0, 0
        for _ in range(128):
            op = code[pc]
            pc += 1
            if op == 0xc9:
                assert pc == len(code), 'unexpected instructions after RET'
                return
            if op == 0xed:
                extended, port = code[pc:pc+2]
                pc += 2
                if extended == 0x38:
                    a = self.read(port)
                elif extended == 0x39:
                    self.write(port, a)
                else:
                    raise AssertionError(f'unsupported ED opcode {extended:02X}')
            elif op in (0x3e, 0xe6, 0xf6):
                immediate = code[pc]
                pc += 1
                if op == 0x3e:
                    a = immediate
                elif op == 0xe6:
                    a &= immediate
                else:
                    a |= immediate
            elif op == 0xaf:
                a = 0
            else:
                raise AssertionError(f'unsupported opcode {op:02X}')
        raise AssertionError('UART initializer exceeded instruction budget')


class NativeUARTTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.code = assemble_uart(ROOT/'arch/ez80/uart0_raw.s', ROOT/'build/verification/uart0')

    def assert_uart(self, uart):
        self.assertEqual((uart.dll, uart.dlm), (10, 0))
        self.assertEqual(18_432_000 // (16 * ((uart.dlm << 8) | uart.dll)), 115200)
        self.assertEqual((uart.lcr, uart.ier, uart.fifo, uart.modem), (3, 0, 7, 2))
        self.assertEqual(uart.transmitted, [], 'divisor setup accidentally transmitted a byte')
        # Literal bank trace distinguishes DLM from IER at the same address.
        self.assertEqual([item for item in uart.writes if item[0] in (0xc0, 0xc1)],
                         [(0xc0, 10, True), (0xc1, 0, True), (0xc1, 0, False)])
        self.assertEqual([value for port, value, _ in uart.writes if port == 0xc3], [0x83, 3])

    def test_selected_rates_initialize_cold_clocks_and_inherited_dlab(self):
        for rate, divisor in ((115200,10),(384000,3),(1152000,1)):
            code=assemble_uart(ROOT/'arch/ez80/uart0_raw.s', ROOT/f'build/verification/uart0-{rate}', rate)
            for lcr in (0,0x80,0xff):
                with self.subTest(rate=rate,lcr=lcr):
                    uart=BankedUART({0xdb:0xff,0xa3:0x5a,0xa4:0xb7,0xa5:0xc0},lcr=lcr,dll=0xee,dlm=0xff,ier=0xff)
                    uart.execute(code)
                    self.assertEqual((uart.dll,uart.dlm),(divisor,0))
                    self.assertEqual((uart.lcr,uart.ier,uart.fifo,uart.modem),(3,0,7,2))
                    self.assertEqual(uart.transmitted,[])
                    self.assertEqual(uart.io,{0xdb:0x7e,0xa3:0x5f,0xa4:0xb0,0xa5:0xcf})

    def test_fresh_reset_establishes_clocks_pins_and_115200_without_mos(self):
        uart = BankedUART({0xdb: 0xff, 0xa3: 0xff, 0xa4: 0, 0xa5: 0})
        uart.execute(self.code)
        self.assertEqual(uart.io, {0xdb: 0x7e, 0xa3: 0xff, 0xa4: 0, 0xa5: 0x0f})
        self.assert_uart(uart)

    def test_adversarial_state_restores_all_uart_banks_and_preserves_other_pins(self):
        cases = [
            ({0xdb: 0xa5, 0xa3: 0x5a, 0xa4: 0xb7, 0xa5: 0xc0},
             {0xdb: 0x24, 0xa3: 0x5f, 0xa4: 0xb0, 0xa5: 0xcf}, 0xff),
            ({0xdb: 0, 0xa3: 0, 0xa4: 0xff, 0xa5: 0xf0},
             {0xdb: 0, 0xa3: 0x0f, 0xa4: 0xf0, 0xa5: 0xff}, 3),
        ]
        for initial, expected, lcr in cases:
            with self.subTest(initial=initial):
                uart = BankedUART(initial, lcr=lcr, dll=0xef, dlm=0xbe, ier=0xa5, fifo=0xa7, modem=0xfc)
                uart.execute(self.code)
                self.assertEqual(uart.io, expected)
                self.assert_uart(uart)

    def test_every_clock_and_pin_bit_preserves_unrelated_state(self):
        for initial in range(256):
            with self.subTest(initial=initial):
                uart = BankedUART({0xdb: initial, 0xa3: initial, 0xa4: initial, 0xa5: initial}, lcr=0x80)
                uart.execute(self.code)
                self.assertEqual(uart.io[0xdb] & 0x81, 0)
                self.assertEqual(uart.io[0xdb] & 0x7e, initial & 0x7e)
                self.assertEqual([uart.io[port] & 0xf0 for port in (0xa3, 0xa4, 0xa5)], [initial & 0xf0]*3)
                self.assertEqual([uart.io[port] & 0x0f for port in (0xa3, 0xa4, 0xa5)], [0x0f, 0, 0x0f])
                self.assert_uart(uart)


if __name__ == '__main__':
    unittest.main()

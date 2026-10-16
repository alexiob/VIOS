"""Native FIFO collection and CTS-gated peer timing; never opens hardware."""
from collections import deque
from pathlib import Path
import os
import re
import subprocess
from types import SimpleNamespace
import unittest
from test_vios_native_c import LinkedC, ROOT, MASK


class Peer:
    """A 16-byte UART FIFO; one character may finish after RTS goes inactive."""
    def __init__(self, data=b'', ignore_rts=False):
        self.data, self.at = data, 0
        self.fifo = deque()
        self.mctl = 2
        self.inflight = False
        self.phase = self.overruns = 0
        self.error = False
        self.ignore_rts = ignore_rts
        self.period = 16
    def tick(self, count=1):
        for _ in range(count):
            if self.at == len(self.data) or not (self.mctl & 2 or self.inflight or self.ignore_rts):
                continue
            self.phase += 1
            if self.phase < self.period:
                continue
            self.phase = 0
            if len(self.fifo) == 16:
                self.overruns += 1
                self.error = True
            else:
                self.fifo.append(self.data[self.at])
            self.at += 1
            if not self.mctl & 2:
                self.inflight = False
    def read(self, port):
        if port == 0xc4:
            return self.mctl
        if port == 0xc5:
            result = 0x20 | bool(self.fifo) | (2 if self.error else 0)
            self.error = False
            return result
        assert port == 0xc0 and self.fifo, f'unexpected UART read {port:x}'
        return self.fifo.popleft()
    def write(self, port, value):
        assert port == 0xc4, f'unexpected UART write {port:x}'
        if self.mctl & 2 and not value & 2:
            self.inflight = self.at < len(self.data)
        self.mctl = value


class UARTCPU(LinkedC):
    def __init__(self, image, peer):
        super().__init__(image)
        for name in ('_ez80_uart0_tx_burst','_ez80_uart0_rx_resume'):
            self.hooks.pop(self.syms.get(name),None)  # Execute real UART bytes here.
        self.peer, self.steps = peer, 0
    def step(self):
        if not self.pc:
            return False
        self.peer.tick()
        self.steps += 1
        size, text = self.ins[self.pc]
        if text.startswith(('in0 ', 'out0 ')):
            op, rest = text.split(None, 1)
            left, right = (value.strip() for value in rest.split(','))
            if op == 'in0':
                value = self.peer.read(int(right[1:-1], 0))
                self.set(left, value)
                self.flags(value)
            else:
                self.peer.write(int(left[1:-1], 0), self.g(right))
            self.pc += size
            return True
        if text.startswith('djnz '):
            self.set('b', self.g('b')-1)
            displacement = int.from_bytes(self.mem[self.pc+size-1:self.pc+size], 'little', signed=True)
            self.pc += size
            if self.g('b'):
                self.pc += displacement
            return True
        if text == 'dec c':
            self.set('c', self.g('c')-1)
            self.z = self.g('c') == 0  # Byte DEC sets Z; pair DEC does not.
            self.pc += size
            return True
        return super().step()


def image():
    prefix = str(Path(os.environ.get('AGONDEV', str(ROOT/'.tools/agondev')))/'bin/ez80-none-elf')
    out = ROOT/'build/verification/uart0-rx'
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run([prefix+'-as', '-march=ez80+full', '-o', str(out/'uart.o'), str(ROOT/'arch/ez80/uart0.s')], check=True, capture_output=True)
    subprocess.run([prefix+'-ld', '-Ttext=0x40000', '-Tbss=0x80000', '-e', '_ez80_uart0_status', '-o', str(out/'uart.elf'), str(out/'uart.o')], check=True, capture_output=True)
    subprocess.run([prefix+'-objcopy', '-O', 'binary', '-j', '.text', str(out/'uart.elf'), str(out/'uart.bin')], check=True, capture_output=True)
    symbols = subprocess.check_output([prefix+'-nm', '-n', str(out/'uart.elf')], text=True)
    listing = subprocess.check_output([prefix+'-objdump', '-d', str(out/'uart.elf')], text=True)
    (out/'uart.dis').write_text(listing)
    syms = {values[2]:int(values[0],16) for line in symbols.splitlines() if len(values:=line.split()) == 3}
    instructions = {}
    for line in listing.splitlines():
        match = re.match(r'\s*([0-9a-f]+):\s*((?:[0-9a-f]{2} )+)\s*([^\t]+)\s*$', line)
        if match:
            instructions[int(match[1],16)] = (len(match[2].split()), match[3].split(' <')[0].strip())
    return SimpleNamespace(raw=(out/'uart.bin').read_bytes(), syms=syms, ins=instructions)


class NativeRXTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = image()
    def machine(self, peer):
        self.assertIn('_ez80_uart0_rx_drain', self.image.syms, 'Native paused FIFO drain is missing')
        return UARTCPU(self.image, peer)
    def test_native_drain_pauses_then_collects_fifo_before_slow_parser(self):
        peer = Peer();peer.fifo.extend(range(16));peer.mctl = 0x0b
        cpu = self.machine(peer)
        destination = 0x83010
        cpu.mem[destination-1:destination+33] = b'\xa5'*34
        n = cpu.call('_ez80_uart0_rx_drain', [destination]) & MASK
        self.assertEqual(n, 16)
        self.assertEqual(cpu.mem[destination:destination+n], bytes(range(16)))
        self.assertEqual(peer.mctl, 9, 'RTS must be inactive without changing other modem bits')
        self.assertEqual(cpu.mem[destination-1], 0xa5)
        self.assertEqual(cpu.mem[destination+32], 0xa5)
        self.assertLessEqual(cpu.steps, 16*12+32, 'FIFO drain regressed toward per-byte C parser cost')
    def test_cts_gating_preserves_256_bytes_despite_slow_parser(self):
        data = bytes(range(255))+b'\r'
        peer = Peer(data)
        cpu = self.machine(peer)
        received = bytearray();destination = 0x83010
        for _ in range(100):
            cpu.call('_ez80_uart0_rx_resume')
            peer.tick(8*peer.period)
            n = cpu.call('_ez80_uart0_rx_drain', [destination]) & MASK
            self.assertLessEqual(n, 32)
            received.extend(cpu.mem[destination:destination+n])
            self.assertFalse(peer.mctl & 2)
            peer.tick(10000)  # C parsing/REPL can take arbitrarily long while paused.
            if peer.at == len(data) and not peer.fifo:
                break
        self.assertEqual(received, data)
        self.assertEqual(peer.overruns, 0)
        self.assertEqual(cpu.rd(cpu.syms['_ez80_uart0_rx_overruns']), 0)
    def test_ignoring_peer_is_bounded_and_overruns_are_counted(self):
        peer = Peer(bytes(range(256))*4, ignore_rts=True);peer.period = 1
        cpu = self.machine(peer);destination = 0x83010
        cpu.mem[destination+32] = 0xa5
        n = cpu.call('_ez80_uart0_rx_drain', [destination]) & MASK
        self.assertEqual(n, 32)
        self.assertEqual(cpu.mem[destination+32], 0xa5)
        self.assertGreater(peer.overruns, 0)
        self.assertGreater(cpu.rd(cpu.syms['_ez80_uart0_rx_overruns']), 0)
    def test_status_reads_count_overrun_once_before_lsr_clears_it(self):
        peer = Peer();peer.error = True
        cpu = self.machine(peer)
        cpu.call('_ez80_uart0_status')
        self.assertTrue(cpu.r['a'] & 2)
        cpu.call('_ez80_uart0_status')
        self.assertFalse(cpu.r['a'] & 2)
        self.assertEqual(cpu.rd(cpu.syms['_ez80_uart0_rx_overruns']), 1)


class TXPeer(Peer):
    def __init__(self, *, cts_after=None, ready_after=None, error=False):
        super().__init__()
        self.transmitted = bytearray()
        self.cts_after, self.ready_after = cts_after, ready_after
        self.error = error
        self.lsr_reads = self.modem_reads = 0
    def read(self, port):
        if port == 0xc6:
            self.modem_reads += 1
            return 0x10 if self.cts_after is None or len(self.transmitted) < self.cts_after else 0
        if port == 0xc5:
            self.lsr_reads += 1
            result = super().read(port)
            return result if self.ready_after is None or len(self.transmitted) < self.ready_after else result & ~0x20
        return super().read(port)
    def write(self, port, value):
        if port == 0xc0:
            self.transmitted.append(value)
        else:
            super().write(port, value)


class NativeTXTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.image = image()
    def burst(self, data, length=None, **settings):
        self.assertIn('_ez80_uart0_tx_burst', self.image.syms, 'bounded native writer is missing')
        peer = TXPeer(**settings); cpu = UARTCPU(self.image, peer)
        start = cpu.data(data)
        ix, iy, sp = cpu.r['ix'], cpu.r['iy'], cpu.r['sp']
        n = cpu.call('_ez80_uart0_tx_burst', [start, len(data) if length is None else length]) & MASK
        self.assertEqual((cpu.r['ix'], cpu.r['iy'], cpu.r['sp']), (ix, iy, sp))
        self.assertEqual(cpu.reads, list(range(start, start+n)))
        return n, peer, cpu
    def test_burst_is_bounded_and_preserves_every_binary_byte_and_abi(self):
        for length in (1, 16, 32, 256, 0x10000):
            with self.subTest(length=length):
                n, peer, cpu = self.burst(bytes(range(32)), length)
                self.assertEqual(n, min(length,32))
                self.assertEqual(peer.transmitted, bytes(range(n)))
                self.assertLess(cpu.steps, 32*20+60)
    def test_empty_burst_does_not_read_source_or_uart(self):
        n, peer, _ = self.burst(b'', 0)
        self.assertEqual((n,peer.modem_reads,peer.lsr_reads),(0,0,0))
    def test_cts_deassertion_returns_partial_or_zero_progress(self):
        for allowed in (0,1,7):
            n, peer, _ = self.burst(bytes(range(16)), cts_after=allowed)
            self.assertEqual(n,allowed)
            self.assertEqual(peer.transmitted,bytes(range(allowed)))
    def test_thre_deassertion_returns_partial_or_zero_progress(self):
        for allowed in (0,1,7):
            n, peer, _ = self.burst(bytes(range(16)), ready_after=allowed)
            self.assertEqual(n,allowed)
            self.assertEqual(peer.transmitted,bytes(range(allowed)))
    def test_all_transmit_lsr_reads_account_for_clear_on_read_overruns(self):
        for allowed in (0,7):
            n, peer, cpu = self.burst(bytes(range(16)), ready_after=allowed, error=True)
            self.assertEqual(n,allowed)
            self.assertEqual(cpu.rd(cpu.syms['_ez80_uart0_rx_overruns']),1)


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
"""Estimate workbench composition cost by running the linked eZ80 code.

Runs forth_init, loads the plain fast-display fixture into a WBFAST draft and
times vios_workbench_compose for full, incremental and zoom frames. Uses the
semantic interpreter from tests/test_vios_native_c.py, extended with the few
instructions these paths need. No serial port, board or VDP is touched.

Cost model: one cycle per bus byte (opcode bytes fetched plus data bytes read
or written) at 18.432 MHz. CS0 is configured for 0 wait states. On the
2026-10-16 code this model gives 118.3 ms for a full compose after WB-REPAINT
against 125.7 ms measured on the board; use it to compare builds, not as a
replacement for tools/vios_fast_display_bench.py.

Render is not run: after each frame the painted plane is set to the desired
plane and the dirty counters are zeroed, as a full drain would leave them.

  .venv/bin/python tools/verification/compose_cycles.py [--profile] [ELF ...]
"""
import argparse
import bisect
import hashlib
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT/'tests'), str(ROOT/'tools')]
from test_vios_native_c import LinkedC, TargetImage, MASK  # noqa: E402
from vios_incremental_bench import FIXTURE  # noqa: E402

HZ = 18.432e6
CC = {0xc2:'nz', 0xca:'z', 0xd2:'nc', 0xda:'c', 0xe2:'po', 0xea:'pe', 0xf2:'p', 0xfa:'m'}
BYTE = ('a', 'b', 'c', 'd', 'e', 'h', 'l', 'iyl', 'iyh', 'ixl', 'ixh')


class CycleCPU(LinkedC):
    def __init__(self, image, elf):
        super().__init__(image)
        self.elf, self.cycles, self.profile = elf, 0, None
        functions = sorted((v, k) for k, v in image.syms.items() if k.startswith('_'))
        self.starts = [a for a, _ in functions]
        self.names = [n for _, n in functions]

    def rd(self, address, n=3):
        self.cycles += n
        return int.from_bytes(self.mem[address:address+n], 'little')

    def wr(self, address, value, n=3):
        self.cycles += n
        self.mem[address:address+n] = (value & ((1 << (n*8))-1)).to_bytes(n, 'little')

    def helper(self, name):
        self.cycles += 20  # intercepted __frameset/__frameset0: about their real size
        return super().helper(name)

    def decode(self, pc):
        # Runtime helpers jump into the middle of a JP cc,nn to skip bytes.
        if self.mem[pc] in CC:
            target = int.from_bytes(self.mem[pc+1:pc+4], 'little')
            self.ins[pc] = (4, f'jp {CC[self.mem[pc]]},0x{target:x}')
            return
        prefix = str(ROOT/'.tools/agondev/bin/ez80-none-elf-objdump')
        listing = subprocess.check_output([prefix, '-d', f'--start-address={pc}',
                                           f'--stop-address={pc+8}', str(self.elf)], text=True)
        for line in listing.splitlines():
            match = re.match(r'\s*([0-9a-f]+):\s*((?:[0-9a-f]{2} )+)\s*([^\t]+)\s*$', line)
            if match:
                self.ins.setdefault(int(match[1], 16), (len(match[2].split()), match[3].split(' <')[0].strip()))

    def step(self):
        if self.pc in self.hooks or not self.pc:
            return super().step()
        if self.pc not in self.ins:
            self.decode(self.pc)
        before, owner = self.cycles, self.names[bisect.bisect_right(self.starts, self.pc)-1]
        size, text = self.ins[self.pc]
        self.cycles += size
        if not self.extended(size, text):
            super().step()
        if self.profile is not None:
            self.profile[owner] = self.profile.get(owner, 0)+self.cycles-before
        return True

    def extended(self, size, text):
        """Instructions the base interpreter lacks. Returns False to defer."""
        parts = text.split(None, 1)
        op = parts[0]
        args = [x.strip() for x in parts[1].split(',')] if len(parts) > 1 else []
        operand = lambda x: self.rd(self.addr(x), 1) if x.startswith('(') else self.g(x)
        if '.' in op:
            base = op.split('.')[0]
            if base == 'ld':
                dst, src = args
                value = self.rd(self.addr(src), 2) if src.startswith('(') else self.g(src) & 0xffff
                if dst.startswith('('):
                    self.wr(self.addr(dst), value, 2)
                else:
                    self.set(dst, value)
            elif base in ('inc', 'dec'):
                self.set(args[0], (self.g(args[0])+(1 if base == 'inc' else -1)) & 0xffff)
            elif base in ('add', 'sbc'):
                left, right = self.g(args[0]) & 0xffff, self.g(args[1]) & 0xffff
                result = left+right if base == 'add' else left-right-self.c
                self.set(args[0], result & 0xffff)
                if base == 'add':
                    self.c = result > 0xffff
                else:
                    self.flags(result, 16, result < 0)
            else:
                raise AssertionError(text)
        elif op in ('push', 'pop') and args[0] == 'af':
            if op == 'push':
                self.push((self.r['a'] << 8) | (self.s << 7) | (self.z << 6) | (self.p << 2) | self.c)
            else:
                value = self.pop()
                self.r['a'] = (value >> 8) & 255
                self.s, self.z, self.p, self.c = (bool(value & bit) for bit in (128, 64, 4, 1))
        elif op == 'ld' and args[0].startswith('(') and args[1][0].isdigit():
            # ld (hl),n and ld (ix/iy+d),n store one byte. The base interpreter
            # stores three, spilling two zeros past e.g. a scroll's blank row.
            self.wr(self.addr(args[0]), int(args[1], 0), 1)
        elif op == 'ex' and args[0] == '(sp)':
            value = self.rd(self.r['sp'])
            self.wr(self.r['sp'], self.g(args[1]))
            self.set(args[1], value)
        elif op == 'pea':
            self.pc += size
            self.push(self.addr('('+args[0]+')'))
            return True
        elif op == 'djnz':
            displacement = int.from_bytes(self.mem[self.pc+size-1:self.pc+size], 'little', signed=True)
            self.pc += size
            self.set('b', self.g('b')-1)
            if self.g('b'):
                self.pc = (self.pc+displacement) & MASK
                self.cycles += 1
            return True
        elif op in ('ldir', 'lddr', 'ldi', 'ldd'):
            step = 1 if op in ('ldir', 'ldi') else -1
            while True:
                self.wr(self.r['de'], self.rd(self.r['hl'], 1), 1)
                self.r['hl'] = (self.r['hl']+step) & MASK
                self.r['de'] = (self.r['de']+step) & MASK
                self.r['bc'] = (self.r['bc']-1) & MASK
                self.p = self.r['bc'] != 0
                if op in ('ldi', 'ldd') or not self.r['bc']:
                    break
        elif op in ('cpi', 'cpd', 'cpir'):
            while True:
                value = self.rd(self.r['hl'], 1)
                self.z, self.s = self.r['a'] == value, bool((self.r['a']-value) & 0x80)
                self.r['hl'] = (self.r['hl']+(-1 if op == 'cpd' else 1)) & MASK
                self.r['bc'] = (self.r['bc']-1) & MASK
                self.p = self.r['bc'] != 0
                if op != 'cpir' or self.z or not self.p:
                    break
        elif op == 'sub':
            left, right = self.g('a'), operand(args[-1])
            self.set('a', left-right)
            self.flags(left-right, 8, left < right)
        elif op == 'adc':
            left, right = self.g(args[0]), self.g(args[1])
            result, bits = left+right+self.c, 8 if args[0] == 'a' else 24
            self.set(args[0], result)
            self.flags(result, bits, result >> bits != 0)
        elif op == 'add' and args[0] == 'a':
            left, right = self.g('a'), operand(args[1])
            self.set('a', left+right)
            self.flags(left+right, 8, left+right > 255)
        elif op == 'mlt':
            value = self.r[args[0]]
            self.r[args[0]] = ((value >> 8) & 255)*(value & 255)
        elif op == 'neg':
            value = self.g('a')
            self.set('a', -value)
            self.flags(-value, 8, value != 0)
        elif op in ('scf', 'ccf'):
            self.c = True if op == 'scf' else not self.c
        elif op == 'cpl':
            self.set('a', ~self.g('a'))
        elif op in ('rla', 'rra', 'rlca', 'rrca', 'rl', 'rr', 'rlc', 'rrc', 'sla', 'sra', 'srl'):
            register = args[0] if args else 'a'
            value = self.g(register)
            left = op in ('rla', 'rlca', 'rl', 'rlc', 'sla')
            carry = bool(value & (0x80 if left else 1))
            if op in ('rla', 'rl'):
                result = (value << 1) | self.c
            elif op in ('rra', 'rr'):
                result = (value >> 1) | (0x80 if self.c else 0)
            elif op in ('rlca', 'rlc'):
                result = (value << 1) | carry
            elif op in ('rrca', 'rrc'):
                result = (value >> 1) | (0x80 if carry else 0)
            elif op == 'sla':
                result = value << 1
            elif op == 'srl':
                result = value >> 1
            else:
                result = (value >> 1) | (value & 0x80)
            self.set(register, result & 255)
            if op in ('rla', 'rra', 'rlca', 'rrca'):
                self.c = carry
            else:
                self.flags(result, 8, carry)
        elif op in ('res', 'set'):
            bit, target = args
            value = operand(target)
            value = value & ~(1 << int(bit)) if op == 'res' else value | (1 << int(bit))
            if target.startswith('('):
                self.wr(self.addr(target), value, 1)
            else:
                self.set(target, value)
        elif op == 'bit' and args[1].startswith('('):
            self.z = not (operand(args[1]) & (1 << int(args[0])))
        elif op in ('inc', 'dec') and (args[0].startswith('(') or args[0] in BYTE):
            value = (operand(args[0])+(1 if op == 'inc' else -1)) & 255
            if args[0].startswith('('):
                self.wr(self.addr(args[0]), value, 1)
            else:
                self.set(args[0], value)
            self.z, self.s = value == 0, bool(value & 0x80)
        else:
            return False
        self.pc += size
        return True


class Workbench:
    def __init__(self, elf):
        image = TargetImage(Path(elf))
        self.cpu = CycleCPU(image, Path(elf))
        self.syms = image.syms
        self.cpu.r['sp'] = 0xaeff0
        for name in ('_forth_init', '_vios_text_reset', '_vios_workbench_init', '_vios_workbench_open'):
            self.call(name)
        names = 0x9a000
        self.cpu.mem[names:names+10] = b'WBFAST\0\0' b'1\0'
        assert self.call('_vios_workbench_new', (names, names+8, 1)) == 1
        chunk = 0x9b000
        for offset in range(0, len(FIXTURE), 80):
            text = FIXTURE[offset:offset+80].encode()
            self.cpu.mem[chunk:chunk+len(text)] = text
            revision = self.revision()
            # A uint32_t argument occupies two 3-byte stack slots.
            assert self.call('_vios_workbench_patch', (revision & MASK, revision >> 24, offset, 0, chunk, len(text))) == 1

    def call(self, name, args=()):
        return self.cpu.call(name, args, budget=10**8) & MASK

    def revision(self):
        status = 0x9c000
        self.call('_vios_workbench_status', (status,))
        return self.cpu.rd(status+36, 4)  # seven pointers and five unsigneds precede it

    def settle(self):
        cpu, desired, painted = self.cpu, self.syms['_desired'], self.syms['_painted']
        cpu.mem[painted:painted+9600] = cpu.mem[desired:desired+9600]
        cpu.mem[self.syms['_dirty_cells']:self.syms['_dirty_cells']+3] = bytes(3)
        cpu.mem[self.syms['_dirty_rows']:self.syms['_dirty_rows']+60] = bytes(60)
        if '_painted_blank' in self.syms:
            cpu.mem[self.syms['_painted_blank']] = 0
        # The renderer sends a queued scroll before the next frame; left set,
        # it would refuse every later vios_text_scroll.
        if '_scroll_pending' in self.syms:
            cpu.mem[self.syms['_scroll_pending']] = 0

    def compose(self, profile=False):
        cpu = self.cpu
        cpu.cycles, cpu.profile = 0, {} if profile else None
        self.call('_vios_workbench_compose')
        cycles, found = cpu.cycles, cpu.profile
        cpu.profile = None
        desired = self.syms['_desired']
        frame = hashlib.sha256(cpu.mem[desired:desired+9600]).hexdigest()[:16]
        self.settle()
        return cycles, frame, found


SCENARIOS = [
    ('full after WB-REPAINT', [('_vios_text_reset', ())]),
    ('unchanged', []),
    ('TAB to editor', [('_vios_workbench_key', (9, 0))]),
    ('character insert', [('_vios_workbench_key', (90, 0))]),
    ('character delete', [('_vios_workbench_key', (8, 0))]),
    ('newline insert', [('_vios_workbench_key', (13, 0))]),
    ('newline delete', [('_vios_workbench_key', (8, 0))]),
    ('source scroll up', [('_vios_workbench_key', (256, 0))]),
    ('source scroll down', [('_vios_workbench_key', (257, 0))]),
    ('zoom on (painted)', [('_vios_workbench_zoom', (1,))]),
    ('zoom off (painted)', [('_vios_workbench_zoom', (0,))]),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('elf', nargs='*', default=[str(ROOT/'build/vios-resident.elf')])
    parser.add_argument('--profile', action='store_true', help='per-function cycles for the full frame')
    options = parser.parse_args()
    for elf in options.elf:
        bench = Workbench(elf)
        bench.compose()
        print(elf)
        for label, actions in SCENARIOS:
            for name, args in actions:
                bench.call(name, args)
            # Key handling is outside the compose interval, as on the board.
            cycles, frame, profile = bench.compose(options.profile and label.startswith('full'))
            print(f'  {label:24s} {cycles:9d} cycles {cycles/HZ*1e3:8.2f} ms  desired {frame}')
            if profile:
                total = sum(profile.values())
                for cost, name in sorted(((v, k) for k, v in profile.items()), reverse=True)[:12]:
                    print(f'      {name:30s} {cost:9d} {100*cost/total:5.1f}%')


if __name__ == '__main__':
    main()

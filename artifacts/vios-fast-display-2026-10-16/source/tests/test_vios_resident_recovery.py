"""Recovery workflow checks using real host framing, never actual hardware."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
from vios_resident_board import ResidentBoard
from vios_resident_transport import ResidentSession
from vios_wire import Decoder, Packet, encode


class Clock:
    def __init__(self): self.now = 0
    def __call__(self): self.now += .01; return self.now
    def sleep(self, seconds): self.now += seconds


class Peer:
    """Only the serial boundary is replaced; board state/framing stays real."""
    def __init__(self, clock, ack=True, retained=True, watchdog_delay=30):
        self.clock, self.ack, self.retained = clock, ack, retained
        self.decoder = Decoder(); self.pending = bytearray(); self.line = bytearray()
        self.controls = []; self.sources = []; self.definitions = set()
        self.paused = self.gpu_stuck = False; self.closed = self.resets = 0
        self.watchdog_at = None; self.watchdog_delay = watchdog_delay
        self.confirmed = 'A'; self.candidate = 'NONE'

    def frame(self, channel, sequence, payload):
        self.pending.extend(encode(Packet(channel, sequence, payload)))

    def response(self, envelope, body='', ok=True):
        nonce = envelope[1:9]
        self.frame(1, 0, (envelope+'\r\n'+body+'\r\n@'+nonce+
                         '\r\n'+('ok' if ok else 'error')+'\r\nvios> ').encode())

    def reset(self):
        self.resets += 1; self.definitions.clear(); self.paused = self.gpu_stuck = False

    def execute(self, envelope):
        self.sources.append(envelope)
        source = envelope[20:]
        if source == 'KEY .': return
        if source.startswith(': '): self.definitions.add(source.split()[1])
        if ' VDU' in source:
            values = [int(n) for n in re.findall(r'(\d+) VDU', source)]
            if values == [23, 0, 160, 1, 0, 1]: self.gpu_stuck = True; return
            if values == [23, 0, 226, 0]: self.watchdog_at = self.clock.now+self.watchdog_delay
        if source == '.S': body = '<0> '
        elif source == 'BOOT-STATUS':
            state = 'CLEAN' if self.candidate == 'NONE' else 'PENDING'
            body = f'boot confirmed {self.confirmed} candidate {self.candidate} state {state} current {self.confirmed} generation 7'
        elif source.startswith('SEE '):
            name = source.split()[1]
            return self.response(envelope, 'word '+name if name in self.definitions else 'unknown word: '+name,
                                 name in self.definitions)
        elif source == 'DECIMAL 6 7 * . CR': body = '42 '
        else: body = ''
        self.response(envelope, body)

    def write(self, data):
        for packet in self.decoder.feed(data):
            if packet.channel == 1:
                if packet.sequence:
                    self.line.extend(packet.payload)
                    if len(self.line) < 6: continue
                    size, crc = struct.unpack('<HI', self.line[:6])
                    if len(self.line) < size+6: continue
                    content = bytes(self.line[6:]); self.line.clear()
                    assert len(content) == size and zlib.crc32(content) == crc
                    self.frame(1, packet.sequence, b'LINE OK\r\n')
                    self.execute(content[:-1].decode())
                else: raise AssertionError('Recovery must not send raw KEY input')
            elif packet.channel == 2:
                command = packet.payload.rstrip(b'\n'); self.controls.append(command)
                if command == b'Z':
                    if self.ack: self.frame(2, packet.sequence, b'RESTARTING\r\n'); self.reset()
                elif command == b'B':
                    state = b'STARTING' if self.watchdog_at else b'HALTED' if self.paused else b'READY'
                    self.frame(2, packet.sequence, b'BOOT '+state+b'\r\n')
                elif command == b'I':
                    self.frame(2, packet.sequence, b'ID 0007 REV AA STAT '+(b'98' if self.paused else b'18')+b'\r\n')
                elif command in (b'H', b'S'):
                    self.paused = True
                    self.frame(2, packet.sequence, b'REGS 98 000000 000000 000000 000000 000000 000000 0AEFFF 040123\r\n')
                elif command == b'V0400400005':
                    self.frame(2, packet.sequence, b'MEM 040040 0005 '+f'{zlib.crc32(bytes.fromhex("4D4F530001")):08X}'.encode()+b' 4D4F530001\r\n')
                else: raise AssertionError('Unexpected control '+repr(command))

    def read(self, count=None):
        if self.watchdog_at and self.clock.now >= self.watchdog_at:
            self.watchdog_at = None
            self.frame(3, 0, b'BOOT failure 1/3; restarting ESP recovery\r\n')
            self.reset(); self.pending.extend(b'ESP32 ROM text\x00')
            if self.retained: self.frame(3, 0, b'BOOT retained failures 1\r\n')
        size = len(self.pending) if count is None else count
        data = bytes(self.pending[:size]); del self.pending[:size]
        return data

    def close(self): self.closed += 1


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.output = contextlib.redirect_stdout(io.StringIO()); self.output.__enter__()
        self.addCleanup(self.output.__exit__, None, None, None)

    def load(self):
        path = ROOT/'tools/vios_resident_recovery.py'
        self.assertTrue(path.is_file(), 'Standalone recovery acceptance driver is missing')
        spec = importlib.util.spec_from_file_location('recovery', path)
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        return module

    @contextlib.contextmanager
    def fixture(self, **kwargs):
        module = self.load(); clock = Clock(); peer = Peer(clock, **kwargs)
        with tempfile.TemporaryDirectory() as directory:
            report = module.Report(Path(directory)/'recovery.json', 'REC_TEST', ['recovery'])
            board = ResidentBoard(session=ResidentSession(peer), resident_factory=ResidentSession)
            with patch.object(module.time, 'monotonic', clock), patch.object(module.time, 'sleep', clock.sleep):
                yield module, module.Recovery(board, report, 'REC_TEST'), board, peer, report

    def test_cli_requires_explicit_reset_flag_before_creating_report_or_opening_board(self):
        module = self.load()
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stderr(io.StringIO()):
            path = Path(directory)/'never.json'
            with self.assertRaises(SystemExit) as error: module.main(['--log', str(path)])
            self.assertEqual(error.exception.code, 2); self.assertFalse(path.exists())

    def test_key_and_stuck_gpu_recover_on_same_raw_port_without_sd_or_image_mutation(self):
        with self.fixture() as (module, run, board, peer, report):
            run.attach(2); run.exercise()
            self.assertEqual(peer.resets, 2); self.assertEqual(peer.controls.count(b'Z'), 2)
            self.assertIs(board.session.raw, peer); self.assertEqual(peer.closed, 0)
            sources = [line[20:] for line in peer.sources]
            self.assertIn('DECIMAL 23 VDU 0 VDU 160 VDU 1 VDU 0 VDU 0 VDU 6 VDU 0 VDU 23 VDU 0 VDU 160 VDU 1 VDU 0 VDU 7 VDU', sources)
            self.assertIn('DECIMAL 23 VDU 0 VDU 160 VDU 1 VDU 0 VDU 1 VDU', sources)
            self.assertEqual(sources.count('DECIMAL 6 7 * . CR'), 2)
            self.assertNotIn('BOOT-TRIAL', sources); self.assertNotIn('BOOT-CONFIRM', sources)
            self.assertFalse(any(command.startswith(b'W') for command in peer.controls))
            self.assertTrue(all(item['status'] == 'passed' for item in report.data['checks']))

    def test_existing_pending_or_compile_state_is_rejected_before_any_wire_input(self):
        for field, value in [('pending_nonce', 'EXISTING'), ('compiling', True), ('uncertain', True)]:
            with self.subTest(field=field), self.fixture() as (module, run, board, peer, report):
                setattr(board, field, value)
                with self.assertRaises(module.AcceptanceError): run.attach(2)
                self.assertEqual(peer.controls, []); self.assertEqual(peer.sources, [])

    def test_lost_restart_ack_stops_without_retry_or_second_scenario(self):
        with self.fixture(ack=False) as (module, run, board, peer, report):
            run.attach(2)
            with self.assertRaises(TimeoutError): run.exercise()
            self.assertEqual(peer.controls.count(b'Z'), 1)
            self.assertTrue(board.transition_unknown)
            self.assertEqual(json.loads(report.path.read_text())['status'], 'failed')
            self.assertNotIn(' VDU', ' '.join(line[20:] for line in peer.sources))

    def test_watchdog_requires_reset_events_and_rewraps_same_raw_without_sending_z(self):
        with self.fixture() as (module, run, board, peer, report):
            run.attach(2); old = board.session; run.watchdog(timeout=40, connect_timeout=2)
            self.assertEqual(peer.resets, 1); self.assertNotIn(b'Z', peer.controls)
            self.assertIs(board.session.raw, peer); self.assertIsNot(board.session, old)
            self.assertEqual(peer.closed, 0); self.assertFalse(board.session.integrity_error)
            events = next(item['result'] for item in report.data['checks'] if item['name'] == 'watchdog-reset-events')
            self.assertIn('BOOT retained failures 1', ''.join(events['events']))
            self.assertGreaterEqual(events['elapsed_seconds'], 25)

    def test_watchdog_does_not_infer_reset_from_failure_message_without_retained_evidence(self):
        with self.fixture(retained=False) as (module, run, board, peer, report):
            run.attach(2); old = board.session
            with self.assertRaises(module.AcceptanceError): run.watchdog(timeout=35, connect_timeout=2)
            self.assertIs(board.session, old); self.assertNotIn(b'Z', peer.controls)

    def test_changed_confirmed_slot_fails_readback_without_repairing_sd(self):
        with self.fixture() as (module, run, board, peer, report):
            run.attach(2); peer.confirmed = 'B'
            with self.assertRaises(module.AcceptanceError): run.verify_boot('changed')
            self.assertEqual([line[20:] for line in peer.sources][-1], 'BOOT-STATUS')

    def test_pending_boot_candidate_is_rejected_before_any_reset_or_vdp_write(self):
        with self.fixture() as (module, run, board, peer, report):
            peer.candidate = 'B'
            with self.assertRaises(module.AcceptanceError): run.attach(2)
            self.assertNotIn(b'Z', peer.controls)
            self.assertFalse(any(' VDU' in line or line[20:].startswith(': ') for line in peer.sources))

    def test_premature_reset_does_not_pass_as_a_thirty_second_watchdog(self):
        with self.fixture(watchdog_delay=26) as (module, run, board, peer, report):
            run.attach(2)
            with self.assertRaises(module.AcceptanceError): run.watchdog(timeout=40, connect_timeout=2)
            self.assertNotIn(b'Z', peer.controls)
            self.assertEqual(report.data['checks'][-1]['status'], 'failed')

    def test_cli_runs_explicit_workflow_and_persists_final_status_before_closing(self):
        with self.fixture() as (module, run, board, peer, report):
            path = report.path.with_name('cli.json')
            with patch.object(module, 'ResidentBoard', return_value=board):
                self.assertEqual(module.main(['--exercise-resets', '--log', str(path), '--connect-timeout', '2']), 0)
            data = json.loads(path.read_text())
            self.assertEqual(data['status'], 'passed'); self.assertTrue(data['explicit_reset_authorization'])
            self.assertTrue(all(item['status'] == 'passed' for item in data['checks']))
            self.assertEqual(peer.resets, 2); self.assertEqual(peer.closed, 1)


if __name__ == '__main__': unittest.main()

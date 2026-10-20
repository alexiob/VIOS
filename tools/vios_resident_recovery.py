#!/usr/bin/env python3
"""Explicit resident recovery acceptance; unsaved RAM work WILL be lost.

Run only with the shared harness stopped and a known idle resident REPL:
  python3 tools/vios_resident_recovery.py --exercise-resets --log /tmp/recovery.json
  python3 tools/vios_resident_recovery.py --exercise-resets --watchdog

Connects without image upload. Exercises pending KEY and a deliberately infinite
VDP buffered jump, with one explicit supervisor reset per scenario. Optional
watchdog acceptance observes an automatic ESP reset without issuing Z. The
confirmed SD selection must be clean and is read back after every reset; no
source/module/boot file is written or deleted. Protocol results prove no visual
or audible behavior. Construction/import never opens hardware.
"""
import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import re
import secrets
import sys
import time
import zlib

try:
    from .vios_resident_acceptance import Report, AcceptanceError, require, synchronized, assert_numbers
    from .vios_resident_board import ResidentBoard
except ImportError:
    from vios_resident_acceptance import Report, AcceptanceError, require, synchronized, assert_numbers
    from vios_resident_board import ResidentBoard

ROOT = Path(__file__).resolve().parents[1]


def vdu_source(values):
    return 'DECIMAL '+' '.join(str(value)+' VDU' for value in values)


class Recovery:
    def __init__(self, board, report, namespace):
        require(re.fullmatch(r'[A-Z][A-Z0-9_]{2,23}', namespace) is not None, 'Invalid recovery namespace')
        self.board, self.report, self.marker = board, report, namespace+'-RAM'
        self.confirmed = None

    def command(self, source, numbers=None, timeout=5):
        result = synchronized(self.board.evaluate(source, timeout=timeout))
        if numbers is not None: result['numbers'] = assert_numbers(result, source, numbers)
        return result

    def attach(self, timeout=120):
        def attach():
            require(not (self.board.pending_nonce or self.board.compiling or self.board.uncertain
                         or self.board.paused or self.board.transition_unknown),
                    'Recovery acceptance requires an initially idle synchronized board')
            result = synchronized(self.board.connect(timeout=timeout))
            require(result.get('connected'), 'Resident attachment did not connect')
            return result
        self.report.check('attach-idle-resident-no-image-upload', attach)
        def empty_stack():
            result = synchronized(self.board.stacks())
            require(result.get('data_stack', {}).get('depth_text', '').strip() == '0',
                    'Recovery acceptance requires an initially empty data stack')
            return result
        self.report.check('require-empty-stack', empty_stack)
        def baseline():
            result = synchronized(self.board.boot_status()); boot = result.get('boot', {})
            require(boot.get('state') == 'CLEAN' and boot.get('candidate') == 'NONE'
                    and boot.get('confirmed') in ('A', 'B', 'NONE'),
                    'Refusing to reset a pending/tried startup selection')
            self.confirmed = boot['confirmed']
            return result
        self.report.check('record-clean-confirmed-sd-selection', baseline)

    def verify_boot(self, label):
        def action():
            result = synchronized(self.board.boot_status()); boot = result.get('boot', {})
            require(boot.get('confirmed') == self.confirmed and boot.get('current') == self.confirmed
                    and boot.get('candidate') == 'NONE' and boot.get('state') == 'CLEAN',
                    'Confirmed startup selection changed; no automatic SD repair will be attempted')
            return result
        return self.report.check(label+'-confirmed-sd-unchanged', action)

    def mark_ram(self, label):
        return self.report.check(label+'-define-transient-marker',
                                 lambda: self.command(': '+self.marker+' 123 ;'))

    def fresh(self, label):
        def absent():
            result = self.board.evaluate('SEE '+self.marker, timeout=5)
            require(result.get('ok') is False and not result.get('pending')
                    and not result.get('compiling')
                    and 'unknown word: '+self.marker in result.get('output', ''),
                    'Transient RAM marker survived or its absence was not proven')
            return result
        self.report.check(label+'-transient-marker-absent', absent)
        self.report.check(label+'-fresh-vm-42', lambda: self.command('DECIMAL 6 7 * . CR', [42]))
        self.verify_boot(label)

    def pending(self, source):
        result = self.board.evaluate(source, timeout=2)
        require(result.get('pending') is True and result.get('ok') is False,
                'Command did not remain pending as required: '+repr(result))
        return result

    def control(self, command):
        # The ordinary board API intentionally disallows evaluation while pending;
        # channel 2 remains the independent supervisor interface under this lock.
        with self.board.lock:
            return self.board.session.control(command, timeout=3)

    def gpu_control(self):
        boot = self.control(b'B')
        require(boot == b'BOOT READY\r\n', 'Supervisor BOOT request failed during stuck VDP: '+repr(boot))
        identity = self.control(b'I')
        require(re.fullmatch(br'ID 0007 REV AA STAT [0-9A-F]{2}\r\n', identity),
                'Supervisor identity request failed during stuck VDP: '+repr(identity))
        return {'boot': boot.decode(), 'identity': identity.decode()}

    def halt_diagnostic(self):
        # B/I are mandatory. A packet-drain timeout can legitimately make H fail;
        # record it explicitly, then exercise Z which must not depend on UART2.
        try:
            halt = self.control(b'H')
            if not halt.startswith(b'REGS 98 '): return {'halted': False, 'reply': halt.decode('ascii', 'replace')}
            self.board.paused = self.board.session.paused = True
            registers = self.control(b'S')
            require(re.fullmatch(br'REGS 98(?: [0-9A-F]{6}){8}\r\n', registers), 'Malformed halted register response')
            memory = self.control(b'V0400400005')
            match = re.fullmatch(br'MEM 040040 0005 ([0-9A-F]{8}) ([0-9A-F]{10})\r\n', memory)
            require(match is not None, 'Malformed header readback during stuck VDP')
            header = bytes.fromhex(match[2].decode())
            require(header == b'MOS\0\1' and zlib.crc32(header) == int(match[1], 16),
                    'Header/CRC mismatch during stuck VDP')
            return {'halted': True, 'registers': registers.decode(), 'header': header.hex()}
        except TimeoutError as error:
            return {'halted': False, 'error': str(error)}

    def force(self, label):
        def action():
            raw = self.board.session.raw
            result = synchronized(self.board.reboot(force=True, timeout=120))
            require(result.get('connected') and self.board.session.raw is raw,
                    'Forced recovery did not attach on the same raw serial session')
            return result
        self.report.check(label+'-explicit-force-z', action)
        self.fresh(label)

    def exercise(self):
        self.mark_ram('key')
        self.report.check('key-execution-pending', lambda: self.pending('KEY .'))
        self.force('key')
        self.mark_ram('gpu')
        # VDP buffered WRITE: buffer=1, command=0, length=6. Its content is
        # VDU 23,0,A0,1,0,7: JUMP back to buffer1. The subsequent CALL never ends.
        source = vdu_source([23, 0, 160, 1, 0, 0, 6, 0, 23, 0, 160, 1, 0, 7])
        self.report.check('gpu-store-self-jump-complete-ack', lambda: self.command(source))
        self.report.check('gpu-self-jump-execution-pending',
                          lambda: self.pending(vdu_source([23, 0, 160, 1, 0, 1])))
        self.report.check('gpu-independent-boot-and-identity', self.gpu_control)
        self.report.check('gpu-halt-register-and-header-diagnostic', self.halt_diagnostic)
        self.force('gpu')

    def watchdog(self, timeout=90, connect_timeout=120):
        require(math.isfinite(timeout) and 30 < timeout <= 300, 'Watchdog observation timeout must be >30 and <=300 seconds')
        self.mark_ram('watchdog')
        old = self.board.session
        old.events.clear()
        started = time.monotonic()
        self.report.check('watchdog-arm-complete-ack',
                          lambda: self.command(vdu_source([23, 0, 226, 0])))
        # Fresh CPU boot/ROM text is expected to invalidate this protected decoder.
        # It must never be used again for source or an old KEY continuation.
        self.board.deployed = False
        self.board.uncertain = self.board.transition_unknown = True
        def observe():
            events = []; failure_at = None; progress_at = started
            while time.monotonic()-started < timeout:
                old._pump()  # Decode events despite expected reset-time ROM noise.
                batch, old.events[:] = list(old.events), []
                for payload in batch:
                    text = payload.decode('ascii', 'replace'); events.append(text)
                    if re.fullmatch(r'BOOT failure 1/3; restarting ESP recovery\r\n', text):
                        failure_at = time.monotonic()-started
                    if text == 'BOOT retained failures 1\r\n' and failure_at is not None:
                        require(failure_at >= 29, 'Watchdog reset occurred before its expected 30-second deadline')
                        return {'events': events, 'elapsed_seconds': round(failure_at, 3),
                                'old_decoder_integrity_error': old.integrity_error,
                                'retained_failures': 1}
                require(len(events) <= 256, 'Excessive supervisor events during watchdog observation')
                if time.monotonic()-progress_at >= 5:
                    print(json.dumps({'checkpoint': 'watchdog-wait', 'elapsed_seconds': round(time.monotonic()-started, 1)}), flush=True)
                    progress_at = time.monotonic()
                time.sleep(.05)
            raise AcceptanceError('No matching watchdog timeout/reset plus retained-count=1 evidence: '+repr(events))
        self.report.check('watchdog-reset-events', observe)
        def reconnect():
            with self.board.lock:
                factory = self.board.resident_factory or type(old)
                self.board.session = factory(old.raw)
                self.board.paused = self.board.compiling = self.board.uncertain = self.board.transition_unknown = False
                self.board.pending_nonce = self.board.pending_source = self.board.pending_envelope = None
                self.board.pending_remaining = []; self.board.receive_buffer.clear()
                self.board.source_crc = True; self.board._attach_pending = False; self.board._resident_identity = None
                result = synchronized(self.board.connect(timeout=connect_timeout))
                require(result.get('connected') and self.board.session.raw is old.raw,
                        'Watchdog reattachment did not preserve raw serial session')
                return result
        self.report.check('watchdog-reattach-fresh-wrapper-no-z', reconnect)
        self.fresh('watchdog')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exercise-resets', action='store_true', help='Required: explicitly discard unsaved RAM and exercise supervisor resets')
    parser.add_argument('--watchdog', action='store_true', help='Also wait for a deliberate 30-second automatic watchdog reset')
    parser.add_argument('--port', default='/dev/cu.usbserial-224130')
    parser.add_argument('--log', type=Path)
    parser.add_argument('--connect-timeout', type=float, default=120)
    parser.add_argument('--watchdog-timeout', type=float, default=90)
    args = parser.parse_args(argv)
    if not args.exercise_resets: parser.error('--exercise-resets is required; this test loses unsaved RAM work')
    if not math.isfinite(args.connect_timeout) or not 0 < args.connect_timeout <= 300:
        parser.error('--connect-timeout must be >0 and <=300 seconds')
    if not math.isfinite(args.watchdog_timeout) or not 30 < args.watchdog_timeout <= 300:
        parser.error('--watchdog-timeout must be >30 and <=300 seconds')
    namespace = 'REC_'+secrets.token_hex(4).upper()
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    path = args.log or ROOT/'docs/bring-up'/('vios-resident-recovery-'+stamp+'.json')
    report = Report(path, namespace, ['recovery']+(['watchdog'] if args.watchdog else []))
    report.data.update(port=args.port, explicit_reset_authorization=True,
                       limits=['Unsaved RAM is discarded; clean confirmed SD startup is preserved',
                               'B/I must work during stuck VDP; H/S/header diagnostics may report a halt timeout',
                               'No flash write, ROM helper upload, CPU image deployment or SD mutation',
                               'Protocol evidence does not prove visible or audible output'])
    report.save()
    board = ResidentBoard(port=args.port, log=path.with_suffix('.serial.jsonl'), fast_console=True)
    run = Recovery(board, report, namespace)
    try:
        run.attach(args.connect_timeout); run.exercise()
        if args.watchdog: run.watchdog(args.watchdog_timeout, args.connect_timeout)
        report.data['status'] = 'passed'
        return 0
    except BaseException as error:
        report.data.update(status='failed', error=str(error), error_type=type(error).__name__)
        print('Recovery acceptance stopped: '+str(error), file=sys.stderr)
        return 130 if isinstance(error, KeyboardInterrupt) else 1
    finally:
        report.data['finished_utc'] = datetime.now(timezone.utc).isoformat()
        report.save(); board.close()
        print(json.dumps({'report': str(path), 'status': report.data['status']}), flush=True)


if __name__ == '__main__': raise SystemExit(main())

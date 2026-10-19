"""Persistent, serialized host connection to the VIOS UART/debug bridge.

Only deploy opens hardware. Importing this module and constructing a board are
side-effect free. Dependency injection allows protocol testing without a board.
"""
import collections
import contextlib
from pathlib import Path
import re
import secrets
import sys
import threading
import time
import zlib
try:
    from .vios_module_client import ModuleClient
    from .vios_workbench_client import WorkbenchClient
    from .vios_snapshot import SnapshotClient
    from .vios_devtools import DevToolsClient
    from .vios_task_client import TaskClient, ErrorClient, TraceClient, FileServiceClient
except ImportError:
    from vios_module_client import ModuleClient
    from vios_workbench_client import WorkbenchClient
    from vios_snapshot import SnapshotClient
    from vios_devtools import DevToolsClient
    from vios_task_client import TaskClient, ErrorClient, TraceClient, FileServiceClient

ROOT = Path(__file__).resolve().parents[1]


class ViosBoard(FileServiceClient,TraceClient,ErrorClient,TaskClient,DevToolsClient,SnapshotClient,WorkbenchClient,ModuleClient):
    def __init__(self, port='/dev/cu.usbserial-22430', image=None,
                 log=None, probe_image=None,
                 session=None, loader=None, deployer=None, asker=None, reader=None,
                 fast_console=False):
        self.port = port
        self.image = Path(image) if image is not None else ROOT/'build/vios.bin'
        self.log = Path(log) if log is not None else ROOT/'docs/bring-up/vios-harness.jsonl'
        self.probe_image = Path(probe_image) if probe_image is not None else ROOT/'build/zdi-loader.bin'
        self.session = session
        self.loader, self.deployer, self.asker, self.reader = loader, deployer, asker, reader
        self.lock = threading.RLock()
        self.deployed = self.paused = self.compiling = self.uncertain = self.closed = False
        self.pending_nonce = None
        self.pending_source = None
        self.pending_envelope = None
        self.source_crc = False
        self.pending_remaining = []
        self.receive_buffer = bytearray()
        self.transition_unknown = False
        self.fast_console = fast_console
        # Unsolicited console text (keyboard lines and their results, console
        # tasks) seen before a command's echo; drained by events().
        self.event_log = collections.deque(maxlen=200)

    def _deps(self):
        if None in (self.loader, self.deployer, self.asker, self.reader):
            try:
                from . import vios_hardware, zdi_backup
            except ImportError:
                import vios_hardware, zdi_backup
            self.loader = self.loader or zdi_backup.load_probe
            self.deployer = self.deployer or vios_hardware.deploy
            self.asker = self.asker or zdi_backup.ask
            self.reader = self.reader or zdi_backup.read_block

    def _session(self):
        if self.closed or self.session is None:
            raise RuntimeError('No active board session; call deploy first')
        return self.session

    def deploy(self):
        with self.lock:
            if self.closed:
                raise RuntimeError('Board connection is closed')
            if self.transition_unknown:
                raise RuntimeError('Bridge mode is unknown; restart the harness process to reload the ROM/RAM loader')
            self._deps()
            data = self.image.read_bytes()
            self.log.parent.mkdir(parents=True, exist_ok=True)
            self.deployed = False
            try:
                with contextlib.redirect_stdout(sys.stderr):
                    if self.session is None:
                        self.session = self.loader(self.port, self.probe_image, self.log)
                    elif not self.paused:
                        self._pause()
                    result = self.deployer(self.session, data)
            except Exception:
                # Deployment may already have sent X before boot framing fails.
                # An old `paused` flag cannot establish which mode is active.
                self.transition_unknown = self.uncertain = True
                raise
            self.deployed = True
            self.paused = self.compiling = self.uncertain = self.transition_unknown = False
            self.pending_nonce = self.pending_source = None
            self.pending_remaining = []
            self.receive_buffer.clear()
            return result

    @property
    def source_line_limit(self):
        return 235 if self.source_crc else 245

    def _lines(self,source):
        if not isinstance(source, str) or len(source) > 65536:
            raise ValueError('Source must be a string of at most 65536 characters')
        try:
            source.encode('ascii')
        except UnicodeEncodeError as error:
            raise ValueError('Source must be ASCII') from error
        source = source.replace('\r\n', '\n').replace('\r', '\n').replace('\t', ' ')
        lines = source.split('\n')
        if any(len(line) > self.source_line_limit or any(ord(c) < 32 or ord(c) > 126 for c in line) for line in lines):
            raise ValueError(f'Each source line must contain at most {self.source_line_limit} printable ASCII bytes')
        return lines

    @staticmethod
    def _timeout(timeout):
        if type(timeout) not in (int, float) or not 0 < timeout <= 300:
            raise ValueError('Timeout must be >0 and <=300 seconds')

    def _await(self, timeout):
        nonce = self.pending_nonce
        pattern = rb'\r\n@' + nonce.encode() + rb'\r\n(ok|error)\r\n(vios|compile)> '
        deadline = time.monotonic() + timeout
        while True:
            match = re.search(pattern, self.receive_buffer)
            if match:
                before = bytes(self.receive_buffer[:match.start()])
                echo = ((self.pending_envelope or ('@' + nonce + ' ' + self.pending_source)) + '\r\n').encode()
                # Console-attached tasks or the local keyboard can print before
                # the echo; keep that text but still replace the framed echo.
                at = before.find(echo)
                if at >= 0:
                    if before[:at].strip():
                        self._event(before[:at])
                    before = before[:at] + self.pending_source.encode() + b'\r\n' + before[at + len(echo):]
                self.compiling = match[2] == b'compile'
                result = {'output': (before + b'\r\n' + match[1] + b'\r\n' + match[2] + b'> ').decode('ascii','replace'),
                          'ok': match[1] == b'ok', 'compiling': self.compiling, 'pending': False}
                if self.pending_remaining:
                    result['remaining_source'] = '\n'.join(self.pending_remaining)
                del self.receive_buffer[:match.end()]
                self.pending_nonce = self.pending_source = None
                self.pending_envelope = None
                self.pending_remaining = []
                self.uncertain = False
                return result
            if len(self.receive_buffer) > 1048576:
                self.uncertain = True
                self.pending_nonce = None
                raise RuntimeError('Board output exceeded 1 MiB; redeploy to recover')
            if time.monotonic() >= deadline:
                self.uncertain = True
                return {'output': self.receive_buffer.decode('ascii','replace'), 'ok': False,
                        'pending': True, 'compiling': self.compiling, 'remaining_source': '\n'.join(self.pending_remaining),
                        'error': 'Evaluation still pending; use console_input for KEY, then await_result'}
            self.receive_buffer.extend(self._session().read())

    def _send_envelope(self,envelope):
        session=self._session()
        if self.source_crc:
            source=envelope[10:]
            envelope=envelope[:10]+f'!{zlib.crc32(source.encode()):08X} '+source
        self.pending_envelope=envelope
        if hasattr(session,'send_line'):
            session.send_line(envelope)
        elif self.fast_console:
            session.write((envelope+'\r').encode('ascii'))
        else:
            session.command(envelope)

    def evaluate(self, source, timeout=15):
        lines = self._lines(source)
        self._timeout(timeout)
        with self.lock:
            session = self._session()
            if not self.deployed or self.paused or self.uncertain or self.pending_nonce or self.transition_unknown:
                raise RuntimeError('Board must be deployed, running and synchronized; await pending work or redeploy')
            responses = []
            for index,line in enumerate(lines):
                self.uncertain = True
                self.pending_source = line
                self.pending_remaining = lines[index+1:]
                nonce = secrets.token_hex(4).upper()
                # The nonce is part of this line's envelope, not queued input.
                # KEY therefore receives only explicit console_input bytes.
                envelope = '@' + nonce + ' ' + line
                self._send_envelope(envelope)
                self.pending_nonce = nonce
                result = self._await(timeout)
                responses.append(result['output'])
                if not result['ok']:
                    result['output'] = ''.join(responses)
                    return result
            result['output'] = ''.join(responses)
            return result

    IDLE_TAIL = re.compile(rb'(?:\r\nok|error: [ -~]*)\r\n(vios|compile)> $')

    def resync(self, quiet=0.5, timeout=10):
        """Recover a lost envelope frame without rebooting or replaying it.

        Only when the board's newest output is an idle REPL prompt followed by
        quiet; the abandoned request's outcome is reported as unknown. A
        running program that prints a prompt-like tail can defeat this guard,
        so it never sends source, only one empty framed line."""
        self._timeout(timeout)
        with self.lock:
            session = self._session()
            if self.transition_unknown or self.paused:
                raise RuntimeError('Debug transition/pause state prevents resync; resume or restart the harness')
            wire_lost = getattr(session, 'integrity_error', False)
            if not (self.uncertain or self.pending_nonce or wire_lost):
                return {'ok': True, 'synchronized': True, 'resynced': False, 'compiling': self.compiling}
            if wire_lost:
                # Bytes were lost on USB: the abandoned output is untrusted, but
                # a fresh idle prompt proves the REPL is ready. Re-arm framing.
                session.integrity_error = False
            deadline = time.monotonic() + quiet
            while time.monotonic() < deadline:
                data = session.read()
                if data:
                    self.receive_buffer.extend(data)
                    deadline = time.monotonic() + quiet
            match = self.IDLE_TAIL.search(bytes(self.receive_buffer))
            if not match:
                raise RuntimeError('Board is not at a quiet idle prompt; use await_result/console_input or reboot')
            abandoned = {'source': self.pending_source, 'remaining_source': '\n'.join(self.pending_remaining),
                         'output': self.receive_buffer.decode('ascii', 'replace'), 'outcome': 'unknown',
                         'wire_integrity_lost': wire_lost}
            self.receive_buffer.clear()
            self.pending_nonce = self.pending_source = self.pending_envelope = None
            self.pending_remaining = []
            self.compiling = match[1] == b'compile'
            nonce = secrets.token_hex(4).upper()
            self.pending_source = ''
            self._send_envelope('@' + nonce + ' ')
            self.pending_nonce = nonce
            result = self._await(timeout)
            result.update(resynced=True, synchronized=not self.uncertain, abandoned=abandoned)
            return result

    def _event(self, raw):
        self.event_log.append({'time': round(time.time(), 3), 'text': raw.decode('ascii', 'replace')})

    def events(self, quiet=0.3):
        """Return and clear unsolicited console text. When idle, also collect
        what arrived since the last command (no source is ever sent)."""
        with self.lock:
            if not (self.pending_nonce or self.uncertain or self.paused or self.transition_unknown) and self.session is not None:
                deadline = time.monotonic() + quiet
                while time.monotonic() < deadline:
                    data = self.session.read()
                    if data:
                        self.receive_buffer.extend(data)
                if self.receive_buffer.strip():
                    self._event(bytes(self.receive_buffer))
                self.receive_buffer.clear()
            items = list(self.event_log)
            self.event_log.clear()
            return {'ok': True, 'events': items}

    def await_result(self, timeout=15):
        self._timeout(timeout)
        with self.lock:
            self._session()
            if self.transition_unknown or self.paused or self.pending_nonce is None:
                raise RuntimeError('No running pending evaluation, or bridge state is unknown')
            return self._await(timeout)

    def console_input(self, hex):
        if not isinstance(hex,str) or not re.fullmatch(r'(?:[0-9a-fA-F]{2}){1,16}',hex):
            raise ValueError('hex must encode 1..16 console bytes with no separators')
        data = bytes.fromhex(hex)
        if b'\x1d' in data:
            raise ValueError('Ctrl-] is reserved for the debug bridge')
        with self.lock:
            if self.transition_unknown or self.paused or self.pending_nonce is None:
                raise RuntimeError('Console input requires a running pending evaluation')
            try:
                self._session().write(data)
            except Exception:
                self.transition_unknown = self.uncertain = True
                raise
            return {'sent': len(data), 'pending': True}

    def _query(self, source, timeout=15):
        if self.compiling:
            raise RuntimeError('Definition in progress; finish it with evaluate or call abort')
        return self.evaluate(source, timeout)

    @staticmethod
    def _body(text, command):
        lines = text.replace('\r', '').splitlines()
        return [line for line in lines if line not in (command, 'ok', 'vios> ', 'compile> ', '')]

    def words(self):
        with self.lock:
            result = self._query('WORDS')
            result['words'] = ' '.join(self._body(result['output'], 'WORDS')).split()
            return result

    def inspect_word(self, name):
        if not isinstance(name, str) or not re.fullmatch(r'[!-~]{1,31}', name):
            raise ValueError('Word name must be one printable ASCII token, at most 31 bytes')
        with self.lock:
            result = self._query('SEE ' + name)
            match = re.search(r'(?:^|\r\n)word (\S+) ((?:\S+ \d+ ?)+)', result['output'])
            if match:
                pairs = match[2].split()
                result['word'] = {'name': match[1], **{pairs[i]: int(pairs[i+1]) for i in range(0,len(pairs)-1,2)}}
            return result

    def stacks(self):
        with self.lock:
            result = self._query('.S')
            match = re.search(r'<([^>]+)> ([^\r\n]*)', result['output'])
            if match:
                result['data_stack'] = {'depth_text': match[1], 'values_text': match[2].split()}
            return result

    def resources(self):
        with self.lock:
            result = self._query('RESOURCES')
            body = ' '.join(self._body(result['output'], 'RESOURCES')).split()
            result['resources'] = {body[i]: int(body[i+1]) if body[i+1].isdigit() else body[i+1]
                                   for i in range(0,len(body)-1,2)}
            return result

    def benchmark(self):
        with self.lock:
            result = self._query('BENCH', 120)
            result['ok'] = result['ok'] and 'BENCH DONE' in result['output'] and 'BENCH ERROR' not in result['output']
            return result

    def abort(self):
        with self.lock:
            if self.pending_nonce or self.uncertain or self.transition_unknown:
                raise RuntimeError('Pending/unknown execution: use console_input/await_result, or explicitly debug/redeploy; Ctrl-C cannot unwind KEY')
            if self.paused:
                raise RuntimeError('Board is debug-paused; resume before abort')
            # Ctrl-C is interpreted only by the idle REPL. Frame an empty line
            # after it to confirm cancellation without compiling ABORT.
            try:
                self._session().write(b'\x03')
            except Exception:
                self.uncertain = True
                raise
            return self.evaluate('')

    def _pause(self):
        if self.transition_unknown:
            raise RuntimeError('Debug transition state unknown; restart the harness process')
        if not self.paused or self.transition_unknown:
            try:
                self._session().write(b'\x1d' * 3)
                raw = self._session().until(b'\r\nDEBUG\r\n', 5)
                if self.pending_nonce:
                    before,_,after = raw.partition(b'\r\nDEBUG\r\n')
                    self.receive_buffer.extend(before+after)
                self.paused = True
                self.transition_unknown = False
            except Exception:
                self.transition_unknown = self.uncertain = True
                raise

    def resume(self):
        with self.lock:
            if self.transition_unknown:
                raise RuntimeError('Debug transition state unknown; restart the harness process')
            if self.paused:
                try:
                    self._session().write(b'Y')
                    raw = self._session().until(b'RESUME USB/UART bridge 115200\r\n', 5)
                    if self.pending_nonce:
                        before,_,after = raw.partition(b'RESUME USB/UART bridge 115200\r\n')
                        self.receive_buffer.extend(before+after)
                    self.paused = False
                except Exception:
                    self.transition_unknown = self.uncertain = True
                    raise
            return {'paused': self.paused, 'compiling': self.compiling, 'synchronized': not self.uncertain}

    @contextlib.contextmanager
    def _debug(self):
        self._deps()
        was_paused = self.paused
        self._pause()
        try:
            yield
        finally:
            if not was_paused:
                self.resume()

    def debug_registers(self):
        with self.lock, self._debug():
            raw = self.asker(self.session, b'S').decode('ascii')
            parts = raw.split()
            if len(parts) != 10 or parts[0] != 'REGS':
                raise IOError('Malformed debug register response: ' + raw)
            return {'status': int(parts[1],16), 'registers': dict(zip(
                ('af','bc','de','hl','ix','iy','sp','pc'), (int(value,16) for value in parts[2:]))), 'raw': raw}

    @staticmethod
    def _range(address, count, upper):
        if type(address) is not int or type(count) is not int or not 1 <= count <= 65536 or not 0x40000 <= address < upper or count > upper-address:
            raise ValueError('Requested range is outside permitted RAM')

    def memory_read(self, address, count=256):
        self._range(address, count, 0xC0000)
        with self.lock, self._debug():
            data = b''.join(self.reader(self.session, address+offset, min(256,count-offset), 'V')
                            for offset in range(0,count,256))
            if len(data) != count:
                raise IOError('Short RAM read')
            return {'address': address, 'count': count, 'hex': data.hex(), 'crc32': f'{zlib.crc32(data):08x}'}

    def memory_write(self, address, hex):
        if not isinstance(hex, str) or not re.fullmatch(r'(?:[0-9a-fA-F]{2}){1,256}', hex):
            raise ValueError('hex must encode 1..256 bytes with no separators')
        data = bytes.fromhex(hex)
        self._range(address, len(data), 0xAF000)
        with self.lock, self._debug():
            crc = zlib.crc32(data)
            expected = f'WROTE {address:06X} {len(data):04X} {crc:08X}'.encode()
            command = f'W{address:06X}{len(data):04X}{crc:08X}'.encode() + data.hex().upper().encode()
            if self.asker(self.session, command) != expected:
                raise IOError('RAM write acknowledgement mismatch')
            if self.reader(self.session, address,len(data),'V') != data:
                raise IOError('RAM write readback mismatch')
            return {'address': address, 'count': len(data), 'verified': True, 'crc32': f'{crc:08x}'}

    def close(self):
        with self.lock:
            if not self.closed and self.session is not None:
                self.session.close()
            self.closed = True

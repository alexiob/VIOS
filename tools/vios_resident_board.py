"""Attach the host harness to the persistent full-VDP firmware.

connect() requires an idle REPL (including its compile prompt), not a program
already consuming KEY input. It sends one empty tagged line to establish state;
there is no safe way to discover an unknown KEY continuation after reconnect.
Use explicit force recovery or deploy to replace such a session. Opening the serial driver can
reset this board electrically, so initial connection permits a 120-second boot.
No ROM helper is uploaded by this backend and construction never opens a port.
"""
import contextlib
import secrets
import sys
import threading
import time
try:
    from .vios_board import ROOT, ViosBoard
except ImportError:
    from vios_board import ROOT, ViosBoard


class ResidentBoard(ViosBoard):
    def __init__(self, *args, raw_factory=None, resident_factory=None, **kwargs):
        if len(args) < 2 and kwargs.get("image") is None:
            kwargs["image"] = ROOT/"build/vios-resident.bin"
        super().__init__(*args, **kwargs)
        self.source_crc = True
        self.raw_factory = raw_factory
        self.resident_factory = resident_factory
        self._attach_pending = False
        self._resident_identity = None
        self.host_service = None
        self._host_pump = None

    def _open_resident(self):
        if self.session is not None:
            return self.session
        if self.raw_factory is None:
            try:
                from .serial_monitor import Session
            except ImportError:
                from serial_monitor import Session
            self.raw_factory = Session
        if self.resident_factory is None:
            try:
                from .vios_resident_transport import ResidentSession
            except ImportError:
                from vios_resident_transport import ResidentSession
            self.resident_factory = ResidentSession
        self.log.parent.mkdir(parents=True, exist_ok=True)
        with contextlib.redirect_stdout(sys.stderr):
            raw = self.raw_factory(self.port, self.log)
            try:
                self.session = self.resident_factory(raw)
            except Exception:
                raw.close()
                raise
        if self.host_service is not None and hasattr(self.session, 'host_service'):
            self.session.host_service = self.host_service
        return self.session

    def enable_host_service(self, root):
        """Serve /host/ files from root; an idle pump answers board requests."""
        try:
            from .vios_host_service import HostService
        except ImportError:
            from vios_host_service import HostService
        self.host_service = HostService(root)
        if self.session is not None and hasattr(self.session, 'host_service'):
            self.session.host_service = self.host_service
        if self._host_pump is None:
            self._host_pump = threading.Thread(target=self._pump_idle, daemon=True)
            self._host_pump.start()
        return str(self.host_service.root)

    def _pump_idle(self):
        # While no command is in flight nobody reads the serial port, so host
        # requests would time out on the board. Pump briefly when idle only.
        while not self.closed:
            time.sleep(0.02)
            if not self.lock.acquire(blocking=False):
                continue
            try:
                session = self.session
                port = getattr(getattr(session, 'raw', None), 'port', None)
                waiting = getattr(port, 'in_waiting', 0) if port is not None else 0
                if (waiting and session is not None and self.deployed and not self.paused and not self.pending_nonce
                        and hasattr(session, '_pump') and not getattr(session, 'integrity_error', False)):
                    session._pump()
            except Exception:
                pass
            finally:
                self.lock.release()

    def _attach_result(self, result):
        if not result.get('pending'):
            self._attach_pending = False
            if result.get('ok'):
                self.deployed = True
        return {**result, 'connected': self.deployed, 'identity': self._resident_identity}

    def _wait_boot(self, session, deadline):
        """Read-only startup gate, before console probes or CPU mutations."""
        while True:
            remaining = deadline-time.monotonic()
            if remaining <= 0:
                raise TimeoutError('Resident firmware did not answer during boot; port retained')
            try:
                boot = session.control(b'B', timeout=min(5, remaining))
            except TimeoutError:
                continue  # Read-only boot requests can safely retry during boot.
            if boot in (b'BOOT STARTING\r\n', b'BOOT USER waiting for the MOS stub\r\n'):   # no ZDI: MOS loads the stub first
                time.sleep(max(0, min(.05, deadline-time.monotonic())))
                continue
            if boot not in (b'BOOT READY\r\n', b'BOOT HALTED\r\n'):
                raise IOError('Malformed resident boot status: '+repr(boot))
            if time.monotonic() >= deadline:
                raise TimeoutError('Resident boot status exceeded connection deadline; port retained')
            return boot

    def connect(self, timeout=120):
        self._timeout(timeout)
        with self.lock:
            if self.closed or self.transition_unknown:
                raise RuntimeError('Connection is closed or bridge mode unknown; restart the harness')
            if self.deployed:
                return {'connected': True, 'already_connected': True,
                        'compiling': self.compiling, 'pending': self.pending_nonce is not None,
                        'identity': self._resident_identity}
            session = self._open_resident()
            if self._attach_pending:
                return self._attach_result(self._await(timeout))
            if self.uncertain or self.pending_nonce:
                raise RuntimeError('Partial/unknown attach state; explicitly deploy to recover')
            deadline = time.monotonic() + timeout
            boot = self._wait_boot(session, deadline)
            identity = session.control(b'I', timeout=min(5, max(.001, deadline-time.monotonic())))
            parts = identity.split()
            untethered = parts[6:] == [b'USER'] or (
                any(b'BOOT user mode' in (e if isinstance(e, bytes) else str(e).encode()) for e in getattr(session, 'events', []))   # firmware before the USER tag
                and parts[1:2] in ([b'FFFF'], [b'0000']))
            if len(parts) not in (6, 7) or parts[0] != b'ID' or parts[2] != b'REV' or parts[4] != b'STAT' or (len(parts) == 7 and not untethered):
                raise IOError('Malformed resident identity: ' + repr(identity))
            try:
                product, revision, status = (int(parts[index],16) for index in (1,3,5))
            except ValueError as error:
                raise IOError('Malformed resident identity: ' + repr(identity)) from error
            if untethered:
                # No ZDI wire (MOS and the boot stub loaded VIOS): the CPU cannot be
                # inspected, only its boot state, which must be READY.
                if boot != b'BOOT READY\r\n':
                    raise RuntimeError('Untethered resident is not running yet: ' + boot.decode('ascii', 'replace').strip())
                self._resident_identity = 'untethered (no ZDI wire)'
                self.paused = session.paused = False
            else:
                if product != 7:
                    raise IOError('Resident CPU identity mismatch: ' + repr(identity))
                self._resident_identity = identity.decode('ascii').strip()
                self.paused = session.paused = bool(status & 0x80)
                if self.paused or boot == b'BOOT HALTED\r\n':
                    raise RuntimeError('Resident CPU is halted; use explicit deploy or debug recovery')
                if not status & 0x10:
                    raise RuntimeError('Resident CPU is not in ADL mode; explicitly deploy to recover')
            nonce = secrets.token_hex(4).upper()
            self.pending_source = ''
            self.pending_remaining = []
            self.uncertain = True
            envelope = '@' + nonce + ' '
            self._send_envelope(envelope)
            self.pending_nonce = nonce
            self._attach_pending = True
            return self._attach_result(self._await(max(.001,deadline-time.monotonic())))

    def await_result(self, timeout=15):
        with self.lock:
            result = super().await_result(timeout)
            return self._attach_result(result) if self._attach_pending else result

    def reboot(self, force=False, timeout=120):
        """Restart and attach on the same raw port; force explicitly loses RAM work.

        The framed supervisor Z command is permitted despite stale console/debug
        state. Its CRC/sequence-checked acknowledgment is still mandatory; a lost
        acknowledgment is never retried automatically.
        """
        if type(force) is not bool:
            raise ValueError('force must be a boolean')
        self._timeout(timeout)
        with self.lock:
            if self.closed:
                raise RuntimeError('Board connection is closed')
            if not force and (not self.deployed or self.session is None or self.paused
                    or self.compiling or self.uncertain or self.transition_unknown
                    or self.pending_nonce is not None or self._attach_pending
                    or getattr(self.session,'paused',False)
                    or getattr(self.session,'integrity_error',False)):
                raise RuntimeError('Reboot requires a connected, idle, running and synchronized resident session')
            self.deployed = False
            try:
                session = self._open_resident() if force else self.session
                if session.control(b'Z') != b'RESTARTING\r\n':
                    raise IOError('Resident firmware did not acknowledge reboot')
                factory = self.resident_factory
                if factory is None:
                    try:
                        from .vios_resident_transport import ResidentSession
                    except ImportError:
                        from vios_resident_transport import ResidentSession
                    factory = ResidentSession
                # Closing/reopening raw can toggle board reset pins. Drop only the
                # framed wrapper, including old decoder, replies and console bytes.
                self.session = factory(session.raw)
            except BaseException:
                self.transition_unknown = self.uncertain = True
                raise
            self.paused = self.compiling = self.uncertain = self.transition_unknown = False
            self.pending_nonce = self.pending_source = self.pending_envelope = None
            self.pending_remaining = []
            self.receive_buffer.clear()
            self.source_crc = True
            self._attach_pending = False
            self._resident_identity = None
            return {'rebooted': True, 'forced': force, **self.connect(timeout)}

    def deploy(self, timeout=120):
        self._timeout(timeout)
        with self.lock:
            if self.closed or self.transition_unknown:
                raise RuntimeError('Connection is closed or bridge mode unknown; restart the harness')
            self._deps()
            data = self.image.read_bytes()
            try:
                from .serial_monitor import validate_image
            except ImportError:
                from serial_monitor import validate_image
            validate_image(data)  # Validate before opening or changing target state.
            session = self._open_resident()
            self.deployed = False
            # Port open may reset ESP into the restricted B/Z supervisor. Do not
            # classify a read-only startup timeout as an incomplete RAM upload.
            self._wait_boot(session, time.monotonic()+timeout)
            try:
                with contextlib.redirect_stdout(sys.stderr):
                    # ResidentSession must route the legacy deployer's first '2'
                    # through channel 2 rather than delivering it as Forth input.
                    if not self.paused or not getattr(session,'paused',False):
                        self.paused = False
                        self._pause()
                    result = self.deployer(session,data)
            except BaseException:
                self.transition_unknown = self.uncertain = True
                raise
            self.deployed = True
            self.paused = self.compiling = self.uncertain = self.transition_unknown = False
            self.pending_nonce = self.pending_source = None
            self.pending_remaining = []
            self.receive_buffer.clear()
            self._attach_pending = False
            return result

    # GUI Stage 0 instrumentation on the ESP (control channel; the CPU keeps running).
    def _esp(self, command):
        with self.lock:
            if not self.deployed:
                raise RuntimeError('No active board session; call connect first')
            return self.session.control(command, timeout=5).decode('ascii').strip()

    def esp_stats(self, reset=False):
        """ESP counters: glyphs drawn, USB console bytes/writes and blocking, parser time (us)."""
        line = self._esp(b'T')
        stats = {key: int(value) for key, value in (item.split('=') for item in line.split()[1:])}
        if reset:
            self._esp(b'T0')
        return stats

    def esp_probe(self, char=None):
        """Arm the key-to-glyph probe for one character, or report its samples (us)."""
        if char is not None:
            if len(char) != 1 or not 32 <= ord(char) < 127:
                raise ValueError('probe character must be one printable ASCII character')
            return {'armed': char, 'reply': self._esp(b'PA%02X' % ord(char))}
        line = self._esp(b'PR')
        return {key: int(value) for key, value in (item.split('=') for item in line.split()[1:])}

    def esp_inject(self, text):
        """Type up to 32 ASCII characters at the PS/2 boundary, as if on the Agon keyboard."""
        data = text.encode('ascii')
        if not 1 <= len(data) <= 32 or any(b not in (3, 8, 9, 13, 27) and not 32 <= b < 127 for b in data):
            raise ValueError('inject 1..32 printable characters (or Ctrl-C, BS, TAB, CR, ESC)')
        return {'reply': self._esp(b'KI' + data.hex().upper().encode())}

    def esp_console(self, rows=59):
        """Exact text of the ESP-rendered console rows 0..rows-1 (row 58 is the status bar)."""
        if type(rows) is not int or not 1 <= rows <= 59:
            raise ValueError('rows must be 1..59')
        lines = []
        for y in range(rows):
            line = self._esp(b'CR%02X' % y)
            if not line.startswith('CROW %02X' % y):
                raise IOError('Malformed console row reply: ' + line)
            lines.append(line[8:])
        return {'rows': lines}

    def esp_gui(self, vk=None, ascii=0, mods=0, pixels=()):
        """GUI display server on the ESP: its status line; with vk, press and release
        one key through the window manager (used or not; never forwarded to the CPU); with
        pixels, the framebuffer colour index at each [x, y]."""
        result = {'status': self._esp(b'GS')}
        if vk is not None:
            if not all(type(n) is int and 0 <= n <= 255 for n in (vk, ascii, mods)):
                raise ValueError('vk, ascii and mods must be 0..255')
            used = [self._esp(b'GK%02X%02X%02X%02X' % (ascii, mods, vk, down)) for down in (1, 0)]
            result['used'] = used[0] == 'GK used'
            result['status'] = self._esp(b'GS')
        if pixels:
            if len(pixels) > 64 or not all(len(p) == 2 and 0 <= p[0] < 640 and 0 <= p[1] < 480 for p in pixels):
                raise ValueError('up to 64 [x, y] pairs inside 640x480')
            result['pixels'] = [int(self._esp(b'GP%03X%03X' % (x, y)).split()[1]) for x, y in pixels]
        return result

    def esp_audio(self):
        """Lean audio engine state on the ESP (control command AS), one status line."""
        return {'status': self._esp(b'AS')}

    def esp_g0(self, mode=None, reset_gap=False):
        """GUI gate G0 probe on the ESP: run it for VDP mode `mode` and return its lines,
        or with reset_gap restart the VDP loop-gap maximum."""
        if reset_gap:
            return {'reply': self._esp(b'G0Z')}
        if mode not in (0, 1, 129):
            raise ValueError('mode must be 0, 1 or 129')
        started = self._esp(b'G0%02X' % mode)
        if started != 'G0 STARTED':
            raise IOError(started)
        deadline = time.monotonic() + 60
        while self._esp(b'G0R0') == 'G0 BUSY':
            if time.monotonic() > deadline:
                raise TimeoutError('G0 probe did not finish')
            time.sleep(0.5)
        lines = [self._esp(b'G0R%d' % n)[4:] for n in range(6)]
        return {'mode': mode, 'lines': [line for line in lines if line]}

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
        return self.session

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
            if boot == b'BOOT STARTING\r\n':
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
            if len(parts) != 6 or parts[0] != b'ID' or parts[2] != b'REV' or parts[4] != b'STAT':
                raise IOError('Malformed resident identity: ' + repr(identity))
            try:
                product, revision, status = (int(parts[index],16) for index in (1,3,5))
            except ValueError as error:
                raise IOError('Malformed resident identity: ' + repr(identity)) from error
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

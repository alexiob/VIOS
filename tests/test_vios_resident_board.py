"""Resident attach/deploy lifecycle; never opens real serial hardware."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
try:
    from vios_resident_board import ResidentBoard
except ImportError:
    ResidentBoard=None

class Raw:
    def __init__(self):self.closed=0
    def close(self):self.closed+=1
class Session:
    def __init__(self,raw):
        self.raw=raw;self.paused=False;self.pending=b'';self.commands=[];self.controls=[];self.writes=[]
        self.events=[];self.status=b'18';self.identity=b'0007';self.compile=False;self.user=False
    def control(self,command,timeout=20):
        self.controls.append(command)
        if command==b'B':return b'BOOT HALTED\r\n' if self.status==b'98' else b'BOOT READY\r\n'
        if self.user:return b'ID FFFF REV FF STAT 00 USER\r\n'
        return b'ID '+self.identity+b' REV AA STAT '+self.status+b'\r\n'
    def command(self,envelope):
        self.commands.append(envelope)
        nonce=envelope[1:9].encode()
        prompt=b'compile> ' if self.compile else b'vios> '
        self.pending=envelope.encode()+b'\r\n\r\n@'+nonce+b'\r\nok\r\n'+prompt
    def write(self,data):
        self.writes.append(data)
        if data==b'\x1d'*3:self.paused=True;self.pending=b'\r\nDEBUG\r\n'
        elif data==b'2':assert self.paused, 'delay selector must use debug channel'
        elif data==b'X':self.paused=False
        else:raise AssertionError(data)
    def read(self):
        data,self.pending=self.pending,b'';return data
    def until(self,marker,timeout=10):
        assert marker in self.pending
        data,self.pending=self.pending,b'';return data
    def close(self):self.raw.close()

class ResidentBoardTests(unittest.TestCase):
    def setUp(self):self.assertIsNotNone(ResidentBoard,'ResidentBoard missing')
    def test_lazy_connect_is_idempotent_and_preserves_compile_state(self):
        raw=Raw();opened=[];wrapped=[]
        def raw_factory(*args):opened.append(args);return raw
        def resident_factory(value):
            session=Session(value);session.compile=True;wrapped.append(session);return session
        with tempfile.TemporaryDirectory() as tmp:
            board=ResidentBoard(log=Path(tmp)/'serial.jsonl',raw_factory=raw_factory,resident_factory=resident_factory)
            self.assertEqual(opened,[])
            self.assertEqual(board.image.name,'vios-resident.bin')
            self.assertTrue(board.source_crc)
            result=board.connect(.1)
            self.assertTrue(result['connected']);self.assertTrue(board.deployed);self.assertTrue(board.compiling)
            self.assertEqual(len(opened),1);self.assertEqual(wrapped[0].controls,[b'B',b'I'])
            self.assertEqual(len(wrapped[0].commands),1)
            board.pending_nonce='KEYWAIT';before=list(wrapped[0].commands)
            self.assertTrue(board.connect()['already_connected'])
            self.assertEqual(wrapped[0].commands,before)
            board.close();board.close();self.assertEqual(raw.closed,1)
    def test_identity_and_halted_state_do_not_deploy_or_resume_implicitly(self):
        session=Session(Raw());session.identity=b'FFFF'
        board=ResidentBoard(session=session)
        with self.assertRaises(IOError):board.connect(.01)
        self.assertFalse(board.deployed);self.assertEqual(session.commands,[]);self.assertEqual(session.writes,[])
        session=Session(Raw());session.status=b'98';board=ResidentBoard(session=session)
        with self.assertRaises(RuntimeError):board.connect(.01)
        self.assertTrue(board.paused);self.assertTrue(session.paused)
        self.assertFalse(board.deployed);self.assertEqual(session.commands,[]);self.assertEqual(session.writes,[])
    def test_untethered_boards_connect_without_zdi(self):
        for tagged in (True, False):              # the USER tag, or the boot event of older firmware
            session=Session(Raw())
            if tagged:session.user=True
            else:session.identity=b'FFFF';session.events=[b'BOOT user mode: no ZDI; MOS and its stub load the resident\r\n']
            board=ResidentBoard(session=session)
            result=board.connect(.1)
            self.assertTrue(result['connected']);self.assertEqual(result['identity'],'untethered (no ZDI wire)')
            self.assertEqual(len(session.commands),1)
        session=Session(Raw());session.identity=b'FFFF'          # floating ZDI without the user-mode signal: still refused
        with self.assertRaises(IOError):ResidentBoard(session=session).connect(.01)
    def test_explicit_deploy_uses_resident_halt_before_delay_and_never_ram_loader(self):
        raw=Raw();session=Session(raw);loads=[];deploys=[]
        def forbidden_loader(*args):loads.append(args);raise AssertionError('ROM loader must not be used')
        def deployer(s,data):
            self.assertTrue(s.paused);s.write(b'2');s.write(b'X');deploys.append(data);print('deployment progress')
            return {'boot':'vios> '}
        with tempfile.TemporaryDirectory() as tmp:
            image=Path(tmp)/'vios.bin';data=bytearray(100);data[64:69]=b'MOS\0\1';image.write_bytes(data)
            board=ResidentBoard(session=session,image=image,log=Path(tmp)/'log',loader=forbidden_loader,deployer=deployer)
            with contextlib.redirect_stdout(io.StringIO()) as stdout:board.deploy()
            self.assertEqual(stdout.getvalue(),'')
            self.assertEqual(loads,[]);self.assertEqual(len(deploys),1)
            self.assertEqual(session.writes,[b'\x1d'*3,b'2',b'X']);self.assertTrue(board.deployed);self.assertFalse(board.paused)
            board.deploy();self.assertEqual(len(deploys),2);self.assertEqual(loads,[])
    def test_connection_timeout_keeps_one_pending_probe_without_queuing_more(self):
        class Waiting(Session):
            def command(self,line):self.commands.append(line)
        session=Waiting(Raw());board=ResidentBoard(session=session)
        result=board.connect(.001)
        self.assertFalse(result['ok']);self.assertTrue(result['pending']);self.assertFalse(board.deployed)
        board.connect(.001)
        self.assertEqual(len(session.commands),1)
        nonce=board.pending_nonce.encode();session.pending=b'\r\n@'+nonce+b'\r\nok\r\nvios> '
        self.assertTrue(board.await_result(.01)['ok']);self.assertTrue(board.deployed)

    def test_starting_status_is_polled_before_any_tagged_console_probe(self):
        class Booting(Session):
            def __init__(self,raw):super().__init__(raw);self.polls=0
            def control(self,command,timeout=20):
                if command==b'B':
                    self.polls+=1
                    self.assert_no_input()
                    if self.polls<3:return b'BOOT STARTING\r\n'
                return super().control(command,timeout)
            def assert_no_input(self):assert self.commands==[]
        session=Booting(Raw());board=ResidentBoard(session=session)
        self.assertTrue(board.connect(.3)['connected'])
        self.assertEqual(session.polls,3);self.assertEqual(len(session.commands),1)

    def test_starting_timeout_sends_no_source_and_can_retry_on_same_port(self):
        class Starting(Session):
            def control(self,command,timeout=20):
                if command==b'B' and not self.events:return b'BOOT STARTING\r\n'
                return super().control(command,timeout)
        session=Starting(Raw());board=ResidentBoard(session=session)
        with self.assertRaises(TimeoutError):board.connect(.001)
        self.assertFalse(board.deployed);self.assertEqual(session.commands,[])
        self.assertFalse(board.transition_unknown)
        session.events.append('ready')
        self.assertTrue(board.connect(.01)['connected'])

class RebootPeer:
    """Byte-level serial peer exercising the real resident wrapper on restart."""
    def __init__(self,ack=b'RESTARTING\r\n'):
        from vios_wire import Decoder
        self.decoder=Decoder();self.pending=bytearray();self.line=bytearray()
        self.controls=[];self.executed=[];self.closed=0;self.ack=ack;self.boot_polls=0
    def write(self,data):
        import struct
        import zlib
        from vios_wire import Packet,encode
        for packet in self.decoder.feed(data):
            if packet.channel==2:
                command=packet.payload.rstrip(b'\n');self.controls.append(command)
                if command==b'Z':reply=self.ack
                elif command==b'B':
                    self.boot_polls+=1
                    reply=b'BOOT STARTING\r\n' if self.boot_polls<3 else b'BOOT READY\r\n'
                elif command==b'I':reply=b'ID 0007 REV AA STAT 18\r\n'
                else:raise AssertionError(command)
                if reply is not None:self.pending+=encode(Packet(2,packet.sequence,reply))
            elif packet.channel==1:
                self.line+=packet.payload
                size,crc=struct.unpack('<HI',self.line[:6])
                if len(self.line)<size+6:continue
                body=bytes(self.line[6:]);assert len(body)==size and zlib.crc32(body)==crc
                self.executed.append(body);self.line.clear()
                self.pending+=encode(Packet(1,packet.sequence,b'LINE OK\r\n'))
                nonce=body[1:9]
                self.pending+=encode(Packet(1,0,body+b'\n\r\n@'+nonce+b'\r\nok\r\nvios> '))
    def read(self):
        data=bytes(self.pending[:17]);del self.pending[:17];return data
    def close(self):self.closed+=1

class RebootTests(unittest.TestCase):
    def board(self,raw=None):
        from vios_resident_transport import ResidentSession
        raw=raw or RebootPeer();session=ResidentSession(raw)
        board=ResidentBoard(session=session,resident_factory=ResidentSession)
        board.deployed=True
        return board,raw
    def test_reboot_reuses_raw_serial_and_discards_stale_session_state(self):
        board,raw=self.board();old=board.session
        old.pending+=b'stale console';old.received+=b'stale history';old.sequence=99
        board.receive_buffer+=b'stale response';board._resident_identity='old';board.source_crc=True
        self.assertTrue(hasattr(board,'reboot'),'Resident reboot support missing')
        result=board.reboot()
        self.assertTrue(result['rebooted']);self.assertTrue(result['connected'])
        self.assertIsNot(board.session,old);self.assertIs(board.session.raw,raw);self.assertEqual(raw.closed,0)
        self.assertEqual(raw.controls,[b'Z',b'B',b'B',b'B',b'I'])
        self.assertEqual(len(raw.executed),1);self.assertNotIn(b'stale',board.receive_buffer)
        self.assertFalse(board.compiling or board.uncertain or board.transition_unknown or board.paused)
        self.assertIsNone(board.pending_nonce);self.assertFalse(board._attach_pending)
        self.assertNotEqual(board._resident_identity,'old');self.assertTrue(board.source_crc)
        self.assertTrue(board.evaluate('1 2 +')['ok']);self.assertEqual(len(raw.executed),2)
        board.close();self.assertEqual(raw.closed,1)
    def test_reboot_rejects_non_idle_state_before_any_control_write(self):
        for field,value in [('closed',True),('deployed',False),('paused',True),('compiling',True),
                            ('uncertain',True),('transition_unknown',True),('pending_nonce','12345678'),('_attach_pending',True)]:
            with self.subTest(field=field):
                board,raw=self.board();setattr(board,field,value)
                self.assertTrue(hasattr(board,'reboot'),'Resident reboot support missing')
                with self.assertRaises(RuntimeError):board.reboot()
                self.assertEqual(raw.controls,[]);self.assertEqual(raw.closed,0)
    def test_bad_restart_ack_fails_closed_without_reconnecting(self):
        board,raw=self.board(RebootPeer(b'ERROR UNKNOWN\r\n'))
        self.assertTrue(hasattr(board,'reboot'),'Resident reboot support missing')
        with self.assertRaises(IOError):board.reboot()
        self.assertEqual(raw.controls,[b'Z']);self.assertEqual(raw.closed,0)
        self.assertFalse(board.deployed);self.assertTrue(board.uncertain);self.assertTrue(board.transition_unknown)
        with self.assertRaises(RuntimeError):board.evaluate('MUST-NOT-RUN')

    def test_initial_probe_uses_atomic_send_and_cpu_source_crc(self):
        class Atomic(Session):
            def send_line(self,line):
                self.atomic=line
                self.command(line)
        session=Atomic(Raw());board=ResidentBoard(session=session)
        self.assertTrue(board.connect(.1)['connected'])
        self.assertRegex(session.atomic,r'^@[0-9A-F]{8} !00000000 $')

    def test_reboot_rejects_wrapper_pause_or_integrity_loss_before_write(self):
        for field in ('paused','integrity_error'):
            with self.subTest(field=field):
                board,raw=self.board();setattr(board.session,field,True)
                with self.assertRaises(RuntimeError):board.reboot()
                self.assertEqual(raw.controls,[])

    def test_restart_write_failure_blocks_further_source(self):
        class FailedPeer(RebootPeer):
            def write(self,data):raise IOError('serial write failed')
        board,raw=self.board(FailedPeer())
        with self.assertRaises(IOError):board.reboot()
        self.assertFalse(board.deployed);self.assertTrue(board.uncertain);self.assertTrue(board.transition_unknown)
        self.assertEqual(raw.closed,0)
        with self.assertRaises(RuntimeError):board.evaluate('MUST-NOT-RUN')

    def test_boot_timeout_retains_new_wrapper_for_explicit_connect_retry(self):
        board,raw=self.board();old=board.session
        with self.assertRaises(TimeoutError):board.reboot(timeout=.001)
        self.assertIsNot(board.session,old);self.assertIs(board.session.raw,raw)
        self.assertFalse(board.deployed);self.assertFalse(board.transition_unknown);self.assertEqual(raw.closed,0)
        self.assertTrue(board.connect(.3)['connected']);self.assertEqual(raw.controls.count(b'Z'),1)

    def test_explicit_force_recovers_pending_key_and_blocked_gpu_rpc_with_real_empty_attach(self):
        for source in ('KEY .', '0 0 PIXEL@ .'):
            with self.subTest(source=source):
                board,raw=self.board();old=board.session;raw.boot_polls=2
                board.pending_nonce='DEADBEEF';board.pending_source=source
                board.pending_remaining=['MUST-NOT-RUN'];board.pending_envelope='stale'
                board.receive_buffer+=b'partial output';board.uncertain=True
                result=board.reboot(force=True)
                self.assertTrue(result['connected']);self.assertTrue(result['rebooted'])
                self.assertEqual(raw.controls,[b'Z',b'B',b'I'])
                self.assertEqual(len(raw.executed),1)
                self.assertRegex(raw.executed[0],br'^@[0-9A-F]{8} !00000000 \r$')
                self.assertNotIn(source.encode(),raw.executed[0])
                self.assertIs(board.session.raw,raw);self.assertIsNot(board.session,old)
                self.assertEqual(raw.closed,0);self.assertFalse(board.pending_remaining)
                self.assertIsNone(board.pending_nonce);self.assertFalse(board.receive_buffer)
                self.assertTrue(board.evaluate('1 2 +')['ok'])

    def test_explicit_force_abandons_compile_pause_attach_unknown_and_integrity_state(self):
        for field,value in [('compiling',True),('paused',True),('_attach_pending',True),
                            ('transition_unknown',True),('deployed',False)]:
            with self.subTest(field=field):
                board,raw=self.board();raw.boot_polls=2;setattr(board,field,value)
                board.session.integrity_error=True;board.session.paused=True
                board.source_crc=False;board._resident_identity='obsolete'
                result=board.reboot(force=True)
                self.assertTrue(result['connected']);self.assertTrue(board.source_crc)
                self.assertFalse(board.uncertain or board.transition_unknown or board.compiling or board.paused)
                self.assertFalse(board._attach_pending or board.session.integrity_error or board.session.paused)
                self.assertEqual(raw.controls,[b'Z',b'B',b'I']);self.assertEqual(raw.closed,0)

    def test_explicit_force_can_open_resident_transport_without_deploy_or_ram_helper(self):
        from vios_resident_transport import ResidentSession
        raw=RebootPeer();raw.boot_polls=2;opened=[]
        def open_raw(*args):opened.append(args);return raw
        def forbidden(*args):raise AssertionError('Force recovery must not deploy or load a ROM helper')
        with tempfile.TemporaryDirectory() as tmp:
            board=ResidentBoard(log=Path(tmp)/'serial.jsonl',raw_factory=open_raw,
                                resident_factory=ResidentSession,loader=forbidden,deployer=forbidden)
            result=board.reboot(force=True)
        self.assertTrue(result['connected']);self.assertEqual(len(opened),1)
        self.assertEqual(raw.controls,[b'Z',b'B',b'I']);self.assertEqual(raw.closed,0)

    def test_force_requires_boolean_and_never_revives_closed_board(self):
        for force in (None,1,0,'true'):
            with self.subTest(force=force):
                board,raw=self.board()
                with self.assertRaises(ValueError):board.reboot(force=force)
                self.assertEqual(raw.controls,[])
        board,raw=self.board();board.closed=True
        with self.assertRaises(RuntimeError):board.reboot(force=True)
        self.assertEqual(raw.controls,[])

    def test_force_lost_or_corrupt_restart_ack_fails_closed_without_retry_or_attach(self):
        class CorruptPeer(RebootPeer):
            def write(self,data):
                super().write(data)
                if self.controls[-1:]==[b'Z'] and self.pending:self.pending[-3]^=1
        for peer in (RebootPeer(None),CorruptPeer()):
            with self.subTest(peer=type(peer).__name__):
                board,raw=self.board(peer);old=board.session
                board.pending_nonce='DEADBEEF';board.uncertain=board.transition_unknown=True
                old.integrity_error=True
                original_control=old.control
                old.control=lambda command,timeout=20:original_control(command,timeout=.005)
                with self.assertRaises(TimeoutError):board.reboot(force=True)
                self.assertEqual(raw.controls,[b'Z']);self.assertEqual(raw.executed,[])
                self.assertIs(board.session,old);self.assertEqual(raw.closed,0)
                self.assertFalse(board.deployed);self.assertTrue(board.uncertain and board.transition_unknown)
                with self.assertRaises(RuntimeError):board.evaluate('MUST-NOT-RUN')

    def test_interrupted_force_request_marks_outcome_unknown_before_propagating_interrupt(self):
        class InterruptedPeer(RebootPeer):
            def write(self,data):raise KeyboardInterrupt()
        board,raw=self.board(InterruptedPeer())
        with self.assertRaises(KeyboardInterrupt):board.reboot(force=True)
        self.assertFalse(board.deployed)
        self.assertTrue(board.transition_unknown and board.uncertain)
        with self.assertRaises(RuntimeError):board.connect(.01)


class DeployBootPeer:
    """Real framed supervisor; CPU mutations are forbidden while boot owns it."""
    def __init__(self, statuses):
        from vios_wire import Decoder
        self.decoder=Decoder();self.statuses=list(statuses);self.controls=[]
        self.pending=bytearray();self.console=[];self.closed=0
    def write(self,data):
        from vios_wire import Packet,encode
        for packet in self.decoder.feed(data):
            assert packet.channel==2, 'Deploy must not send a Forth attach probe'
            command=packet.payload.rstrip(b'\n');self.controls.append(command)
            if command==b'B':
                reply=self.statuses[0]
                if len(self.statuses)>1:self.statuses.pop(0)
            else:
                assert self.statuses[0] in (b'BOOT READY\r\n',b'BOOT HALTED\r\n'), 'CPU touched during restricted bootstrap'
                if command==b'H':reply=b'REGS 98 000000 000000 000000 000000 000000 000000 0AEFFF 040000\r\n'
                elif command==b'2':reply=b'ACK 2\r\n'
                elif command==b'X':reply=b'GO 040000 USB/UART bridge 115200\r\n'
                else:raise AssertionError(command)
            self.pending.extend(encode(Packet(2,packet.sequence,reply)))
    def read(self):
        data=bytes(self.pending[:17]);del self.pending[:17];return data
    def close(self):self.closed+=1


class DeployBootTests(unittest.TestCase):
    @contextlib.contextmanager
    def board(self,statuses,deployer=None):
        from vios_resident_transport import ResidentSession
        raw=DeployBootPeer(statuses);uploads=[]
        def upload(session,data):
            uploads.append(data);session.write(b'2');session.write(b'X')
            session.until(b'GO 040000 USB/UART bridge 115200\r\n',.1)
            return {'boot':'vios> '}
        def forbidden(*args):raise AssertionError('Resident deployment must never load a temporary bridge')
        with tempfile.TemporaryDirectory() as directory:
            image=Path(directory)/'vios.bin';data=bytearray(100);data[64:69]=b'MOS\0\1';image.write_bytes(data)
            board=ResidentBoard(session=ResidentSession(raw),image=image,loader=forbidden,deployer=deployer or upload)
            yield board,raw,uploads

    def test_fresh_deploy_polls_boot_starting_until_halted_before_pause_and_upload(self):
        with self.board([b'BOOT STARTING\r\n',b'BOOT STARTING\r\n',b'BOOT HALTED\r\n']) as (board,raw,uploads):
            board.deploy(timeout=.3)
            self.assertEqual(raw.controls,[b'B',b'B',b'B',b'H',b'2',b'X'])
            self.assertEqual(len(uploads),1);self.assertTrue(board.deployed)
            self.assertFalse(board.transition_unknown or board.uncertain);self.assertEqual(raw.closed,0)

    def test_boot_timeout_is_read_only_and_explicit_retry_uses_same_raw_connection(self):
        with self.board([b'BOOT STARTING\r\n']) as (board,raw,uploads):
            with self.assertRaises(TimeoutError):board.deploy(timeout=.001)
            self.assertTrue(raw.controls);self.assertEqual(set(raw.controls),{b'B'})
            self.assertFalse(uploads);self.assertFalse(board.transition_unknown or board.uncertain)
            self.assertFalse(board.deployed);self.assertEqual(raw.closed,0)
            raw.statuses[:]=[b'BOOT READY\r\n'];board.deploy(timeout=.1)
            self.assertEqual(len(uploads),1);self.assertTrue(board.deployed)

    def test_malformed_boot_response_fails_before_halt_or_upload_without_unknown_state(self):
        with self.board([b'BOOT MAYBE\r\n']) as (board,raw,uploads):
            with self.assertRaises(IOError):board.deploy(timeout=.1)
            self.assertEqual(raw.controls,[b'B']);self.assertFalse(uploads)
            self.assertFalse(board.transition_unknown or board.uncertain);self.assertEqual(raw.closed,0)

    def test_interrupted_mutating_phase_is_unknown_unlike_read_only_boot_wait(self):
        def interrupted(*args):raise KeyboardInterrupt()
        with self.board([b'BOOT HALTED\r\n'],interrupted) as (board,raw,uploads):
            with self.assertRaises(KeyboardInterrupt):board.deploy(timeout=.1)
            self.assertEqual(raw.controls,[b'B',b'H'])
            self.assertTrue(board.transition_unknown and board.uncertain)

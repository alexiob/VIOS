"""Host-only protocol/session checks. Never opens a serial port."""
import contextlib
import io
import json
from pathlib import Path
import socket
import sys
import tempfile
import threading
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
try:
    from vios_board import ViosBoard
    from vios_harness import RpcServer, make_tcp_server, serve_stream
except ImportError:
    ViosBoard = None


class Session:
    def __init__(self):
        self.pending = b''
        self.commands = []
        self.writes = []
        self.closed = 0
        self.compiling = False
    def command(self, envelope):
        self.commands.append(envelope)
        assert not self.pending, 'source input queued while busy'
        assert envelope.startswith('@') and envelope[9] == ' '
        nonce, line = envelope[1:9], envelope[10:]
        if line.startswith('!'):
            assert line[9]==' ' and int(line[1:9],16)==zlib.crc32(line[10:].encode())
            line=line[10:]
        if line.startswith(':') and ';' not in line:
            self.compiling = True
        if ';' in line or line == 'ABORT':
            self.compiling = False
        prompt = b'compile> ' if self.compiling else b'vios> '
        bad = line == 'BAD'
        body = b'error: unknown word: BAD\r\n' if bad else b''
        self.pending = envelope.encode()+b'\r\n'+body+b'\r\n@'+nonce.encode()+b'\r\n'+(b'error' if bad else b'ok')+b'\r\n'+prompt
    def read(self):
        data, self.pending = self.pending[:3], self.pending[3:]
        return data
    def write(self, data):
        self.writes.append(data)
        if data == b'\x03':
            self.compiling = False
        elif data == b'\x1d' * 3:
            self.pending = b'\r\nDEBUG\r\n'
        elif data == b'Y':
            self.pending = b'RESUME USB/UART bridge 115200\r\n'
    def until(self, marker, timeout=10):
        assert marker in self.pending, (marker, self.pending)
        data, self.pending = self.pending, b''
        return data
    def close(self):
        self.closed += 1


class FakeBoard:
    def __init__(self):
        self.calls = []
    def evaluate(self, source, timeout=15):
        self.calls.append(source)
        if source == 'fail':
            raise RuntimeError('board error')
        return {'output': source, 'ok': True, 'compiling': False}


class HarnessTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(ViosBoard, 'VIOS host harness is not implemented')
    def rpc(self, server, method, params=None, ident=1):
        return server.handle({'jsonrpc': '2.0', 'id': ident, 'method': method, 'params': params or {}})
    def initialize(self, server):
        result = self.rpc(server, 'initialize', {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'test', 'version': '1'}})
        self.assertEqual(result['result']['protocolVersion'], '2025-11-25')
        self.assertIsNone(server.handle({'jsonrpc': '2.0', 'method': 'notifications/initialized'}))
    def test_session_persists_and_multiline_waits_before_next_line(self):
        session = Session()
        loads = []
        def loader(*args):
            loads.append(args)
            print('esptool progress')
            return session
        def deployer(s, data):
            print('deploy progress')
            return {'boot': 'vios> '}
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / 'vios.bin'; image.write_bytes(b'fixture')
            board = ViosBoard(image=image, log=Path(tmp)/'serial.jsonl', loader=loader, deployer=deployer)
            with contextlib.redirect_stdout(io.StringIO()) as output:
                board.deploy()
                self.assertTrue(board.evaluate(': SUM\n1 2 +')['compiling'])
                self.assertFalse(board.evaluate(';\nSUM .')['compiling'])
                board.deploy()  # Explicit fresh entry, same already-open port.
            self.assertEqual(output.getvalue(), '')
            self.assertEqual(len(loads), 1)
            self.assertEqual(len(session.commands), 4)
            self.assertEqual([c[10:] for c in session.commands], [': SUM', '1 2 +', ';', 'SUM .'])
            board.close(); board.close()
            self.assertEqual(session.closed, 1)
    def test_rejects_bad_source_before_any_input_and_reports_language_error(self):
        session = Session(); board = ViosBoard(session=session); board.deployed = True
        for source in ['A'*246, 'okay\n'+'A'*246, '\x1d', 'héllo']:
            with self.assertRaises(ValueError): board.evaluate(source)
        self.assertEqual(session.commands, [])
        self.assertFalse(board.evaluate('BAD\nSHOULD-NOT-RUN')['ok'])
        self.assertEqual([c[10:] for c in session.commands], ['BAD'])
    def test_debug_memory_preserves_pause_state_and_write_verifies(self):
        session = Session(); memory = bytearray(512)
        def ask(s, command):
            if command == b'S': return b'REGS 98 000001 000002 000003 000004 000005 000006 0AFFF0 040123'
            head, data = command[:19], bytes.fromhex(command[19:].decode())
            addr, count, crc = int(head[1:7],16), int(head[7:11],16), int(head[11:19],16)
            memory[addr-0x40000:addr-0x40000+count] = data
            return f'WROTE {addr:06X} {count:04X} {crc:08X}'.encode()
        def read(s, address, count, operation):
            self.assertEqual(operation, 'V')
            return bytes(memory[address-0x40000:address-0x40000+count])
        board = ViosBoard(session=session, asker=ask, reader=read); board.deployed = True
        self.assertEqual(board.debug_registers()['registers']['pc'], 0x40123)
        self.assertEqual(session.writes, [b'\x1d'*3, b'Y'])
        board.memory_write(0x40000, '010203')
        self.assertEqual(board.memory_read(0x40000,3)['hex'], '010203')
        board.paused = True; session.writes.clear()
        board.memory_read(0x40000,1)
        self.assertTrue(board.paused); self.assertEqual(session.writes, [])
        for address in [0x3FFFF,0xAF000,0xFFFFFF]:
            with self.assertRaises(ValueError): board.memory_write(address,'01')
        with self.assertRaises(ValueError): board.memory_read(0xBFFFF,2)
    def test_rpc_lifecycle_listing_tool_errors_and_notifications(self):
        server = RpcServer(FakeBoard())
        self.assertIn('error',self.rpc(server,'tools/list'))
        self.initialize(server)
        names={tool['name'] for tool in self.rpc(server,'tools/list')['result']['tools']}
        self.assertTrue({'evaluate','console_input','module_stage','boot_trial','connect'}<=names)
        self.assertIn('await_result',[t['name'] for t in self.rpc(server,'tools/list')['result']['tools']])
        self.assertIn('console_input',[t['name'] for t in self.rpc(server,'tools/list')['result']['tools']])
        result = self.rpc(server,'tools/call',{'name':'evaluate','arguments':{'source':'2 3 +'}})['result']
        self.assertFalse(result['isError']); self.assertEqual(json.loads(result['content'][0]['text'])['output'],'2 3 +')
        self.assertTrue(self.rpc(server,'tools/call',{'name':'evaluate','arguments':{'source':'fail'}})['result']['isError'])
        for args in [{'name':'absent'}, {'name':'evaluate','arguments':{}}, {'name':'evaluate','arguments':{'source':1}}]:
            self.assertIn('error',self.rpc(server,'tools/call',args))
        self.assertIsNone(server.handle({'jsonrpc':'2.0','method':'tools/call','params':{'name':'evaluate','arguments':{'source':'MUST-NOT-RUN'}}}))
        self.assertEqual(server.backend.calls,['2 3 +','fail'])
    def test_framing_bad_json_invalid_request_and_clean_stdio(self):
        server = RpcServer(FakeBoard()); self.initialize(server)
        source = io.BytesIO(b'{broken\n[]\n'+json.dumps({'jsonrpc':'2.0','id':3,'method':'ping'}).encode()+b'\n'+json.dumps({'jsonrpc':'2.0','id':4,'method':'ping'}).encode())
        output = io.BytesIO(); serve_stream(source,output,server)
        rows=[json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual([row.get('error',{}).get('code') for row in rows],[-32700,-32600,None,-32700])
        self.assertEqual(rows[2]['result'],{})
    def test_loopback_tcp_uses_shared_backend_and_rejects_external_bind(self):
        backend = FakeBoard()
        with self.assertRaises(ValueError): make_tcp_server(('0.0.0.0',0),backend)
        server = make_tcp_server(('127.0.0.1',0),backend)
        thread = threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        try:
            for source in ['first','second']:
                with socket.create_connection(server.server_address,timeout=2) as sock:
                    stream=sock.makefile('rwb')
                    requests=[{'jsonrpc':'2.0','id':1,'method':'initialize','params':{'protocolVersion':'2025-11-25','capabilities':{},'clientInfo':{'name':'test','version':'1'}}}, {'jsonrpc':'2.0','method':'notifications/initialized'}, {'jsonrpc':'2.0','id':2,'method':'tools/call','params':{'name':'evaluate','arguments':{'source':source}}}]
                    for request in requests: stream.write(json.dumps(request).encode()+b'\n')
                    stream.flush();self.assertIn('result',json.loads(stream.readline()));self.assertFalse(json.loads(stream.readline())['result']['isError'])
                    stream.close()
            self.assertEqual(backend.calls,['first','second'])
        finally:
            server.shutdown();server.server_close();thread.join()
    def test_failed_debug_verification_resumes_and_timeout_does_not_queue_more_input(self):
        session = Session()
        def broken_read(*args):
            raise IOError('CRC mismatch')
        board = ViosBoard(session=session, reader=broken_read); board.deployed = True
        with self.assertRaises(IOError): board.memory_read(0x40000,1)
        self.assertEqual(session.writes,[b'\x1d'*3,b'Y'])
        board.uncertain = True
        before = list(session.commands)
        with self.assertRaises(RuntimeError): board.evaluate('1 2 +')
        with self.assertRaises(RuntimeError): board.abort()
        self.assertEqual(session.commands,before)

    def test_key_wait_retains_partial_marker_and_resumes_without_new_input(self):
        class KeySession(Session):
            def command(self, envelope):
                super().command(envelope)
                self.completion, self.pending = self.pending, b''
                self.nonce = envelope[1:9]
                self.pending = envelope.encode()+b'\r\nkey waiting '
            def write(self, data):
                self.writes.append(data)
                self.pending += b'key='+data+b'\r\n@'+self.nonce[:4].encode()
            def finish(self):
                self.pending += self.nonce[4:].encode()+b'\r\nok\r\nvios> '
        session=KeySession();board=ViosBoard(session=session);board.deployed=True
        server=RpcServer(board);self.initialize(server)
        reply=self.rpc(server,'tools/call',{'name':'evaluate','arguments':{'source':'KEY .','timeout':.001}})['result']
        self.assertTrue(reply['isError'])
        first=json.loads(reply['content'][0]['text'])
        self.assertFalse(first['ok']);self.assertTrue(first['pending'])
        self.assertEqual(len(session.commands),1)
        with self.assertRaises(RuntimeError):board.abort()
        with self.assertRaises(ValueError):board.console_input('1d')
        self.assertEqual(session.writes,[])
        board.console_input('41')
        partial=board.await_result(.001)
        self.assertTrue(partial['pending']);self.assertIn('@'+session.nonce[:4],partial['output'])
        session.finish()
        result=board.await_result(.01)
        self.assertTrue(result['ok']);self.assertFalse(result['pending']);self.assertIn('key=A',result['output'])
        self.assertEqual(session.writes,[b'A']);self.assertEqual(len(session.commands),1)
        with self.assertRaises(RuntimeError):board.console_input('42')
    def test_failed_debug_and_resume_transitions_block_unsafe_followups(self):
        class BrokenTransition(Session):
            def until(self,marker,timeout=10):
                raise TimeoutError('lost transition acknowledgement')
        session=BrokenTransition();board=ViosBoard(session=session);board.deployed=True
        with self.assertRaises(TimeoutError):board.debug_registers()
        self.assertTrue(board.transition_unknown)
        before=list(session.writes)
        with self.assertRaises(RuntimeError):board.deploy()
        self.assertEqual(session.writes,before)
        with self.assertRaises(RuntimeError):board.evaluate('1')
        with self.assertRaises(RuntimeError):board.memory_read(0x40000,1)
        session=BrokenTransition();board=ViosBoard(session=session);board.deployed=True;board.paused=True
        with self.assertRaises(TimeoutError):board.resume()
        self.assertTrue(board.transition_unknown)
        with self.assertRaises(RuntimeError):board.debug_registers()

    def test_envelope_boundary_and_abort_compile_without_queued_sync(self):
        session=Session();board=ViosBoard(session=session);board.deployed=True
        self.assertTrue(board.evaluate('A'*245)['ok'])
        self.assertEqual(len(session.commands[0]),255)
        self.assertTrue(board.evaluate(': PARTIAL')['compiling'])
        self.assertFalse(board.abort()['compiling'])
        self.assertEqual(session.writes,[b'\x03'])
        self.assertTrue(all(command.startswith('@') for command in session.commands))

    def test_partial_source_write_blocks_input_until_explicit_same_port_deploy(self):
        class PartialWrite(Session):
            def command(self,line):
                self.commands.append(line)
                raise IOError('short serial write')
        session=PartialWrite()
        with tempfile.TemporaryDirectory() as tmp:
            image=Path(tmp)/'vios.bin';image.write_bytes(b'fixture')
            board=ViosBoard(session=session,image=image,log=Path(tmp)/'log',deployer=lambda *args:{'boot':'vios> '})
            board.deployed=True
            with self.assertRaises(IOError):board.evaluate('1 2 +')
            self.assertTrue(board.uncertain);self.assertIsNone(board.pending_nonce)
            with self.assertRaises(RuntimeError):board.evaluate('3')
            with self.assertRaises(RuntimeError):board.await_result(.001)
            with self.assertRaises(RuntimeError):board.console_input('41')
            with self.assertRaises(RuntimeError):board.abort()
            self.assertEqual(len(session.commands),1)
            self.assertEqual(session.writes,[])
            board.deploy()
            self.assertIs(board.session,session);self.assertFalse(board.uncertain)
            self.assertTrue(board.deployed);self.assertEqual(session.closed,0)
    def test_fast_console_sends_one_complete_envelope_write_per_line(self):
        class FastSession(Session):
            def write(self,data):
                self.writes.append(data)
                if data.startswith(b'@') and data.endswith(b'\r'):
                    self.command(data[:-1].decode('ascii'))
                else:
                    raise AssertionError('unexpected fast-console write')
        session=FastSession();board=ViosBoard(session=session,fast_console=True);board.deployed=True
        self.assertTrue(board.evaluate('1 2 +\n3 4 +')['ok'])
        self.assertEqual(len(session.writes),2)
        self.assertTrue(all(payload.endswith(b'\r') for payload in session.writes))
        self.assertEqual([payload[10:-1] for payload in session.writes],[b'1 2 +',b'3 4 +'])
    def test_failed_deployment_cannot_leave_a_falsely_paused_session(self):
        session=Session()
        def failed_deploy(*args):raise TimeoutError('boot completion missing')
        with tempfile.TemporaryDirectory() as tmp:
            image=Path(tmp)/'vios.bin';image.write_bytes(b'fixture')
            board=ViosBoard(session=session,image=image,log=Path(tmp)/'log',deployer=failed_deploy)
            board.paused=True
            with self.assertRaises(TimeoutError):board.deploy()
            self.assertTrue(board.transition_unknown)
            with self.assertRaises(RuntimeError):board.debug_registers()

    def test_resident_source_crc_and_prefix_budget(self):
        session=Session();board=ViosBoard(session=session);board.deployed=True;board.source_crc=True
        result=board.evaluate('1 2 + DROP')
        self.assertTrue(session.commands[0][10:].startswith('!'),'resident source CRC missing')
        self.assertNotIn('!',result['output'])
        self.assertEqual(board.source_line_limit,235)
        before=list(session.commands)
        with self.assertRaises(ValueError):board.evaluate(' '*236)
        self.assertEqual(before,session.commands)

class RebootRpcTests(unittest.TestCase):
    rpc=HarnessTests.rpc
    initialize=HarnessTests.initialize
    def test_reboot_tool_dispatches_and_rejects_arguments(self):
        from test_vios_resident_board import RebootTests
        board,raw=RebootTests().board();server=RpcServer(board);self.initialize(server)
        tools=self.rpc(server,'tools/list')['result']['tools']
        self.assertIn('reboot',[item['name'] for item in tools])
        bad=self.rpc(server,'tools/call',{'name':'reboot','arguments':{'timeout':0}})
        self.assertEqual(bad['error']['code'],-32602);self.assertEqual(raw.controls,[])
        result=self.rpc(server,'tools/call',{'name':'reboot'})['result']
        self.assertFalse(result['isError']);self.assertTrue(json.loads(result['content'][0]['text'])['rebooted'])
        self.assertEqual(raw.closed,0)
    def test_reboot_ram_backend_reports_clear_unsupported_error(self):
        board=ViosBoard();server=RpcServer(board);self.initialize(server)
        response=self.rpc(server,'tools/call',{'name':'reboot'})
        self.assertIn('result',response)
        self.assertTrue(response['result']['isError'])
        self.assertIn('resident',response['result']['content'][0]['text'].lower())
        self.assertIn('support',response['result']['content'][0]['text'].lower())
        self.assertIsNone(board.session)

    def test_force_reboot_schema_rejects_non_boolean_and_dispatches_explicit_rescue(self):
        from test_vios_resident_board import RebootTests
        board,raw=RebootTests().board();raw.boot_polls=2
        board.pending_nonce='DEADBEEF';board.uncertain=board.transition_unknown=True
        server=RpcServer(board);self.initialize(server)
        for force in (1,0,None,'true'):
            response=self.rpc(server,'tools/call',{'name':'reboot','arguments':{'force':force}})
            self.assertEqual(response['error']['code'],-32602)
        self.assertEqual(raw.controls,[])
        result=self.rpc(server,'tools/call',{'name':'reboot','arguments':{'force':True}})['result']
        self.assertFalse(result['isError'])
        self.assertTrue(json.loads(result['content'][0]['text'])['connected'])
        self.assertEqual(raw.controls,[b'Z',b'B',b'I'])

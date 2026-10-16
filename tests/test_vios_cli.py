"""Plain-text client over fixture loopback TCP servers; no serial hardware."""
import contextlib
import io
import json
from pathlib import Path
import socketserver
import sys
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from vios_harness import make_tcp_server
try:
    from vios_cli import Console, RPCClient, ProtocolError, RPCError
except ImportError:
    Console = RPCClient = ProtocolError = RPCError = None


class Board:
    def __init__(self):
        self.calls = []
        self.compiling = self.pending = False
        self.key = None
    def connect(self):
        self.calls.append(('connect',))
        return {'connected': True, 'compiling': self.compiling, 'pending': self.pending}
    def result(self, output, ok=True):
        return {'output': output, 'ok': ok, 'compiling': self.compiling, 'pending': self.pending}
    def evaluate(self, source, timeout=15):
        self.calls.append(('evaluate', source))
        if source == 'BROKEN':
            raise IOError('wire integrity lost; execution unknown')
        if source == ': DOUBLE':
            self.compiling = True
            return self.result(': DOUBLE\r\nok\r\ncompile> ')
        if source == 'DUP + ;':
            self.compiling = False
            return self.result('DUP + ;\r\nok\r\nvios> ')
        if source == 'KEY .':
            self.pending = True
            return self.result('KEY .\r\n', False)
        if source == 'BAD':
            return self.result('error: unknown word BAD\r\nerror\r\nvios> ', False)
        return self.result('42\r\nok\r\nvios> ')
    def console_input(self, hex):
        self.calls.append(('console_input', hex))
        self.key = bytes.fromhex(hex)
        return {'sent': len(self.key), 'pending': True}
    def await_result(self, timeout=15):
        self.calls.append(('await_result',))
        if self.key:
            self.pending = False
            return self.result('KEY .\r\n65\r\nok\r\nvios> ')
        return self.result('KEY .\r\n', False)
    def abort(self):
        self.calls.append(('abort',))
        self.compiling = False
        return self.result('ok\r\nvios> ')
    def words(self):
        self.calls.append(('words',))
        return self.result('DUP DROP DOUBLE\r\nok\r\nvios> ')
    def resources(self):
        self.calls.append(('resources',))
        return self.result('data-peak 4 return-peak 1\r\nok\r\nvios> ')
    def wb_open(self):
        self.calls.append(('wb_open',))
        return {'ok':True,'workbench':{'open':1,'revision':1,'words_max':512}}
    def wb_close(self):
        self.calls.append(('wb_close',))
        return {'ok':True,'workbench':{'open':0,'revision':1,'words_max':512}}
    def wb_view(self):
        self.calls.append(('wb_view',))
        return {'ok':True,'rows':[{'row':0,'text':'VIOS WORKBENCH'.ljust(80),'attributes':[3]*80}],
                'output':'WB-VIEW row 0 text HEX attr HEX'}
    def reboot(self,force=False):
        self.calls.append(('reboot',True) if force else ('reboot',))
        self.pending=self.compiling=False
        return {'rebooted': True, 'connected': True, 'pending': False, 'compiling': False, 'ok': True}


@contextlib.contextmanager
def serving(server):
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server.server_address[1]
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


class CLITests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(Console, 'Plain-text VIOS CLI missing')
    def run_console(self, lines, board=None):
        board = board or Board()
        prompts, output, errors = [], io.StringIO(), io.StringIO()
        values = iter(lines)
        def read(prompt):
            prompts.append(prompt)
            try:
                return next(values)
            except StopIteration:
                raise EOFError
        with serving(make_tcp_server(('127.0.0.1', 0), board)) as port:
            with RPCClient(port=port, timeout=.5) as client:
                result = Console(client, output=output, errors=errors).run(read)
        return board, result, prompts, output.getvalue(), errors.getvalue()

    def test_connect_compile_query_abort_and_reboot_without_deployment(self):
        board, status, prompts, output, errors = self.run_console(
            [':help', ': DOUBLE', 'DUP + ;', ':words', ':status', ': AGAIN', ':abort', ':reboot', ':quit'])
        self.assertEqual(status, 0)
        self.assertEqual(board.calls, [('connect',), ('evaluate', ': DOUBLE'), ('evaluate', 'DUP + ;'),
                                      ('words',), ('resources',), ('evaluate', ': AGAIN'), ('abort',), ('reboot',)])
        self.assertIn('compile> ', prompts)
        self.assertIn('data-peak 4', output)
        self.assertIn('DUP DROP DOUBLE', output)
        self.assertIn(':input HEX', output)
        self.assertNotIn('jsonrpc', output)
        self.assertEqual(errors, '')

    def test_workbench_remains_an_idle_command_and_view_is_readable(self):
        board,status,_,output,errors=self.run_console(
            [':wb open',':wb view','KEY .',':wb close',':input 41',':await',':wb close',':quit'])
        self.assertEqual(status,0)
        self.assertEqual(board.calls,[('connect',),('wb_open',),('wb_view',),
            ('evaluate','KEY .'),('console_input','41'),('await_result',),('wb_close',)])
        self.assertIn('VIOS WORKBENCH',output)
        self.assertNotIn('text HEX',output)
        self.assertIn(':await',errors)

    def test_workbench_snapshot_conflict_does_not_poison_forth_session(self):
        class Editing(Board):
            def wb_view(self):
                self.calls.append(('wb_view',))
                raise IOError('Workbench view changed during read')
        board,status,prompts,output,errors=self.run_console([':wb view','NEXT',':quit'],Editing())
        self.assertEqual(status,0)
        self.assertEqual(board.calls,[('connect',),('wb_view',),('evaluate','NEXT')])
        self.assertIn('changed during read',errors)
        self.assertNotIn('uncertain> ',prompts)

    def test_pending_key_blocks_source_and_state_changing_local_commands(self):
        board, status, prompts, output, errors = self.run_console(
            ['KEY .', 'MUST-NOT-RUN', ':status', ':words', ':abort', ':reboot', ':input 41', ':await', 'NEXT', ':quit'])
        self.assertEqual(status, 0)
        self.assertEqual(board.calls, [('connect',), ('evaluate', 'KEY .'), ('console_input', '41'),
                                      ('await_result',), ('evaluate', 'NEXT')])
        self.assertIn('pending> ', prompts)
        self.assertEqual(output.count('KEY .'), 1, 'await duplicated already displayed partial output')
        self.assertIn('65', output)
        self.assertIn(':input', errors)
        self.assertIn(':await', errors)

    def test_attaching_existing_pending_or_compile_state_does_not_queue_source(self):
        pending = Board();pending.pending = True
        board, _, _, _, _ = self.run_console(['MUST-NOT-RUN', ':quit'], pending)
        self.assertEqual(board.calls, [('connect',)])
        compiling = Board();compiling.compiling = True
        board, _, prompts, _, _ = self.run_console([':reboot', ':abort', ':quit'], compiling)
        self.assertEqual(board.calls, [('connect',), ('abort',)])
        self.assertEqual(prompts[0], 'compile> ')

    def test_language_error_is_complete_but_unknown_execution_blocks_followup(self):
        board, status, prompts, output, errors = self.run_console(['BAD', 'NEXT', 'BROKEN', 'MUST-NOT-RUN', ':reboot', ':quit'])
        self.assertEqual(status, 0)
        self.assertEqual(board.calls, [('connect',), ('evaluate', 'BAD'), ('evaluate', 'NEXT'), ('evaluate', 'BROKEN')])
        self.assertIn('unknown word BAD', output)
        self.assertIn('wire integrity lost', errors)
        self.assertIn('uncertain> ', prompts)

    def test_recover_explicitly_abandons_pending_or_unknown_execution_while_normal_reboot_stays_blocked(self):
        for first in ('KEY .','BROKEN',': DOUBLE'):
            with self.subTest(first=first):
                board,status,prompts,output,errors=self.run_console(
                    [first,':reboot',':recover','NEXT',':quit'])
                self.assertEqual(status,0)
                self.assertEqual(board.calls,[('connect',),('evaluate',first),('reboot',True),('evaluate','NEXT')])
                self.assertIn('unsaved RAM',output)
                self.assertEqual(prompts[-1],'vios> ')

    def test_recover_is_available_after_failed_initial_attach(self):
        class Unknown(Board):
            def connect(self):self.calls.append(('connect',));raise IOError('bridge state unknown')
        board,status,prompts,output,errors=self.run_console(['MUST-NOT-RUN',':recover','NEXT',':quit'],Unknown())
        self.assertEqual(status,0)
        self.assertEqual(board.calls,[('connect',),('reboot',True),('evaluate','NEXT')])
        self.assertEqual(prompts[0],'uncertain> ')
        self.assertIn(':recover',errors)

    def test_failed_force_recovery_does_not_offer_obsolete_key_continuation_or_retry(self):
        class Lost(Board):
            def reboot(self,force=False):self.calls.append(('reboot',force));raise IOError('restart acknowledgment lost')
        board,status,prompts,output,errors=self.run_console(
            ['KEY .',':recover',':input 41','MUST-NOT-RUN',':reboot',':quit'],Lost())
        self.assertEqual(status,0)
        self.assertEqual(board.calls,[('connect',),('evaluate','KEY .'),('reboot',True)])
        self.assertEqual(prompts[-1],'uncertain> ')
        self.assertIn('acknowledgment lost',errors)

    def test_invalid_hex_and_source_are_rejected_before_rpc(self):
        board, _, _, _, errors = self.run_console(['KEY .', ':input 4', ':input GG', ':input '+('41'*17), ':input 1D', ':quit'])
        self.assertEqual(board.calls, [('connect',), ('evaluate', 'KEY .')])
        self.assertIn('hex', errors.lower())
        board, _, _, _, _ = self.run_console(['caf\u00e9', 'X'*236, '\x1d', ':quit'])
        self.assertEqual(board.calls, [('connect',)])

    @contextlib.contextmanager
    def wire_server(self, respond):
        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                for line in self.rfile:
                    request = json.loads(line)
                    if 'id' not in request:
                        continue
                    reply = respond(request)
                    if reply is None:
                        return
                    self.wfile.write(reply);self.wfile.flush()
        class Server(socketserver.ThreadingTCPServer):
            allow_reuse_address = True
            daemon_threads = True
        with serving(Server(('127.0.0.1', 0), Handler)) as port:
            yield port

    def test_rpc_errors_are_plain_exceptions_and_response_id_is_strict(self):
        def error(request):
            return json.dumps({'jsonrpc':'2.0','id':request['id'],'error':{'code':-32602,'message':'bad arguments'}}).encode()+b'\n'
        with self.wire_server(error) as port:
            with RPCClient(port=port, timeout=.2) as client:
                with self.assertRaisesRegex(RPCError, 'bad arguments'):
                    client.request('ping')
        for bad_id in (99, True, '1'):
            with self.subTest(bad_id=bad_id):
                def wrong(request):
                    return json.dumps({'jsonrpc':'2.0','id':bad_id,'result':{}}).encode()+b'\n'
                with self.wire_server(wrong) as port:
                    with RPCClient(port=port, timeout=.2) as client:
                        with self.assertRaises(ProtocolError):client.request('ping')

    def test_timeout_disconnect_and_malformed_json_stop_without_retry(self):
        def slow(request):time.sleep(.1);return None
        for respond, expected in ((slow, TimeoutError), (lambda request:None, ConnectionError),
                                  (lambda request:b'{broken\n', ProtocolError)):
            with self.subTest(expected=expected):
                with self.wire_server(respond) as port:
                    with RPCClient(port=port, timeout=.02) as client:
                        with self.assertRaises(expected):client.request('ping')

    def test_disconnect_during_source_stops_console_before_any_followup(self):
        calls = []
        def respond(request):
            calls.append(request)
            if request['method'] == 'initialize':
                result = {'protocolVersion':'2025-11-25', 'capabilities':{}}
            elif request['params']['name'] == 'connect':
                result = {'content':[{'type':'text', 'text':'{"connected":true,"pending":false,"compiling":false}'}], 'isError':False}
            else:
                return None
            return json.dumps({'jsonrpc':'2.0', 'id':request['id'], 'result':result}).encode()+b'\n'
        output, errors = io.StringIO(), io.StringIO()
        values = iter(['FIRST', 'MUST-NOT-RUN'])
        with self.wire_server(respond) as port:
            with RPCClient(port=port, timeout=.2) as client:
                status = Console(client, output=output, errors=errors).run(lambda prompt:next(values))
        self.assertEqual(status, 1)
        self.assertEqual([request['params'].get('name') for request in calls[1:]], ['connect', 'evaluate'])
        self.assertIn('outcome may be unknown', errors.getvalue())
        self.assertNotIn('Traceback', errors.getvalue())

    def test_eof_quits_tcp_client_without_resetting_shared_board(self):
        board, status, _, _, errors = self.run_console([])
        self.assertEqual(status, 0)
        self.assertEqual(board.calls, [('connect',)])
        self.assertEqual(errors, '')


if __name__ == '__main__':
    unittest.main()

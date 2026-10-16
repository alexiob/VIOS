#!/usr/bin/env python3
"""VIOS host MCP stdio server or loopback NDJSON JSON-RPC bridge (no CPU JSON)."""
import argparse
import json
import socketserver
import sys
import threading
try:
    from .vios_board import ViosBoard
except ImportError:
    from vios_board import ViosBoard

PROTOCOL_VERSION = '2025-11-25'
MAX_FRAME = 1048576


def tool(name, description, properties=None, required=()):
    return {'name': name, 'description': description, 'inputSchema': {
        'type': 'object', 'properties': properties or {}, 'required': list(required), 'additionalProperties': False}}


TOOLS = [
    tool('deploy','Explicitly load the ESP32 RAM bridge and readback-verify VIOS RAM; resets VIOS state.'),
    tool('evaluate','Evaluate bounded ASCII Forth lines, preserving definitions/stacks and multiline compilation.',
         {'source': {'type':'string','maxLength':65536}, 'timeout': {'type':'number','exclusiveMinimum':0,'maximum':300}}, ['source']),
    tool('await_result','Wait for a timed-out evaluation to complete without queuing source or nonce input.',
         {'timeout': {'type':'number','exclusiveMinimum':0,'maximum':300}}),
    tool('console_input','Send 1..16 explicit bytes to a pending evaluation, for example a Forth KEY.',
         {'hex': {'type':'string','pattern':r'^(?:[0-9a-fA-F]{2}){1,16}$'}}, ['hex']),
    tool('words','List the resident dictionary.'),
    tool('inspect_word','Read SEE source and word/address metadata.', {'name':{'type':'string','minLength':1,'maxLength':31}}, ['name']),
    tool('stacks','Inspect the data stack using .S.'),
    tool('resources','Inspect dictionary and observed VM resources.'),
    tool('benchmark','Run validated hardware benchmarks; may take up to 120 seconds.'),
    tool('abort','Cancel input/current definition at the idle REPL. Pending KEY/wedges require console input or debug/redeploy.'),
    tool('debug_registers','Briefly debug-pause to read CPU registers, preserving prior pause state.'),
    tool('memory_read','Read 1..65536 RAM bytes from 0x40000..0xBFFFF with debug CRC checking.',
         {'address':{'type':'integer','minimum':0x40000,'maximum':0xBFFFF}, 'count':{'type':'integer','minimum':1,'maximum':65536}}, ['address']),
    tool('memory_write','Write and readback-verify 1..256 RAM bytes below 0xAF000. Trusted systems operation.',
         {'address':{'type':'integer','minimum':0x40000,'maximum':0xAEFFF}, 'hex':{'type':'string','pattern':r'^(?:[0-9a-fA-F]{2}){1,256}$'}}, ['address','hex']),
    tool('resume','Resume existing CPU PC/UART bridge after a debug pause; does not reboot.'),
]
TOOL_MAP = {item['name']:item for item in TOOLS}


class RpcError(Exception):
    def __init__(self, code, message):
        self.code, self.message = code, message


def validate_arguments(name, arguments):
    schema = TOOL_MAP[name]['inputSchema']
    if not isinstance(arguments,dict) or set(arguments)-set(schema['properties']) or set(schema['required'])-set(arguments):
        raise RpcError(-32602,'Invalid tool arguments')
    for key,value in arguments.items():
        spec = schema['properties'][key]
        valid = isinstance(value,str) if spec['type']=='string' else type(value) is int if spec['type']=='integer' else type(value) in (int,float)
        if not valid:
            raise RpcError(-32602,'Invalid argument type: '+key)
        if isinstance(value,str):
            if len(value) < spec.get('minLength',0) or len(value)>spec.get('maxLength',MAX_FRAME):
                raise RpcError(-32602,'Invalid argument length: '+key)
        elif value < spec.get('minimum',float('-inf')) or value > spec.get('maximum',float('inf')) or value <= spec.get('exclusiveMinimum',float('-inf')):
            raise RpcError(-32602,'Argument outside range: '+key)


class RpcServer:
    def __init__(self, backend, lock=None):
        self.backend = backend
        self.lock = lock or threading.RLock()
        self.initialized = False

    def handle(self, request):
        ident = None
        try:
            if not isinstance(request,dict) or request.get('jsonrpc')!='2.0' or not isinstance(request.get('method'),str):
                raise RpcError(-32600,'Invalid Request')
            if 'id' not in request:
                return None  # Notifications never execute tools or receive responses.
            ident = request['id']
            if type(ident) not in (str,int):
                ident = None
                raise RpcError(-32600,'Request id must be a string or integer')
            params = request.get('params',{})
            if not isinstance(params,dict):
                raise RpcError(-32602,'params must be an object')
            method = request['method']
            if method == 'initialize':
                if not isinstance(params.get('protocolVersion'),str) or not isinstance(params.get('capabilities'),dict) or not isinstance(params.get('clientInfo'),dict):
                    raise RpcError(-32602,'initialize requires protocolVersion, capabilities and clientInfo')
                self.initialized = True
                result = {'protocolVersion':PROTOCOL_VERSION,'capabilities':{'tools':{}},'serverInfo':{'name':'vios-harness','version':'0.1.0'},'instructions':'Call deploy explicitly once; one board session persists across tools. Source uses printable ASCII lines <=245 bytes. After timeout, use console_input for KEY and await_result; no new source is queued.'}
            elif method == 'ping':
                result = {}
            elif not self.initialized:
                raise RpcError(-32002,'Initialize this connection first')
            elif method == 'tools/list':
                result = {'tools':TOOLS}
            elif method == 'tools/call':
                name, arguments = params.get('name'), params.get('arguments',{})
                if not isinstance(name,str) or name not in TOOL_MAP:
                    raise RpcError(-32602,'Unknown tool')
                validate_arguments(name,arguments)
                try:
                    with self.lock:
                        data = getattr(self.backend,name)(**arguments)
                    result = {'content':[{'type':'text','text':json.dumps(data,ensure_ascii=True,allow_nan=False)}], 'isError': isinstance(data,dict) and data.get('ok') is False}
                except Exception as error:
                    result = {'content':[{'type':'text','text':str(error)}],'isError':True}
            else:
                raise RpcError(-32601,'Method not found')
            return {'jsonrpc':'2.0','id':ident,'result':result}
        except RpcError as error:
            return {'jsonrpc':'2.0','id':ident,'error':{'code':error.code,'message':error.message}}


def serve_stream(reader, writer, server):
    while True:
        line = reader.readline(MAX_FRAME+1)
        if not line:
            return
        try:
            if len(line)>MAX_FRAME:
                raise ValueError('JSON-RPC frame exceeds 1 MiB')
            if not line.endswith(b'\n'):
                raise ValueError('JSON-RPC frame must end with a newline')
            request = json.loads(line,parse_constant=lambda value: (_ for _ in ()).throw(ValueError('Invalid JSON number')))
            response = server.handle(request)
        except (ValueError,UnicodeError,RecursionError) as error:
            response = {'jsonrpc':'2.0','id':None,'error':{'code':-32700,'message':str(error)}}
        if response is not None:
            writer.write(json.dumps(response,ensure_ascii=True,allow_nan=False).encode()+b'\n')
            writer.flush()
        if len(line)>MAX_FRAME:
            return  # Oversized frame is not safe to resynchronize by chunks.


def make_tcp_server(address, backend):
    if address[0] != '127.0.0.1':
        raise ValueError('TCP control is limited to 127.0.0.1')
    lock = threading.RLock()
    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            serve_stream(self.rfile,self.wfile,RpcServer(backend,lock))
    class Server(socketserver.ThreadingTCPServer):
        allow_reuse_address = True
        daemon_threads = True
    return Server(address,Handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',default='/dev/cu.usbserial-22430')
    parser.add_argument('--image',help='VIOS binary; defaults to this checkout/build/vios.bin')
    parser.add_argument('--log',help='Serial evidence log; defaults to this checkout/docs/bring-up/vios-harness.jsonl')
    parser.add_argument('--tcp',metavar='127.0.0.1:PORT',help='Serve loopback TCP instead of stdio')
    parser.add_argument('--fast-console',action='store_true',help='Opt in to one UART write per envelope (default: paced characters)')
    args = parser.parse_args()
    board = ViosBoard(port=args.port,image=args.image,log=args.log,fast_console=args.fast_console)
    try:
        if args.tcp:
            host,port = args.tcp.rsplit(':',1)
            with make_tcp_server((host,int(port)),board) as server:
                print('VIOS TCP listening on %s:%s' % server.server_address,file=sys.stderr)
                server.serve_forever()
        else:
            serve_stream(sys.stdin.buffer,sys.stdout.buffer,RpcServer(board))
    except KeyboardInterrupt:
        pass
    finally:
        board.close()


if __name__ == '__main__':
    main()

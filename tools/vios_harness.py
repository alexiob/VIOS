#!/usr/bin/env python3
"""VIOS host MCP stdio server or loopback NDJSON JSON-RPC bridge (no CPU JSON)."""
import argparse
import json
import math
import re
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


IDENTIFIER_SCHEMA={'type':'string','minLength':1,'maxLength':31,'pattern':r'^[!-~]+$'}
PATH_SCHEMA={'type':'string','minLength':1,'maxLength':127,'pattern':r'^[!-~]+$'}
CRC_SCHEMA={'anyOf':[{'type':'integer','minimum':0,'maximum':4294967295},
                     {'type':'string','pattern':r'^[0-9a-fA-F]{8}$'}]}
DEPENDENCY_SCHEMA={'type':'object','properties':{'name':IDENTIFIER_SCHEMA,'crc32':CRC_SCHEMA},
                   'required':['name','crc32'],'additionalProperties':False}

TOOLS = [
    tool('connect','Attach to the already-running resident VIOS firmware without RAM deployment (resident backend).'),
    tool('reboot','Restart resident firmware and reconnect on the same serial port. '
         'force=true explicitly abandons pending/unknown execution and loses unsaved RAM state.',
         {'force':{'type':'boolean'},'timeout':{'type':'number','exclusiveMinimum':0,'maximum':300}}),
    tool('module_stage','Validate and stage exact ASCII module source and pinned dependencies; does not activate.',
         {'name':IDENTIFIER_SCHEMA,'version':IDENTIFIER_SCHEMA,
          'source':{'type':'string','maxLength':4096,'pattern':r'^[\t\r\n\u0020-\u007E]*$'},
          'test_word':{'type':'string','maxLength':31,'pattern':r'^[!-~]*$'},
          'dependencies':{'type':'array','maxItems':4,'items':DEPENDENCY_SCHEMA}},['name','version','source']),
    tool('module_activate','Activate one staged module after validation.',{'name':IDENTIFIER_SCHEMA},['name']),
    tool('module_discard','Discard the staged module without activation.'),
    tool('module_rollback','Restore a retained previous module generation.',{'name':IDENTIFIER_SCHEMA},['name']),
    tool('modules','List module versions, decimal CRC32 values and pinned dependencies.'),
    tool('module_source','Read exact retained module source plus host-computed source SHA-256.',
         {'name':IDENTIFIER_SCHEMA,'previous':{'type':'boolean'}},['name']),
    tool('module_save','Save a module package; defaults to its verified content-addressed path.',
         {'name':IDENTIFIER_SCHEMA,'path':PATH_SCHEMA},['name']),
    tool('module_load','Load a saved module package and stage/activate it.',{'path':PATH_SCHEMA},['path']),
    tool('load','Evaluate a raw Forth source file from SD.',{'path':PATH_SCHEMA},['path']),
    tool('boot_status','Inspect confirmed/candidate/current boot slots and trial state.'),
    tool('boot_trial','Write an inactive boot bundle containing all active modules and mark it pending; does not reboot.'),
    tool('boot_confirm','Confirm the currently running trial after validation.'),
    tool('boot_revert','Select the confirmed/previous boot slot; does not reboot.'),
    tool('deploy','Explicitly replace/readback-verify CPU RAM; RAM backend also loads temporary ESP bridge.'),
    tool('evaluate','Evaluate bounded ASCII Forth lines, preserving definitions/stacks and multiline compilation.',
         {'source': {'type':'string','maxLength':65536}, 'timeout': {'type':'number','exclusiveMinimum':0,'maximum':300}}, ['source']),
    tool('await_result','Wait for a timed-out evaluation to complete without queuing source or nonce input.',
         {'timeout': {'type':'number','exclusiveMinimum':0,'maximum':300}}),
    tool('console_input','Send 1..16 explicit bytes to a pending evaluation, for example a Forth KEY.',
         {'hex': {'type':'string','pattern':r'^(?:[0-9a-fA-F]{2}){1,16}$'}}, ['hex']),
    tool('resync','Recover a lost response frame at a quiet idle prompt (for example after physical keyboard bytes merged into a request) '
         'without rebooting. Never replays the request; its outcome is returned as unknown.',
         {'timeout':{'type':'number','exclusiveMinimum':0,'maximum':300}}),
    tool('words','List the resident dictionary.'),
    tool('inspect_word','Read SEE source and word/address metadata.', {'name':{'type':'string','minLength':1,'maxLength':31}}, ['name']),
    tool('stacks','Inspect the data stack using .S.'),
    tool('resources','Inspect dictionary and observed VM resources.'),
    tool('benchmark','Run validated hardware benchmarks; may take up to 120 seconds.'),
    tool('abort','Cancel input/current definition at the idle REPL. Pending KEY/wedges require console input, explicit resident reboot(force=true), or debug/redeploy.'),
    tool('debug_registers','Briefly debug-pause to read CPU registers, preserving prior pause state.'),
    tool('memory_read','Read 1..65536 RAM bytes from 0x40000..0xBFFFF with debug CRC checking.',
         {'address':{'type':'integer','minimum':0x40000,'maximum':0xBFFFF}, 'count':{'type':'integer','minimum':1,'maximum':65536}}, ['address']),
    tool('memory_write','Write and readback-verify 1..256 RAM bytes below 0xAF000. Trusted systems operation.',
         {'address':{'type':'integer','minimum':0x40000,'maximum':0xAEFFF}, 'hex':{'type':'string','pattern':r'^(?:[0-9a-fA-F]{2}){1,256}$'}}, ['address','hex']),
    tool('resume','Resume existing CPU PC/UART bridge after a debug pause; does not reboot.'),
]
WB_REVISION_SCHEMA={'type':'integer','minimum':0,'maximum':4294967295}
WB_BOUND_SCHEMA={'type':'integer','minimum':0,'maximum':4096}
TOOLS.extend([
    tool('wb_status','Inspect the native workbench draft revision and state.'),
    tool('wb_open','Open the text workbench, retaining any existing draft.'),
    tool('wb_close','Close the text workbench and retain its draft.'),
    tool('wb_browse','Browse modules or words, optionally filtering words by module.',
         {'browser':{'type':'string','pattern':r'^(MODULES|WORDS)$'},'module_filter':IDENTIFIER_SCHEMA}),
    tool('wb_select','Select a module name or absolute word catalog index.',
         {'kind':{'type':'string','pattern':r'^(MODULE|WORD)$'},'value':{'anyOf':[IDENTIFIER_SCHEMA,{'type':'integer','minimum':0,'maximum':65534}]}},['kind','value']),
    tool('wb_edit','Load exact active module source; dirty replacement requires explicit discard.',
         {'module':IDENTIFIER_SCHEMA,'discard':{'type':'boolean'}},['module']),
    tool('wb_new','Create one module draft; dirty replacement requires explicit discard.',
         {'module':IDENTIFIER_SCHEMA,'version':IDENTIFIER_SCHEMA,'discard':{'type':'boolean'}},['module','version']),
    tool('wb_patch','Apply one atomic revision-checked source patch of at most 80 ASCII bytes.',
         {'revision':WB_REVISION_SCHEMA,'offset':WB_BOUND_SCHEMA,'remove':WB_BOUND_SCHEMA,
          'source':{'type':'string','maxLength':80,'pattern':r'^[\t\r\n\x20-\x7e]*$'}},['revision','offset','remove','source']),
    tool('wb_undo','Restore the one source/cursor undo snapshot.',{'revision':WB_REVISION_SCHEMA},['revision']),
    tool('wb_metadata','Set draft version and optional test word, retaining pinned dependencies.',
         {'revision':WB_REVISION_SCHEMA,'version':IDENTIFIER_SCHEMA,'test_word':{'type':'string','maxLength':31,'pattern':r'^[!-~]*$'}},['revision','version']),
    tool('wb_stage','Validate and test the exact draft without activation.',{'revision':WB_REVISION_SCHEMA},['revision']),
    tool('wb_activate','Activate the staged draft after revision and catalog validation.',{'revision':WB_REVISION_SCHEMA},['revision']),
    tool('wb_save','Persist only the exact currently active draft.',{'revision':WB_REVISION_SCHEMA},['revision']),
    tool('wb_source','Read exact draft ASCII in bounded chunks with revision validation.'),
    tool('wb_view','Read desired 80-column screen rows with text and packed attributes.',
         {'row':{'type':'integer','minimum':0,'maximum':59},'count':{'type':'integer','minimum':1,'maximum':60}}),
    tool('wb_key','Inject a semantic workbench key (ASCII or 256..267) and modifier bits.',
         {'code':{'type':'integer','minimum':0,'maximum':267},'modifiers':{'type':'integer','minimum':0,'maximum':255}},['code']),
    tool('wb_redraw','Invalidate and recompose the desired text screen.'),
    tool('wb_flush','Drain changed spans in bounded batches and return flush counters.'),
    tool('complete','Complete a word, outer command or compiler word from the live dictionary.',
         {'prefix':{'type':'string','maxLength':31,'pattern':r'^[!-~]*$'},'limit':{'type':'integer','minimum':1,'maximum':512}}),
    tool('help','Explain an outer command or compiler word, or show a live word kind, metadata and source.',
         {'name':{'type':'string','minLength':1,'maxLength':31,'pattern':r'^[!-~]+$'}},['name']),
    tool('develop','One explicit cycle: pin named live dependencies, stage (validate+test), activate, optionally save; '
         'reports each step and stops at the first failure without retrying.',
         {'name':IDENTIFIER_SCHEMA,'version':IDENTIFIER_SCHEMA,
          'source':{'type':'string','maxLength':4096,'pattern':r'^[\t\r\n\u0020-\u007E]*$'},
          'test_word':{'type':'string','maxLength':31,'pattern':r'^[!-~]*$'},
          'depends_on':{'type':'array','maxItems':4,'items':IDENTIFIER_SCHEMA},'save':{'type':'boolean'}},['name','version','source']),
    tool('module_graph','Module dependency edges with CRC satisfaction, reverse dependents and activation order.'),
    tool('snapshot','Read-only structured live-state record: modules, exact sources, draft, stacks, BASE, engine and boot slot, '
         'with a live/dirty/staged/persisted summary. Restorable by tools/vios_fast_display_state.py. Fails closed on a torn read.',
         {'previous_sources':{'type':'boolean'}}),
    tool('wb_render_stats','Read cumulative renderer counts and target 288 kHz ticks.',{'reset':{'type':'boolean'}}),
])
TOOL_MAP = {item['name']:item for item in TOOLS}


class RpcError(Exception):
    def __init__(self, code, message):
        self.code, self.message = code, message


def _validate(value,spec,label):
    if 'anyOf' in spec:
        for choice in spec['anyOf']:
            try:
                _validate(value,choice,label)
                return
            except RpcError:
                pass
        raise RpcError(-32602,'Invalid argument: '+label)
    kind=spec['type']
    valid={'string':isinstance(value,str),'integer':type(value) is int,
           'number':type(value) in (int,float),'boolean':type(value) is bool,
           'object':isinstance(value,dict),'array':isinstance(value,list)}.get(kind,False)
    if not valid:raise RpcError(-32602,'Invalid argument type: '+label)
    if kind=='object':
        properties=spec.get('properties',{})
        if (spec.get('additionalProperties') is False and set(value)-set(properties)) or set(spec.get('required',()))-set(value):
            raise RpcError(-32602,'Invalid tool arguments: '+label)
        for key,item in value.items():
            if key in properties:_validate(item,properties[key],label+'.'+key)
    elif kind=='array':
        if not spec.get('minItems',0)<=len(value)<=spec.get('maxItems',MAX_FRAME):
            raise RpcError(-32602,'Invalid argument length: '+label)
        for index,item in enumerate(value):_validate(item,spec['items'],label+'['+str(index)+']')
    elif kind=='string':
        if not spec.get('minLength',0)<=len(value)<=spec.get('maxLength',MAX_FRAME):
            raise RpcError(-32602,'Invalid argument length: '+label)
        if 'pattern' in spec and re.fullmatch(spec['pattern'],value) is None:
            raise RpcError(-32602,'Invalid argument format: '+label)
    elif kind in ('integer','number'):
        if type(value) is float and not math.isfinite(value):
            raise RpcError(-32602,'Invalid numeric argument: '+label)
        if value<spec.get('minimum',float('-inf')) or value>spec.get('maximum',float('inf')) or value<=spec.get('exclusiveMinimum',float('-inf')):
            raise RpcError(-32602,'Argument outside range: '+label)


def validate_arguments(name,arguments):
    _validate(arguments,TOOL_MAP[name]['inputSchema'],'arguments')


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
                limit = getattr(self.backend,'source_line_limit',245)
                result = {'protocolVersion':PROTOCOL_VERSION,'capabilities':{'tools':{}},'serverInfo':{'name':'vios-harness','version':'0.1.0'},'instructions':f'Call connect for resident firmware or deploy for the RAM bridge explicitly; one board session persists across tools. Source uses printable ASCII lines <={limit} bytes. After timeout, use console_input for KEY and await_result; no new source is queued.'}
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
                        if name == 'reboot' and not hasattr(self.backend,'reboot'):
                            raise RuntimeError('Reboot is supported only by the resident backend; the RAM bridge does not support reboot')
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
    parser.add_argument('--image',help='VIOS binary; defaults to build/vios.bin (ram) or build/vios-resident.bin (resident) in this checkout')
    parser.add_argument('--log',help='Serial evidence log; defaults to this checkout/docs/bring-up/vios-harness.jsonl')
    parser.add_argument('--tcp',metavar='127.0.0.1:PORT',help='Serve loopback TCP instead of stdio')
    parser.add_argument('--backend',choices=('ram','resident'),default='ram',help='RAM bridge deployment or installed resident firmware')
    parser.add_argument('--fast-console',action='store_true',help='Opt in to one UART write per envelope (default: paced characters)')
    args = parser.parse_args()
    if args.backend=='resident':
        try:
            from .vios_resident_board import ResidentBoard
        except ImportError:
            from vios_resident_board import ResidentBoard
        board=ResidentBoard(port=args.port,image=args.image,log=args.log,fast_console=args.fast_console)
    else:
        board=ViosBoard(port=args.port,image=args.image,log=args.log,fast_console=args.fast_console)
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

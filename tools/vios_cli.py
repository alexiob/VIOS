#!/usr/bin/env python3
"""Plain-text Forth console over the shared VIOS loopback MCP harness."""
import argparse
import json
import math
import re
import socket
import sys

PROTOCOL_VERSION = '2025-11-25'
MAX_FRAME = 1048576
SOURCE_LIMIT = 235
HELP = '''Enter printable ASCII Forth (up to 235 bytes per line).
:help [NAME]  Show this help, or explain a word/command
:complete PFX List completions (Tab also completes at the prompt)
:graph        Module dependencies, dependents and activation order
:events       Show console text you did not request (Agon keyboard lines, console tasks)
:describe K [ID]  Typed descriptor: word NAME, module NAME, task N, handle N, error
:why          Explain the last runtime error: word, callers and stack before unwind
:trace W [N]  Single-step word W for up to N (default 200) instructions
:status       Show resources
:words        List words
:wb ACTION    Workbench open, close, status, view or redraw
:snapshot [FILE]  Summarize live/dirty/staged/persisted state; FILE saves full JSON
:await        Await pending execution without sending source
:resync       Recover a lost response frame at an idle prompt; never replays
:input HEX    Send 1..16 explicit KEY bytes, then use :await
:abort        Cancel an idle unfinished definition
:reboot       Restart resident firmware from a synchronized idle prompt
:recover      Force supervisor restart; unsaved RAM work is lost
:quit         Disconnect this client; leave the shared board running'''


class ProtocolError(Exception):
    pass


class RPCError(Exception):
    pass


class ToolError(Exception):
    pass


class RPCClient:
    """One serialized NDJSON connection; failed requests are never retried."""
    def __init__(self, port=8765, timeout=130):
        self.port, self.timeout = port, timeout
        self.socket = self.stream = None
        self.sequence = 0

    def __enter__(self):
        self.socket = socket.create_connection(('127.0.0.1', self.port), timeout=min(5, self.timeout))
        self.socket.settimeout(self.timeout)
        self.stream = self.socket.makefile('rwb')
        return self

    def close(self):
        if self.stream is not None:
            self.stream.close()
            self.stream = None
        if self.socket is not None:
            self.socket.close()
            self.socket = None

    def __exit__(self, *unused):
        self.close()

    def _send(self, request):
        if self.stream is None:
            raise ConnectionError('Harness TCP connection is closed')
        self.stream.write(json.dumps(request, ensure_ascii=True, allow_nan=False).encode()+b'\n')
        self.stream.flush()

    def request(self, method, params=None):
        self.sequence += 1
        self._send({'jsonrpc':'2.0', 'id':self.sequence, 'method':method, 'params':params or {}})
        line = self.stream.readline(MAX_FRAME+1)
        if not line:
            raise ConnectionError('Harness disconnected; request outcome may be unknown')
        if len(line) > MAX_FRAME or not line.endswith(b'\n'):
            raise ProtocolError('Harness response exceeds frame limit or lacks a newline')
        try:
            response = json.loads(line, parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
        except (ValueError, UnicodeError, RecursionError) as error:
            raise ProtocolError('Harness returned invalid JSON') from error
        if (not isinstance(response, dict) or response.get('jsonrpc') != '2.0'
                or type(response.get('id')) is not int or response['id'] != self.sequence
                or ('result' in response) == ('error' in response)):
            raise ProtocolError('Harness response has an invalid envelope or response ID')
        if 'error' in response:
            error = response['error']
            if not isinstance(error, dict) or type(error.get('code')) is not int or not isinstance(error.get('message'), str):
                raise ProtocolError('Harness returned a malformed RPC error')
            raise RPCError(f"RPC {error['code']}: {error['message']}")
        return response['result']

    def initialize(self):
        result = self.request('initialize', {'protocolVersion':PROTOCOL_VERSION, 'capabilities':{},
                                            'clientInfo':{'name':'vios-cli', 'version':'0.1.0'}})
        if not isinstance(result, dict) or result.get('protocolVersion') != PROTOCOL_VERSION:
            raise ProtocolError('Harness MCP protocol version is unsupported')
        self._send({'jsonrpc':'2.0', 'method':'notifications/initialized'})

    def tool(self, name, arguments=None):
        result = self.request('tools/call', {'name':name, 'arguments':arguments or {}})
        if (not isinstance(result, dict) or not isinstance(result.get('content'), list)
                or type(result.get('isError', False)) is not bool):
            raise ProtocolError('Harness returned a malformed tool result')
        blocks = result['content']
        if not blocks or any(not isinstance(block, dict) or block.get('type') != 'text'
                             or not isinstance(block.get('text'), str) for block in blocks):
            raise ProtocolError('Harness tool result must contain text')
        text = '\n'.join(block['text'] for block in blocks)
        try:
            data = json.loads(text)
        except (ValueError, RecursionError) as error:
            if result.get('isError'):
                raise ToolError(text) from error
            raise ProtocolError('Harness tool result is not structured JSON') from error
        if not isinstance(data, dict):
            raise ProtocolError('Harness tool result must be an object')
        if result.get('isError') and data.get('ok') is not False:
            raise ToolError(str(data.get('error', text)))
        return data


class Console:
    def __init__(self, client, output=None, errors=None, timeout=15):
        self.client = client
        self.output = output if output is not None else sys.stdout
        self.errors = errors if errors is not None else sys.stderr
        self.timeout = timeout
        self.connected = self.pending = self.compiling = self.uncertain = False
        self.partial_output = ''

    @property
    def prompt(self):
        return ('pending' if self.pending else 'uncertain' if self.uncertain else
                'compile' if self.compiling else 'vios')+'> '

    def _warning(self):
        if self.pending:
            print('Pending: use :input HEX for KEY, then :await; :recover discards unsaved RAM. :reboot requires idle.', file=self.errors)
        elif self.uncertain:
            print('Execution uncertain: :await can finish known pending work; :recover discards unsaved RAM. :reboot requires idle.', file=self.errors)

    def _result(self, data):
        for flag in ('pending', 'compiling', 'connected', 'uncertain', 'ok'):
            if flag in data and type(data[flag]) is not bool:
                raise ProtocolError('Harness returned invalid '+flag+' state')
        if 'output' in data and not isinstance(data['output'], str):
            raise ProtocolError('Harness returned invalid console output')
        for flag in ('pending', 'compiling', 'connected'):
            if flag in data:
                setattr(self, flag, data[flag])
        self.uncertain = data.get('uncertain', False)
        if 'rows' in data:
            rows=data['rows']
            if (not isinstance(rows,list) or any(not isinstance(row,dict)
                    or not isinstance(row.get('text'),str) or len(row['text'])!=80
                    or any(not 32<=ord(c)<=126 for c in row['text']) for row in rows)):
                raise ProtocolError('Harness returned invalid workbench rows')
            for row in rows:
                print(row['text'].rstrip(),file=self.output)
        elif 'workbench' in data:
            state=data['workbench']
            if not isinstance(state,dict):raise ProtocolError('Harness returned invalid workbench status')
            print('workbench '+' '.join(f'{key} {value}' for key,value in state.items()),file=self.output)
        elif 'output' in data:
            raw = data['output']
            output = raw[len(self.partial_output):] if self.partial_output and raw.startswith(self.partial_output) else raw
            self.partial_output = raw if self.pending else ''
            output = re.sub(r'(?:vios|compile)> $', '', output.replace('\r', '')).rstrip('\n')
            if output:
                print(output, file=self.output)
        elif 'resources' in data:
            print(' '.join(f'{key} {value}' for key, value in data['resources'].items()), file=self.output)
        elif 'words' in data:
            print(' '.join(data['words']), file=self.output)
        elif 'sent' in data:
            print(f"Sent {data['sent']} byte(s); use :await.", file=self.output)
        elif data.get('rebooted'):
            print('Rebooted; connected.' if data.get('connected') else 'Reboot requested.', file=self.output)
        if data.get('error') and not data.get('output'):
            print(str(data['error']), file=self.errors)
        self._warning()

    def _tool(self, name, arguments=None):
        try:
            self._result(self.client.tool(name, arguments))
        except (RPCError, ToolError) as error:
            print(str(error), file=self.errors)
            # A view consistency conflict is a completed read, not unknown
            # Forth execution. The board owner still blocks unsafe source
            # itself if its transport/execution state is actually uncertain.
            if name in ('evaluate', 'abort', 'console_input', 'reboot', 'await_result'):
                self.uncertain = True
                self._warning()

    def _devtool(self, name, arguments=None):
        try:
            data = self.client.tool(name, arguments)
        except (RPCError, ToolError) as error:
            print(str(error), file=self.errors)
            return None
        if data.get('ok') is False:
            print(str(data.get('error', 'failed')), file=self.errors)
        elif name == 'help':
            print(f"{data['name']} ({data['kind']}): " + (data.get('help') or data.get('see', '')), file=self.output)
        elif name == 'complete':
            print(' '.join(m['name'] for m in data['matches']) + (' ...' if data['truncated'] else ''), file=self.output)
        elif name == 'module_graph':
            print('order ' + ' '.join(data['activation_order']), file=self.output)
            for edge in data['edges']:
                print(f"{edge['from']} -> {edge['to']} {edge['crc32']:08X}" + ('' if edge['satisfied'] else ' UNSATISFIED'), file=self.output)
            for node, users in data['dependents'].items():
                if users:
                    print(f"{node} used by {' '.join(users)}", file=self.output)
        return data

    def _why(self):
        try:
            data = self.client.tool('last_error')
        except (RPCError, ToolError) as error:
            print(str(error), file=self.errors)
            return
        e = data.get('last_error')
        if not e:
            print('No runtime error recorded since boot.', file=self.output)
            return
        at = f" at `{e['source_token']}`" if e.get('source_token') else ''
        print(f"error #{e['serial']}: {e['message']} in {e['word'] or '?'}+{e['offset']}{at} ({e['engine']}, base {e['base']})"
              + (' [dictionary changed since]' if e['stale'] else ''), file=self.output)
        for f in e['frames']:
            at = f" at `{f['source_token']}`" if f.get('source_token') else ''
            print(f"  called from {f['word'] or '?'}+{f['offset']}{at}", file=self.output)
        print(f"  data stack (top first, {e['ds_depth']} deep): " + ' '.join(map(str, e['ds_top_first'])), file=self.output)

    def completer(self, text, state):
        """readline completer; one cached dictionary query per prefix."""
        if state == 0:
            self._completions = []
            if self.connected and not (self.pending or self.uncertain):
                try:
                    data = self.client.tool('complete', {'prefix':text[:31], 'limit':128})
                    self._completions = [m['name'] for m in data.get('matches', [])]
                except (RPCError, ToolError, ProtocolError, OSError):
                    pass
        return self._completions[state] if state < len(self._completions) else None

    def _snapshot(self, path):
        try:
            data = self.client.tool('snapshot')
        except (RPCError, ToolError) as error:
            print(str(error), file=self.errors)
            return
        summary = data.get('summary')
        if data.get('ok') is not True or not isinstance(summary, dict):
            raise ProtocolError('Harness returned an invalid snapshot')
        draft, boot = summary['draft'], summary['boot']
        print(f"engine {summary['engine']} base {summary['base']} ds {summary['data_stack_depth']} "
              f"boot {boot['slot']} confirmed {boot['confirmed']} {boot['state']}", file=self.output)
        for m in summary['modules']:
            flags = [('live' if m['live'] else 'retained'), *(['rollback'] if m['rollback_available'] else []),
                     *([] if m['dependencies_satisfied'] else ['deps-unsatisfied:'+','.join(m['unsatisfied_dependencies'])]),
                     *(['draft:'+m['draft']] if m['draft'] else []),
                     'persisted:'+{True:'yes', False:'no', None:'unknown'}[m['persisted']]]
            print(f"module {m['name']} {m['version']} {m['crc32']:08X} "+' '.join(flags), file=self.output)
        print(f"draft {draft['module'] or '-'} {draft['state']} rev {draft['revision']} bytes {draft['bytes']}", file=self.output)
        if path:
            try:
                with open(path, 'x', encoding='ascii') as handle:
                    json.dump(data, handle, indent=2, ensure_ascii=True)
                    handle.write('\n')
            except OSError as error:
                print(f'Snapshot not written: {error}', file=self.errors)
            else:
                print(f'Snapshot written to {path}', file=self.output)

    def _idle(self, allow_compile=False):
        if self.pending or self.uncertain:
            self._warning()
            return False
        if not self.connected:
            print('Resident connection is not ready; reconnect through the harness.', file=self.errors)
            return False
        if self.compiling and not allow_compile:
            print('Definition in progress: finish it or use :abort.', file=self.errors)
            return False
        return True

    def line(self, line):
        """Handle one human input line; False means local quit."""
        token, _, argument = line.strip().partition(' ')
        if token.startswith(':') and len(token) > 1:
            if token == ':quit' and not argument:
                return False
            if token == ':help' and not argument:
                print(HELP, file=self.output)
            elif token == ':help':
                if self._idle():
                    self._devtool('help', {'name':argument})
            elif token == ':complete':
                if self._idle():
                    self._devtool('complete', {'prefix':argument})
            elif token == ':trace' and argument:
                parts = argument.split()
                if len(parts) > 2 or (len(parts) == 2 and not parts[1].isdigit()):
                    print('Use :trace WORD [STEPS]', file=self.errors)
                elif self._idle():
                    self._tool('evaluate', {'source': 'TRACE ' + parts[0] + ' ' + (parts[1] if len(parts) == 2 else '200'), 'timeout': self.timeout})
            elif token == ':why' and not argument:
                if self._idle():
                    self._why()
            elif token == ':events' and not argument:
                if self._idle():
                    data = self._devtool('events')
                    for event in (data or {}).get('events', []):
                        print(event['text'].replace('\r', '').rstrip('\n'), file=self.output)
            elif token == ':describe' and argument:
                kind, _, ident = argument.partition(' ')
                arguments = {'kind': kind}
                if ident:
                    arguments['identity'] = int(ident) if ident.isdigit() else ident
                if self._idle():
                    data = self._devtool('describe', arguments)
                    if data and data.get('ok'):
                        print(json.dumps({k: data[k] for k in ('kind', 'identity', 'generation', 'object')}, indent=1), file=self.output)
            elif token == ':graph' and not argument:
                if self._idle():
                    self._devtool('module_graph')
            elif token == ':input':
                if not re.fullmatch(r'(?:[0-9a-fA-F]{2}){1,16}', argument) or b'\x1d' in bytes.fromhex(argument):
                    print('Use :input HEX with 1..16 bytes; hex 1D is reserved.', file=self.errors)
                elif not self.pending:
                    print(':input requires pending execution.', file=self.errors)
                elif self.uncertain:
                    self._warning()
                else:
                    self._tool('console_input', {'hex':argument})
            elif token == ':await' and not argument:
                if self.pending or self.uncertain:
                    self._tool('await_result', {'timeout':self.timeout})
                else:
                    print('No execution is pending.', file=self.errors)
            elif token == ':resync' and not argument:
                self._tool('resync', {'timeout':self.timeout})
            elif token == ':recover' and not argument:
                print('Recovering through the supervisor; unsaved RAM work is lost.', file=self.output)
                # The old continuation is abandoned even if the restart ACK is
                # lost. Never offer KEY input or append old partial output again.
                self.pending = self.compiling = self.connected = False
                self.uncertain = True
                self.partial_output = ''
                self._tool('reboot', {'force':True})
            elif token in (':status', ':words', ':abort', ':reboot') and not argument:
                if self._idle(allow_compile=token == ':abort'):
                    self._tool({':status':'resources', ':words':'words', ':abort':'abort', ':reboot':'reboot'}[token])
            elif token == ':snapshot':
                if self._idle():
                    self._snapshot(argument)
            elif token == ':wb':
                if argument not in ('open','close','status','view','redraw'):
                    print('Use :wb open, close, status, view or redraw.',file=self.errors)
                elif self._idle():
                    self._tool('wb_'+argument)
            else:
                print('Unknown local command; use :help.', file=self.errors)
        elif self._idle(allow_compile=True):
            source = line.replace('\t', ' ')
            if len(source) > SOURCE_LIMIT or any(not 32 <= ord(char) <= 126 for char in source):
                print('Use at most 235 printable ASCII bytes per source line.', file=self.errors)
            else:
                self._tool('evaluate', {'source':source, 'timeout':self.timeout})
        return True

    def run(self, read=input):
        if read is input:
            try:
                import readline
                readline.set_completer(self.completer)
                readline.set_completer_delims(' \t')
                readline.parse_and_bind('bind ^I rl_complete' if 'libedit' in (readline.__doc__ or '') else 'tab: complete')
            except ImportError:
                pass
        try:
            self.client.initialize()
            try:
                self._result(self.client.tool('connect'))
                if not self.connected and not self.pending:
                    raise ToolError('Harness did not establish a resident connection')
            except (RPCError, ToolError) as error:
                # The TCP harness is reachable but its board state may need an
                # explicit supervisor restart. Transport/framing errors still
                # leave through the outer failure path without any retry.
                self.connected = False
                self.uncertain = True
                print(f'Attach failed: {error}. Use :recover to discard unsaved RAM and restart.', file=self.errors)
            print('VIOS console — :help for commands.', file=self.output)
            while True:
                try:
                    line = read(self.prompt)
                except EOFError:
                    return 0
                if not self.line(line):
                    return 0
        except KeyboardInterrupt:
            print(file=self.output)
            return 130
        except (OSError, ProtocolError, RPCError, ToolError) as error:
            self.uncertain = True
            print(f'Console stopped: {error}. No request was retried; shared board left running.', file=self.errors)
            return 1


def positive_timeout(value):
    number = float(value)
    if not math.isfinite(number) or not 0 < number <= 300:
        raise argparse.ArgumentTypeError('timeout must be >0 and <=300 seconds')
    return number


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8765, help='shared harness TCP port on 127.0.0.1 (default: 8765)')
    parser.add_argument('--timeout', type=positive_timeout, default=15, help='evaluate/await timeout in seconds (default: 15)')
    parser.add_argument('--rpc-timeout', type=positive_timeout, default=130, help='TCP response deadline; allow resident boot time (default: 130)')
    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65535:
        parser.error('TCP port must be 1..65535')
    try:
        with RPCClient(port=args.port, timeout=args.rpc_timeout) as client:
            return Console(client, timeout=args.timeout).run()
    except OSError as error:
        print(f'Cannot connect to shared VIOS harness on 127.0.0.1:{args.port}: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())

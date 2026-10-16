#!/usr/bin/env python3
"""Explicit, destructive-to-RAM integration check of the real VIOS harness."""
import argparse
import json
from pathlib import Path
import socket
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--transport', choices=['tcp', 'stdio'], default='tcp')
    parser.add_argument('--fast-console', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    events = []
    command = [sys.executable, '-u', str(ROOT/'tools/vios_harness.py'),
               '--log', str(args.output.with_suffix('.serial.jsonl'))]
    if args.fast_console: command.append('--fast-console')
    port = None
    if args.transport == 'tcp':
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        command += ['--tcp', f'127.0.0.1:{port}']
    stderr = args.output.with_suffix('.stderr.txt').open('w')
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=stderr, cwd=ROOT)
    sock = stream = None
    ident = 0
    try:
        if port is not None:
            deadline = time.monotonic()+5
            while True:
                try:
                    sock = socket.create_connection(('127.0.0.1', port), timeout=300)
                    stream = sock.makefile('rwb')
                    break
                except ConnectionRefusedError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(.05)
            reader = writer = stream
        else:
            reader, writer = process.stdout, process.stdin

        def rpc(method, params=None):
            nonlocal ident
            ident += 1
            request = {'jsonrpc':'2.0','id':ident,'method':method,'params':params or {}}
            start = time.monotonic()
            writer.write(json.dumps(request).encode()+b'\n'); writer.flush()
            response = json.loads(reader.readline())
            elapsed = (time.monotonic()-start)*1000
            events.append({'request':request,'response':response,'milliseconds':elapsed})
            assert response['id'] == ident and 'error' not in response, response
            return response['result'], elapsed

        def call(name, arguments=None, expected_error=False):
            result, elapsed = rpc('tools/call', {'name':name,'arguments':arguments or {}})
            assert bool(result.get('isError')) == expected_error, result
            text = result['content'][0]['text']
            try:
                data = json.loads(text)
            except ValueError:
                data = text
            return data, elapsed

        rpc('initialize', {'protocolVersion':'2025-11-25','capabilities':{},
                           'clientInfo':{'name':'vios-real-hardware-check','version':'1'}})
        writer.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n'); writer.flush()
        tools, _ = rpc('tools/list'); assert len(tools['tools']) == 14
        deployed, _ = call('deploy'); print('RAM deploy verified', flush=True)
        call('evaluate', {'source':': HARNESS-SQUARE DUP * ;'})
        data, _ = call('evaluate', {'source':'9 HARNESS-SQUARE .'})
        assert '81 ' in data['output'], data
        # Inspection and synchronization preserve both BASE and stack contents.
        call('evaluate', {'source':'HEX A'})
        word, _ = call('inspect_word', {'name':'HARNESS-SQUARE'})
        assert word['word']['name'] == 'HARNESS-SQUARE' and word['word']['direct-bytes'] > 0, word
        resources, _ = call('resources'); assert resources['resources']['engine'] == 'DIRECT'
        stack, _ = call('stacks'); assert stack['data_stack']['values_text'] == ['A'], stack
        data, _ = call('evaluate', {'source':'. DECIMAL'})
        assert 'A ' in data['output'], data
        data, _ = call('evaluate', {'source':': MULTI-HARNESS\n7 8 +'})
        assert data['compiling'], data
        data, _ = call('evaluate', {'source':';\nMULTI-HARNESS .'})
        assert not data['compiling'] and '15 ' in data['output'], data
        call('evaluate', {'source':'DEFER SERVICE\n\' HARNESS-SQUARE IS SERVICE\n: CLIENT SERVICE ;\n3 CLIENT .'})
        data, _ = call('evaluate', {'source':"' MULTI-HARNESS IS SERVICE\nCLIENT ."})
        assert '15 ' in data['output'], data
        words, _ = call('words'); assert 'HARNESS-SQUARE' in words['words'], words
        call('evaluate', {'source':'NO-SUCH-HARNESS-WORD'}, expected_error=True)
        call('evaluate', {'source':'1 2 + DROP'})
        call('evaluate', {'source':': DISCARDED 1'})
        call('abort')
        data, _ = call('evaluate', {'source':'2 3 + .'})
        assert '5 ' in data['output'] and not data['compiling'], data
        pending, _ = call('evaluate', {'source':'KEY .','timeout':.2}, expected_error=True)
        assert pending['pending'], pending
        call('console_input', {'hex':'41'})
        data, _ = call('await_result')
        assert not data['pending'] and '65 ' in data['output'], data
        registers, _ = call('debug_registers')
        assert 0x40000 <= registers['registers']['pc'] < 0xB0000, registers
        scratch = 0x80000
        original, _ = call('memory_read', {'address':scratch,'count':8})
        try:
            call('memory_write', {'address':scratch,'hex':'0123456789abcdef'})
            changed, _ = call('memory_read', {'address':scratch,'count':8})
            assert changed['hex'] == '0123456789abcdef', changed
        finally:
            call('memory_write', {'address':scratch,'hex':original['hex']})
        call('memory_read', {'address':0xBFFFF,'count':2}, expected_error=True)
        call('resume')
        data, _ = call('evaluate', {'source':'11 HARNESS-SQUARE .'})
        assert '121 ' in data['output'], data
        if args.fast_console:
            # Exercise the entire 255-byte envelope input buffer repeatedly,
            # then compile and run a long line without per-character sleeps.
            source = '1 2 + DROP'
            for _ in range(50):
                call('evaluate', {'source':source.rjust(245)})
            source = ': FAST-STRESS 17 19 + ;'
            call('evaluate', {'source':source.rjust(245)})
            data, _ = call('evaluate', {'source':'FAST-STRESS .'})
            assert '36 ' in data['output'], data
        latency = {}
        for name, arguments in [('evaluate',{'source':'1 2 + DROP'}),
                                ('inspect_word',{'name':'HARNESS-SQUARE'}),
                                ('resources',{})]:
            samples = [call(name, arguments)[1] for _ in range(9)]
            latency[name] = {'samples_ms':samples,'median_ms':statistics.median(samples)}
        report = {'transport':args.transport,'fast_console':args.fast_console,'passed':True,'deploy':deployed,
                  'latency':latency,'events':events}
        args.output.write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps({'passed':True,'transport':args.transport,'latency':latency},indent=2),flush=True)
    finally:
        if not args.output.exists():
            args.output.write_text(json.dumps({'passed':False,'transport':args.transport,'events':events},indent=2)+'\n')
        if stream is not None: stream.close()
        if sock is not None: sock.close()
        if args.transport == 'stdio':
            process.stdin.close()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.terminate()
        else:
            process.send_signal(2)
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
        stderr.close()


if __name__ == '__main__':
    main()

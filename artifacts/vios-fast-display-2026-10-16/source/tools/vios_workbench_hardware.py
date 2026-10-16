#!/usr/bin/env python3
"""Workbench acceptance/benchmarks through an existing loopback TCP harness.

No serial port is opened, image deployed, SD file saved, or boot slot changed.
The run creates one RAM-only WBM1P module and temporary observer test words.
Replies establish VDP protocol behavior, not physical display/keyboard visibility.

  python3 tools/vios_workbench_hardware.py --output /tmp/wb-hardware.json
  python3 tools/vios_workbench_hardware.py --self-test
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import sys
import time
try:
    from .vios_cli import RPCClient
except ImportError:
    from vios_cli import RPCClient

TIMER_HZ = 288000
VDP_UART_BAUD = 115200
MODULE = 'WBM1P'
KEY_UP, KEY_DOWN = 256, 257


class CheckFailed(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise CheckFailed(message)


def record_fields(output, tag):
    """Parse the unique explicit device record, ignoring echoes/prompts."""
    # The real REPL echoes commands (e.g. "WB-RENDER-STATS RESET").
    # Device records begin with a lower-case field name; command arguments do
    # not. Keep malformed device records so validation still rejects them.
    rows = [line.strip() for line in output.replace('\r', '').splitlines()
            if re.match(re.escape(tag)+r' [a-z_]+(?: |$)',line.strip())]
    require(len(rows) == 1, f'Expected one {tag} record, received {rows!r}')
    tokens = rows[0].split()[1:]
    require(len(tokens) % 2 == 0, f'Malformed {tag} record')
    result = {}
    for key, value in zip(tokens[::2], tokens[1::2]):
        require(key not in result, f'Duplicate {tag} field {key}')
        result[key] = value
    return result


def integer_fields(output, tag, names):
    record = record_fields(output, tag)
    for name in names:
        require(name in record and re.fullmatch(r'\d+', record[name]),
                f'Missing or invalid {tag} numeric field {name}')
        record[name] = int(record[name])
    return record


def decoded(value):
    if value == '-':
        return ''
    require(re.fullmatch(r'(?:[0-9A-F]{2})+', value) is not None, 'Invalid device hex string')
    return bytes.fromhex(value).decode('ascii')


def numeric_output(output, command):
    values = []
    for line in output.replace('\r', '').splitlines():
        line = line.strip()
        if line == command:
            continue
        if re.fullmatch(r'-?\d+(?:\s+-?\d+)*', line):
            values.extend(map(int, line.split()))
    return values


class Run:
    def __init__(self, client, report, output):
        self.client, self.report, self.output = client, report, output
        self.usable = True

    def save(self):
        self.output.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.output.with_name(self.output.name+'.tmp')
        temporary.write_text(json.dumps(self.report, indent=2)+'\n', encoding='utf-8')
        temporary.replace(self.output)

    def tool(self, name, arguments=None, allow_error=False):
        event = {'tool': name, 'arguments': arguments or {}, 'started_utc': datetime.now(timezone.utc).isoformat()}
        self.report['events'].append(event)
        start = time.perf_counter()
        try:
            result = self.client.tool(name, arguments)
            event['result'] = result
            if result.get('pending') or result.get('compiling') or result.get('uncertain'):
                self.usable = False
                raise CheckFailed(f'{name} did not return to a synchronized idle interpreter: {result!r}')
            require(result.get('ok') is not False or allow_error, f'{name} failed: {result!r}')
            return result
        except BaseException as error:
            if 'result' not in event:
                self.usable = False
            event['exception'] = str(error)
            raise
        finally:
            event['host_elapsed_ms'] = round((time.perf_counter()-start)*1000, 3)
            self.save()

    def command(self, source, allow_error=False):
        require(len(source) <= 235 and all(32 <= ord(ch) <= 126 for ch in source),
                'Command exceeds bounded printable ASCII console line')
        return self.tool('evaluate', {'source': source, 'timeout': 30}, allow_error)

    def status(self):
        record = integer_fields(self.command('WB-STATUS').get('output', ''), 'WB-STATUS',
                                ['revision', 'open', 'draft', 'dirty', 'active', 'length', 'cursor', 'focus'])
        for name in ('module', 'version', 'test', 'error'):
            record[name] = decoded(record[name])
        return record

    def stats(self, reset=False):
        result = self.command('WB-RENDER-STATS'+(' RESET' if reset else ''))
        stats = integer_fields(result.get('output', ''), 'WB-RENDER-STATS',
                               ['cells', 'spans', 'batches', 'bytes', 'elapsed_ticks', 'pending'])
        stats['target_render_ms'] = round(stats['elapsed_ticks']*1000/TIMER_HZ, 6)
        return stats

    def settle(self):
        self.command('WB-FLUSH')
        # A VDP character query is ordered after all outgoing glyphs. The reply
        # proves command consumption; host timing includes harness round trips.
        self.command('0 0 CHAR@ DROP')
        stats = self.stats()
        require(stats['pending'] == 0, 'Painting remains pending after WB-FLUSH')
        require(stats['bytes'] <= stats['batches']*128, 'Renderer exceeded 128 bytes per batch')
        require(stats['bytes'] == stats['cells']+7*stats['spans'], 'Wire byte accounting mismatch')
        return stats

    def measure(self, name, action):
        self.settle()
        self.stats(reset=True)
        start = time.perf_counter()
        action()
        stats = self.settle()
        stats['host_elapsed_ms'] = round((time.perf_counter()-start)*1000, 3)
        stats['wire_min_ms_8n1'] = round(stats['bytes']*10*1000/VDP_UART_BAUD, 6)
        stats['fence'] = '0 0 CHAR@ DROP (VDP 0x83 reply)'
        self.report['benchmarks'][name] = stats
        self.save()
        print(json.dumps({'benchmark': name, **stats}), flush=True)
        return stats

    def patch(self, offset, remove, text):
        status = self.status()
        encoded = text.encode('ascii').hex().upper() or '-'
        self.command(f'WB-PATCH {status["revision"]} {offset} {remove} {encoded}')
        after = self.status()
        require(after['revision'] == status['revision']+1, 'Successful patch did not advance revision')
        return after

    def read_source(self):
        status = self.status()
        chunks = []
        for offset in range(0, status['length'], 80):
            row = integer_fields(self.command(f'WB-SOURCE {offset} 80').get('output', ''),
                                 'WB-SOURCE', ['revision', 'offset', 'total'])
            require(row['offset'] == offset and row['total'] == status['length'] and
                    row['revision'] == status['revision'], 'Source chunk coordinates/revision changed')
            chunks.append(decoded(row['hex']))
        return ''.join(chunks)

    def fill_source(self, source):
        self.command(f'WB-NEW {MODULE} 1.0 0')
        for offset in range(0, len(source), 80):
            self.patch(offset, 0, source[offset:offset+80])
        require(self.status()['length'] == len(source), 'Draft length differs from fixture')
        require(self.read_source() == source, 'Source round trip lost bytes')
        self.report['fixture_source'] = source


def check_observer(run, probe_stack, base_command):
    # Inspect the actual deferred target through the structured inspector view
    # before replacing it; preserve custom user bindings by refusing them.
    catalog = run.command('WB-BROWSE WORDS').get('output', '')
    matches = re.findall(r'^WB-ITEM word (\d+) kind \d+ name ([0-9A-F]+) module (?:[0-9A-F]+|-)\s*$',
                         catalog.replace('\r', ''), re.M)
    indices = [int(index) for index, name in matches if decoded(name) == 'WB-PAINT']
    require(len(indices) == 1, 'Cannot locate WB-PAINT in structured catalog')
    run.command(f'WB-SELECT WORD {indices[0]}')
    view = record_fields(run.command('WB-VIEW 8 1').get('output', ''), 'WB-VIEW')
    require('DEFER -> WB-PAINT-DEFAULT' in decoded(view['text']),
            'Observer test requires the default WB-PAINT binding')
    run.command(': WBM1P-BAD DECIMAL 123 ;')
    bound = False
    try:
        bound = True  # The next request outcome can become unknown on I/O loss.
        run.command("' WBM1P-BAD IS WB-PAINT")
        failure = run.command('WB-FLUSH', allow_error=True)
        status = run.status()
        require('stack' in status['error'].lower(), 'Broken observer did not report a stack contract error')
        require(run.tool('stacks').get('data_stack') == probe_stack,
                'Failed observer altered application stack')
        require(numeric_output(run.command(base_command).get('output', ''), base_command) == [16],
                'Failed observer altered application BASE')
        run.report['broken_observer'] = {'failure': failure, 'status': status,
                                         'stack_preserved': True, 'base_preserved': True}
    finally:
        if bound and run.usable:
            run.command("' WB-PAINT-DEFAULT IS WB-PAINT")
            run.command('WB-REDRAW')
            repaired = run.settle()
            require(repaired['pending'] == 0, 'Default observer did not resume after repair')
            run.report['observer_repair'] = repaired
        elif bound:
            run.report['observer_repair_note'] = 'Connection/state uncertain; no cleanup request was sent'
    run.command('WB-BROWSE MODULES')


def check_error_preservation(run, probe_stack, base_command):
    before = run.status()
    source = run.read_source()
    stale = run.command(f'WB-PATCH {before["revision"]-1} 0 0 58', allow_error=True)
    after = run.status()
    require(stale.get('ok') is False and 'stale' in after['error'].lower(),
            'Stale patch did not fail with a revision error')
    require(all(after[key] == before[key] for key in ('revision', 'length', 'cursor', 'dirty')) and
            run.read_source() == source, 'Stale patch mutated draft state')
    bad = run.patch(len(source), 0, '\nWBM1P-MISSING\n')
    bad_source = run.read_source()
    failed = run.command(f'WB-STAGE {bad["revision"]}', allow_error=True)
    require(failed.get('ok') is False, 'Invalid source unexpectedly staged')
    require(run.status()['revision'] == bad['revision'] and run.read_source() == bad_source,
            'Failed staging changed draft source/revision')
    value_command = 'WBM1P-VALUE DECIMAL . HEX CR'
    require(numeric_output(run.command(value_command).get('output', ''), value_command) == [18],
            'Failed staging replaced the live module')
    require(run.tool('stacks').get('data_stack') == probe_stack and
            numeric_output(run.command(base_command).get('output', ''), base_command) == [16],
            'Rejected patch or failed staging changed application stack/BASE')
    run.command(f'WB-UNDO {bad["revision"]}')
    require(run.read_source() == source, 'Undo did not restore source after failed stage')
    run.report['error_preservation'] = {'stale_patch': stale, 'failed_stage': failed,
                                         'draft_preserved': True, 'live_value': 18,
                                         'stack_preserved': True, 'base_preserved': True}


def sample_display(run):
    run.settle()
    # getScreenChar compares glyph pixels with current VDP text background.
    # These samples are normal black-background cells, away from the inverse
    # editor cursor. Explicit COLOUR changes no glyph and spans set both colors.
    run.command('$80 COLOUR')
    coordinates = [(0, 0), (6, 0), (0, 1), (26, 1), (25, 20), (26, 16), (28, 18), (0, 56)]
    rows = {}
    samples = []
    for x, y in coordinates:
        if y not in rows:
            row = record_fields(run.command(f'WB-VIEW {y} 1').get('output', ''), 'WB-VIEW')
            require(int(row['row']) == y, 'View returned the wrong row')
            rows[y] = decoded(row['text'])
            require(len(rows[y]) == 80, 'View row has the wrong length')
        expected = ord(rows[y][x])
        # Pinned getScreenChar classifies pixels against current text BG.
        # Every sampled cell is normal bg0; a preceding cursor span may have
        # left bg2 selected. This changes drawing context, not existing pixels.
        command = f'$80 COLOUR ${x:X} ${y:X} CHAR@ DECIMAL . HEX CR'
        result = run.command(command)
        actual = numeric_output(result.get('output', ''), command)
        require(actual == [expected], f'VDP cell ({x},{y}) is {actual!r}; desired ASCII {expected}')
        samples.append({'x': x, 'y': y, 'desired': expected, 'actual': actual[0], 'result': result})
    run.report['display_samples'] = samples
    run.report['desired_sample_rows'] = rows


def exercise(run):
    connected = run.tool('connect')
    require(connected.get('connected'), 'Harness did not attach to the resident image')
    initial_status = run.status()
    require(not initial_status['open'] and not initial_status['dirty'],
            'Existing active workbench or dirty draft; refusing to replace it')
    modules = run.tool('modules')
    # Match the structured names, with a conservative raw fallback for older
    # harness versions. A reused namespace makes the run non-reproducible.
    require(MODULE not in json.dumps(modules), 'WBM1P already exists; use a fresh RAM acceptance image')
    initial_stack = run.tool('stacks').get('data_stack')
    require(isinstance(initial_stack, dict), 'Missing initial stack metadata')
    base_command = 'BASE @ DUP DECIMAL . BASE ! CR'
    base_values = numeric_output(run.command(base_command).get('output', ''), base_command)
    require(len(base_values) == 1 and 2 <= base_values[0] <= 36, 'Cannot read initial BASE')
    initial_base = base_values[0]
    run.report['initial'] = {'base': initial_base, 'stack': initial_stack, 'workbench': initial_status}
    probe_stack = None
    probe_live = False
    try:
        # Explicit $ prefixes work independently of the application's BASE.
        run.command('$13579B $2468AC HEX')
        probe_live = True
        probe_stack = run.tool('stacks').get('data_stack')
        run.stats(reset=True)
        run.command('WB-OPEN')
        full = run.settle()
        require(full['bytes'] > 0, 'Opening workbench produced no VDP output')
        run.report['initial_open_render'] = full
        check_observer(run, probe_stack, base_command)
        source = ': WBM1P-VALUE 17 ;\n'+''.join(f'\\ WBM1P source row {row:02d} retained exactly\n' for row in range(50))
        run.fill_source(source)
        run.command('WB-KEY 9 0')  # Browser -> editor.
        run.patch(0, 0, '')        # Place cursor at start, without source loss.
        # Include the populated source pane in the full-frame baseline. Start
        # host timing before redraw; cumulative counters retain intervening idle
        # batches as well as synchronous flush rendering.
        full = run.measure('full_repaint', lambda: run.command('WB-REDRAW'))
        unchanged = run.measure('unchanged', lambda: None)
        require(unchanged['bytes'] == 0, 'Unchanged frame emitted VDP glyph bytes')
        offset = source.index('17')+1
        edited = run.measure('one_character_edit', lambda: run.patch(offset, 1, '8'))
        require(edited['bytes'] > 0, 'Visible one-character edit did not repaint')
        require(edited['bytes'] < full['bytes'], 'One-character edit repainted as much as a full frame')
        status = run.status()
        run.command(f'WB-STAGE {status["revision"]}')
        require(integer_fields(run.command('WB-STATUS').get('output', ''), 'WB-STATUS', ['staged'])['staged'] == 1,
                'Fixture did not stage')
        run.command(f'WB-ACTIVATE {status["revision"]}')
        require(run.status()['active'] == 1, 'Fixture did not activate')
        # Print in decimal and restore HEX, keeping the application canaries.
        value_cmd = 'WBM1P-VALUE DECIMAL . HEX CR'
        require(numeric_output(run.command(value_cmd).get('output', ''), value_cmd) == [18],
                'Activated fixture returned the wrong value')
        run.command('WB-BROWSE WORDS')
        run.command('WB-SELECT WORD 0')
        run.command('WB-KEY 9 0')  # Editor -> browser.
        selected = run.measure('browser_selection', lambda: run.command(f'WB-KEY {KEY_DOWN} 0'))
        require(selected['bytes'] > 0, 'Browser selection did not repaint')
        run.command('WB-KEY 9 0')
        # Cursor on the last visible editor row; DOWN scrolls by one source row.
        line_37 = sum(len(line) for line in source.splitlines(keepends=True)[:37])
        run.patch(line_37, 0, '')
        scrolled = run.measure('source_scroll_one_row', lambda: run.command(f'WB-KEY {KEY_DOWN} 0'))
        require(scrolled['bytes'] > 0, 'Source scrolling did not repaint')
        check_error_preservation(run, probe_stack, base_command)
        after_stack = run.tool('stacks').get('data_stack')
        require(after_stack == probe_stack, 'Painting or workbench commands altered application stack')
        base = numeric_output(run.command(base_command).get('output', ''), base_command)
        require(base == [16], 'Painting or workbench commands altered BASE')
        run.report['application_state_preserved'] = True
        sample_display(run)
        run.command('WB-CLOSE')
        run.report['final_workbench'] = run.status()
    finally:
        if probe_live and run.usable:
            # Only remove our canaries if the complete original probe stack
            # still matches. Unexpected state is retained for diagnosis.
            current = run.tool('stacks').get('data_stack')
            if current == probe_stack:
                run.command(f'DROP DROP ${initial_base:X} BASE !')
                restored = run.tool('stacks').get('data_stack')
                require(restored == initial_stack, 'Cleanup did not restore initial application stack')
                run.report['application_state_restored'] = True
            else:
                run.report['cleanup_note'] = 'Stack differs from probe snapshot; left untouched for diagnosis'
        elif probe_live:
            run.report['cleanup_note'] = 'Connection/state uncertain; no cleanup request was sent'


def self_test():
    import unittest
    import tempfile
    class ParsingTests(unittest.TestCase):
        def test_record_ignores_echo_and_prompt(self):
            text = 'WB-STATUS\r\nWB-STATUS revision 12 module 57424D3150 error -\r\nvios> '
            record = integer_fields(text, 'WB-STATUS', ['revision'])
            self.assertEqual(record['revision'], 12)
            self.assertEqual(decoded(record['module']), MODULE)
            self.assertEqual(decoded(record['error']), '')
            self.assertEqual(integer_fields('WB-RENDER-STATS RESET\r\nWB-RENDER-STATS cells 0\r\n',
                                           'WB-RENDER-STATS',['cells'])['cells'],0)
            self.assertEqual(integer_fields('WB-SOURCE 0 80\r\nWB-SOURCE revision 2\r\n',
                                           'WB-SOURCE',['revision'])['revision'],2)
        def test_malformed_records_fail(self):
            for text in ('WB-STATUS revision', 'WB-STATUS revision -1',
                         'WB-STATUS revision 1 revision 2', 'WB-STATUS revision 1\nWB-STATUS revision 2'):
                with self.subTest(text=text), self.assertRaises(CheckFailed):
                    integer_fields(text, 'WB-STATUS', ['revision'])
        def test_pending_result_stops_without_retry(self):
            class PendingClient:
                calls = 0
                def tool(self, name, arguments):
                    self.calls += 1
                    return {'ok': False, 'pending': True}
            with tempfile.TemporaryDirectory() as folder:
                client = PendingClient()
                run = Run(client, {'events': []}, Path(folder)/'report.json')
                with self.assertRaises(CheckFailed): run.command('WB-FLUSH')
                self.assertFalse(run.usable)
                self.assertEqual(client.calls, 1)
        def test_hex_and_numbers(self):
            self.assertEqual(decoded('410A42'), 'A\nB')
            with self.assertRaises(CheckFailed): decoded('0a')
            self.assertEqual(numeric_output('9 .\r\n9 \r\nok\r\nvios> ', '9 .'), [9])
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ParsingTests))
    return 0 if result.wasSuccessful() else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--timeout', type=float, default=130)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.output is None:
        parser.error('--output is required for a hardware run')
    require(not args.output.exists(), 'Refusing to overwrite existing evidence')
    report = {'schema': 1, 'started_utc': datetime.now(timezone.utc).isoformat(), 'status': 'running',
              'transport': f'existing TCP harness 127.0.0.1:{args.port}', 'module': MODULE,
              'timer_hz': TIMER_HZ, 'vdp_uart_baud': VDP_UART_BAUD, 'events': [], 'benchmarks': {},
              'limits': ['VDP replies do not prove a physical display or keyboard is attached',
                         'Host elapsed includes TCP/harness/CLI/fence round trips',
                         'Target ticks sum bounded native-render calls, excluding idle gaps',
                         'Creates RAM module/words only; no SD or boot edits; no deployment or recovery']}
    try:
        with RPCClient(port=args.port, timeout=args.timeout) as client:
            client.initialize()
            run = Run(client, report, args.output)
            run.save()
            exercise(run)
        report['status'] = 'passed'
        return 0
    except BaseException as error:
        report.update(status='failed', error=str(error), error_type=type(error).__name__)
        print(f'Workbench acceptance stopped: {error}', file=sys.stderr)
        return 1
    finally:
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')


if __name__ == '__main__':
    raise SystemExit(main())

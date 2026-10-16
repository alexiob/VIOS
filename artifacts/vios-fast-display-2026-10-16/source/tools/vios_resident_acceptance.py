#!/usr/bin/env python3
"""Explicit hardware acceptance for the resident full-VDP firmware.

Construction/import never opens hardware. Execution attaches with ResidentBoard;
it never uploads a ROM helper, redeploys a CPU image, formats, or deletes files.
The default core phase changes the screen, exercises a restored RAM scratch
range and runs BENCH. Modules add a unique three-module test namespace. Boot is
explicit and refuses existing confirmed/pending startup selections; it leaves
its own v1 bundle confirmed after testing v1 -> v2 -> revert v1.

Examples:
  python3 tools/vios_resident_acceptance.py --phase core --log /tmp/core.json
  python3 tools/vios_resident_acceptance.py --phase modules --log /tmp/modules.json
  python3 tools/vios_resident_acceptance.py --phase boot --log /tmp/boot.json
  python3 tools/vios_resident_acceptance.py --phase core --phase modules --phase boot

The map must describe the actually deployed image before core's scratch write.
No visual display or audible-output success is inferred from protocol replies.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import secrets
import statistics
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
PHASES = ('core', 'modules', 'boot')


class AcceptanceError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise AcceptanceError(message)


def synchronized(result):
    require(isinstance(result, dict) and result.get('ok') is True,
            'Device command failed: ' + repr(result))
    require(not result.get('pending') and not result.get('compiling'),
            'Device did not return to an idle interpreter: ' + repr(result))
    return result


def assert_numbers(result, command, expected):
    synchronized(result)
    values = []
    for line in result.get('output', '').replace('\r', '').splitlines():
        text = line.strip()
        if text == command.strip():
            continue
        if re.fullmatch(r'-?\d+(?:\s+-?\d+)*', text):
            values.extend(int(value) for value in text.split())
    require(values == list(expected), f'Numeric readback {values!r}, expected {expected!r}')
    return values


def validate_benchmark(text):
    require('BENCH DONE' in text and 'BENCH ERROR' not in text, 'Benchmark did not complete')
    rows = []
    seen = set()
    pattern = (r'SAMPLE engine=([012]) word=BM-(ARITH|CALL|BRANCH|LOOP|MEM) '
               r'n=(25|50|100) result=(\d+) ticks=([\d,]+) median=(\d+)')
    for line in text.replace('\r', '').splitlines():
        match = re.fullmatch(pattern, line.strip())
        if not match:
            require(not line.startswith('SAMPLE '), 'Malformed benchmark sample: ' + line)
            continue
        engine, word, n, result, ticks, median = match.groups()
        engine, n, result, median = map(int, (engine, n, result, median))
        key = (engine, word, n)
        require(key not in seen, 'Duplicate benchmark condition')
        seen.add(key)
        samples = [int(value) for value in ticks.split(',')]
        require(len(samples) == 9 and all(0 <= value <= 65535 for value in samples),
                'Benchmark requires nine 16-bit timer samples per condition')
        wanted = {'ARITH': {25: 3, 50: 0, 100: 0}[n], 'CALL': 3*n,
                  'BRANCH': (n//2)*4+(n & 1), 'LOOP': n*(n-1)//2, 'MEM': n}[word]
        require(result == wanted, f'Wrong benchmark result for {key}: {result}')
        require(median == sorted(samples)[4], 'Benchmark median disagrees with samples')
        rows.append({'engine': engine, 'word': word, 'n': n, 'result': result,
                     'ticks': samples, 'median': median})
    require(len(rows) == 45, f'Expected 45 distinct benchmark conditions, received {len(rows)}')
    return rows


def scratch_bounds(map_path, address, count):
    raw = Path(map_path).read_bytes()
    text = raw.decode('utf-8')
    def symbol(name):
        match = re.search(r'\b(0x[0-9a-fA-F]+)\s+' + re.escape(name) + r'\s*=', text)
        require(match is not None, 'Missing linker-map symbol ' + name)
        return int(match[1], 16)
    bss, stack = symbol('__bss_end'), symbol('__stack_bottom')
    require(0x40000 <= bss <= address and address+count <= min(stack, 0xAF000),
            'Scratch range overlaps the deployed image/BSS/stack according to supplied map')
    return {'map': str(Path(map_path).resolve()), 'map_sha256': hashlib.sha256(raw).hexdigest(),
            'bss_end': bss, 'stack_bottom': stack, 'address': address, 'count': count,
            'assumption': 'Supplied map describes the actually deployed resident image'}


class Report:
    def __init__(self, path, namespace, phases):
        self.path = Path(path)
        require(not self.path.exists(), 'Refusing to overwrite an existing report: ' + str(path))
        self.data = {'schema': 1, 'started_utc': datetime.now(timezone.utc).isoformat(),
                     'namespace': namespace, 'phases': list(phases), 'status': 'running',
                     'hardware_run': True, 'checks': [],
                     'limits': ['VDP replies do not prove visible or audible physical output',
                                'Boot phase changes startup only from an unconfirmed clean baseline',
                                'No flash write, ROM helper upload, image deployment or file cleanup']}
        self.save()

    def save(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.path.with_name(self.path.name + '.tmp')
        temporary.write_text(json.dumps(self.data, indent=2) + '\n', encoding='utf-8')
        temporary.replace(self.path)

    def check(self, name, action):
        entry = {'name': name, 'status': 'running'}
        self.data['checks'].append(entry)
        self.save()
        print(json.dumps({'checkpoint': name, 'status': 'running'}), flush=True)
        start = time.perf_counter()
        try:
            result = action()
            entry.update(status='passed', result=result)
            return result
        except BaseException as error:
            entry.update(status='failed', error=str(error), error_type=type(error).__name__)
            self.data['status'] = 'failed'
            raise
        finally:
            entry['elapsed_ms'] = round((time.perf_counter()-start)*1000, 3)
            self.save()
            print(json.dumps({'checkpoint': name, 'status': entry['status'],
                              'elapsed_ms': entry['elapsed_ms']}), flush=True)


class Acceptance:
    def __init__(self, board, report, namespace, map_path=None):
        require(re.fullmatch(r'[A-Z][A-Z0-9_]{2,15}', namespace) is not None,
                'Namespace must be 3..16 uppercase ASCII letters/digits/underscores, starting with a letter')
        self.board, self.report, self.namespace = board, report, namespace
        self.map_path = Path(map_path) if map_path else ROOT/'build/vios-resident.map'
        self.names = {'interface': namespace+'I', 'provider': namespace+'P', 'consumer': namespace+'D'}
        self.service, self.caller = namespace+'-S', namespace+'-C'
        self.value, self.test = namespace+'-V', namespace+'-T'
        self.dependent, self.dep_test = namespace+'-D', namespace+'-U'
        self.initial_engine, self.initial_base = None, None

    def command(self, source, numbers=None, timeout=20):
        result = self.board.evaluate(source, timeout=timeout)
        synchronized(result)
        if numbers is not None:
            result['numbers'] = assert_numbers(result, source, numbers)
        return result

    def check_command(self, name, source, numbers=None, timeout=20):
        return self.report.check(name, lambda: self.command(source, numbers, timeout))

    def attach(self, timeout):
        result = self.report.check('attach-resident-no-deploy', lambda: synchronized(self.board.connect(timeout)))
        require(result.get('connected'), 'Resident backend did not report connected')
        stacks = self.report.check('require-idle-empty-stack', lambda: synchronized(self.board.stacks()))
        require(stacks.get('data_stack', {}).get('depth_text', '').strip() == '0',
                'Acceptance requires an empty data stack; it will not ABORT or discard user values')
        resources = self.report.check('initial-resources', lambda: synchronized(self.board.resources()))
        self.initial_engine = resources.get('resources', {}).get('engine')
        require(self.initial_engine in ('DIRECT', 'TOKEN', 'C'), 'Missing engine metadata')
        command = 'BASE @ DECIMAL . CR'
        base = self.board.evaluate(command)
        synchronized(base)
        output = [int(line.strip()) for line in base['output'].replace('\r', '').splitlines()
                  if re.fullmatch(r'\d+', line.strip())]
        require(len(output) == 1 and 2 <= output[0] <= 36, 'Cannot read the initial BASE')
        self.initial_base = output[0]
        self.report.data.update(identity=result.get('identity'), initial_engine=self.initial_engine,
                                initial_base=self.initial_base)
        self.report.save()

    def preflight_boot(self):
        def action():
            result = synchronized(self.board.boot_status())
            boot = result.get('boot', {})
            require(boot.get('confirmed') == 'NONE' and boot.get('candidate') == 'NONE'
                    and boot.get('current') == 'NONE' and boot.get('state') == 'CLEAN',
                    'Boot acceptance refuses existing confirmed/pending startup artifacts; '
                    'use a clean dedicated acceptance SD baseline')
            return result
        return self.report.check('boot-require-clean-unconfirmed-baseline', action)

    def core(self):
        for engine in ('DIRECT', 'TOKEN', 'C'):
            self.check_command('arithmetic-'+engine,
                               f'ENGINE {engine} DECIMAL 6 7 * . -7 3 / . -7 3 MOD . CR', [42, -2, -1])
        self.check_command('default-direct-engine', 'ENGINE DIRECT')
        require(self.board.source_line_limit == 235, 'Resident CRC source-line profile must be 235 bytes')
        stress = 'DECIMAL 123 456 + . CR'.ljust(235)
        for sample in range(50):
            self.check_command(f'max-line-two-fragment-{sample+1:02d}', stress, [579])
        def pending_key():
            result = self.board.evaluate('KEY . CR', timeout=.25)
            require(result.get('pending') is True and not result.get('compiling'),
                    'KEY did not remain pending without injected input')
            return result
        self.report.check('key-pending', pending_key)
        self.report.check('key-inject-A', lambda: self.board.console_input('41'))
        def complete_key():
            result = self.board.await_result(timeout=20)
            result['numbers'] = assert_numbers(result, 'KEY . CR', [65])
            return result
        self.report.check('key-completion', complete_key)
        def registers():
            result = self.board.debug_registers()
            require(result.get('status', 0) & 0x90 == 0x90, 'Debug snapshot is not halted in ADL mode')
            values = result.get('registers', {})
            require(set(values) == {'af', 'bc', 'de', 'hl', 'ix', 'iy', 'sp', 'pc'},
                    'Incomplete debug register snapshot')
            return result
        self.report.check('debug-registers', registers)
        self.report.check('debug-scratch-map-guard', lambda: scratch_bounds(self.map_path, 0xA0000, 16))
        original = self.report.check('debug-scratch-original', lambda: self.board.memory_read(0xA0000, 16))
        require(re.fullmatch(r'[0-9a-fA-F]{32}', original.get('hex', '')) is not None,
                'Malformed original scratch read; no write attempted')
        original_hex = original['hex']
        pattern = '00112233445566778899aabbccddeeff'
        try:
            self.report.check('debug-scratch-write-verified', lambda: self.board.memory_write(0xA0000, pattern))
            def readback():
                result = self.board.memory_read(0xA0000, 16)
                require(result.get('hex', '').lower() == pattern, 'Independent scratch readback mismatch')
                return result
            self.report.check('debug-scratch-independent-readback', readback)
        finally:
            # Even a failed readback attempts to restore the logged original.
            # Backend unknown-state guards remain authoritative; no redeploy/retry.
            self.report.check('debug-scratch-restore', lambda: self.board.memory_write(0xA0000, original_hex))
            def restored():
                result = self.board.memory_read(0xA0000, 16)
                require(result.get('hex', '').lower() == original_hex.lower(), 'Original scratch bytes were not restored')
                return result
            self.report.check('debug-scratch-restored-readback', restored)
        self.report.check('debug-resume', self.board.resume)
        self.check_command('post-debug-evaluation', 'DECIMAL 20 22 + . CR', [42])
        self.check_command('vdp-select-pinned-mode-1', '1 MODE')
        # MODE resets BBC logical coordinate scaling. CHAR@ takes text cells,
        # but PIXEL@ passes through the VDP graphics-coordinate translation.
        self.check_command('vdp-physical-pixel-coordinates', '23 VDU 0 VDU 192 VDU 0 VDU')
        def mode_info():
            result = self.command('VDP-INFO')
            match = re.search(r'vdp width (\d+) height (\d+) colours (\d+) mode (\d+) console-dropped (\d+)', result['output'])
            require(match is not None, 'Missing VDP mode response')
            fields = dict(zip(('width', 'height', 'colours', 'mode', 'console_dropped'), map(int, match.groups())))
            require([fields[key] for key in ('width', 'height', 'colours', 'mode')] == [640, 480, 4, 1],
                    'Pinned nonlegacy mode 1 was not established')
            require(fields['console_dropped'] == 0, 'Console bytes were dropped')
            result['vdp'] = fields
            return result
        self.report.check('vdp-mode-info', mode_info)
        self.check_command('vdp-clear-background', 'CLS 0 0 PIXEL@ . . CR', [0, 0])
        self.check_command('vdp-text-red-V', '1 COLOUR S" V" VTYPE')
        self.check_command('vdp-character-readback', '0 0 CHAR@ . CR', [86])
        self.check_command('vdp-red-pixel-readback', '1 0 PIXEL@ . . CR', [1, 255])
        self.check_command('vdp-beep-acknowledgment', '440 25 BEEP . CR', [1])
        for kind in ('evaluate', 'inspect', 'resources'):
            def sample_latencies(kind=kind):
                samples, results = [], []
                for _ in range(9):
                    start = time.perf_counter()
                    if kind == 'evaluate':
                        result = self.command('DECIMAL 6 7 * . CR', [42])
                    elif kind == 'inspect':
                        result = synchronized(self.board.inspect_word('+'))
                        require(result.get('word', {}).get('name') == '+', 'Missing inspected word metadata')
                    else:
                        result = synchronized(self.board.resources())
                        require('words' in result.get('resources', {}), 'Missing resource metadata')
                    samples.append(round((time.perf_counter()-start)*1000, 3))
                    results.append(result)
                return {'samples_ms': samples, 'median_ms': statistics.median(samples),
                        'minimum_ms': min(samples), 'maximum_ms': max(samples), 'responses': results}
            self.report.check('latency-nine-'+kind, sample_latencies)
        resource = synchronized(self.board.resources())
        words = str(resource.get('resources', {}).get('words', ''))
        require(re.fullmatch(r'\d+/128', words) is not None, 'Missing dictionary occupancy')
        require(int(words.split('/')[0]) <= 100, 'BENCH requires at most 100 existing dictionary words')
        def benchmark():
            result = self.board.benchmark()
            bench_path = self.report.path.with_suffix('.bench.txt')
            require(not bench_path.exists(), 'Refusing to overwrite existing benchmark output')
            bench_path.write_text(result.get('output', ''), encoding='utf-8')
            synchronized(result)
            rows = validate_benchmark(result['output'])
            return {'path': str(bench_path), 'conditions': len(rows), 'rows': rows}
        self.report.check('benchmark-all-45-conditions-once', benchmark)
        self.report.check('core-final-resources', lambda: synchronized(self.board.resources()))

    def source(self, role, value=11):
        if role == 'interface':
            return f'DEFER {self.service} : {self.caller} {self.service} ;'
        if role == 'provider':
            return (f': {self.value} {value} ; : {self.test} {self.value} {value} = ; '
                    f"' {self.value} IS {self.service}")
        return (f': {self.dependent} {self.caller} 1+ ; '
                f': {self.dep_test} {self.dependent} {value+1} = ;')

    def dependency(self, role, value=11):
        source = self.source(role, value)
        return {'name': self.names[role], 'crc32': zlib.crc32(source.encode('ascii'))}

    def stage(self, role, value=11):
        deps = [] if role == 'interface' else [self.dependency('interface')]
        if role == 'consumer':
            deps.append(self.dependency('provider', value))
        test = '' if role == 'interface' else self.test if role == 'provider' else self.dep_test
        return synchronized(self.board.module_stage(self.names[role], str(value), self.source(role, value),
                                                     test_word=test, dependencies=deps))

    def inspect_source(self, role, value):
        result = synchronized(self.board.module_source(self.names[role]))
        require(result.get('source') == self.source(role, value), 'Exact module source roundtrip mismatch')
        require(result.get('crc32') == self.dependency(role, value)['crc32'], 'Module source CRC mismatch')
        return result

    def expected_failure(self, result, diagnostic):
        require(result.get('ok') is False and not result.get('pending') and not result.get('compiling'),
                'Expected a completed module rejection: ' + repr(result))
        require(diagnostic in result.get('output', '').lower(), 'Missing expected rejection diagnostic')
        return result

    def values(self, value):
        return self.command(f'DECIMAL {self.caller} . {self.dependent} . CR', [value, value+1])

    def save(self, role):
        result = synchronized(self.board.module_save(self.names[role]))
        require(re.fullmatch(r'/VIOS/[0-9A-F]{8}/[0-9A-F]{8}\.VIM', result.get('path', '')) is not None,
                'Missing immutable module package path')
        return result

    def modules(self):
        inventory = self.report.check('module-inventory-before', lambda: synchronized(self.board.modules()))
        owned = {entry['name']: entry for entry in inventory.get('modules', []) if entry['name'] in self.names.values()}
        require(not owned, 'Namespace is already active; choose a fresh --namespace')
        require(len(inventory.get('modules', [])) <= 5, 'Three free module slots are required')
        for role in ('interface', 'provider', 'consumer'):
            if role == 'consumer':
                bad = self.dependency('provider');bad['crc32'] ^= 1
                self.report.check('module-reject-stale-dependency', lambda: self.expected_failure(
                    self.board.module_stage(self.names['consumer'], 'bad', self.source('consumer'),
                                            test_word=self.dep_test, dependencies=[bad]), 'dependency'))
                self.report.check('module-discard-bad-builder', lambda: synchronized(self.board.module_discard()))
            self.report.check('module-stage-v1-'+role, lambda role=role: self.stage(role))
            self.report.check('module-activate-v1-'+role,
                              lambda role=role: synchronized(self.board.module_activate(self.names[role])))
            self.report.check('module-source-v1-'+role, lambda role=role: self.inspect_source(role, 11))
            self.report.check('module-save-v1-'+role, lambda role=role: self.save(role))
        self.report.check('module-values-v1', lambda: self.values(11))
        bad_source = (f': {self.value} 99 ; : {self.test} 0 ; '
                      f"' {self.value} IS {self.service}")
        self.report.check('module-failed-test-restores-service', lambda: self.expected_failure(
            self.board.module_stage(self.names['provider'], 'bad', bad_source, test_word=self.test,
                                    dependencies=[self.dependency('interface')]), 'test'))
        self.report.check('module-discard-failed-test', lambda: synchronized(self.board.module_discard()))
        self.report.check('module-service-survives-failed-stage', lambda: self.values(11))
        self.report.check('module-source-survives-failed-stage', lambda: self.inspect_source('provider', 11))
        self.report.check('module-inventory-after', lambda: synchronized(self.board.modules()))

    def reboot(self, label):
        require(callable(getattr(self.board, 'reboot', None)), 'ResidentBoard.reboot() is required for true restart tests')
        result = self.report.check(label, lambda: synchronized(self.board.reboot(timeout=120)))
        require(result.get('rebooted') is True and result.get('connected') is True,
                'Backend did not prove a true resident reboot and synchronized reconnect')
        return result

    def boot_status(self, label, state='CLEAN', value=None):
        def check():
            result = synchronized(self.board.boot_status())
            boot = result['boot']
            require(boot['state'] == state, 'Unexpected boot selection state')
            if state == 'CLEAN':
                require(boot['current'] in ('A', 'B') and boot['current'] == boot['confirmed']
                        and boot['candidate'] == 'NONE', 'Running boot bundle is not confirmed')
            else:
                require(boot['candidate'] in ('A', 'B'), 'Missing trial candidate')
            return result
        result = self.report.check(label, check)
        if value is not None:
            self.report.check(label+'-values', lambda: self.values(value))
        return result

    def boot(self, modules_prepared=False):
        # Run before creating test modules so existing startup state is never touched.
        self.preflight_boot()
        inventory = synchronized(self.board.modules()).get('modules', [])
        foreign = [entry['name'] for entry in inventory if entry['name'] not in self.names.values()]
        require(not foreign, 'Boot acceptance refuses persisting unrelated active modules: ' + repr(foreign))
        if not modules_prepared:
            self.modules()
        for role in ('interface', 'provider', 'consumer'):
            self.report.check('boot-exact-v1-source-'+role, lambda role=role: self.inspect_source(role, 11))
        self.report.check('boot-trial-v1', lambda: synchronized(self.board.boot_trial()))
        self.boot_status('boot-pending-v1', 'PENDING')
        self.reboot('true-reboot-load-v1')
        first = self.boot_status('boot-confirmed-v1', value=11)['boot']['confirmed']
        self.report.check('boot-confirm-v1-idempotent', lambda: synchronized(self.board.boot_confirm()))
        # Interface stays stable; both provider and dependent pin advance together.
        for role in ('provider', 'consumer'):
            self.report.check('module-stage-v2-'+role, lambda role=role: self.stage(role, 22))
            self.report.check('module-activate-v2-'+role,
                              lambda role=role: synchronized(self.board.module_activate(self.names[role])))
            if role == 'provider':
                self.report.check('module-existing-caller-observes-v2', lambda: self.values(22))
            self.report.check('module-save-v2-'+role, lambda role=role: self.save(role))
        self.report.check('module-values-v2-before-reboot', lambda: self.values(22))
        self.report.check('boot-trial-v2', lambda: synchronized(self.board.boot_trial()))
        self.boot_status('boot-pending-v2', 'PENDING')
        self.reboot('true-reboot-load-v2')
        second = self.boot_status('boot-confirmed-v2', value=22)['boot']['confirmed']
        require(second != first, 'Trial did not switch the source-bundle slot')
        for role in ('provider', 'consumer'):
            self.report.check('boot-source-v2-'+role, lambda role=role: self.inspect_source(role, 22))
        self.report.check('boot-confirm-v2-idempotent', lambda: synchronized(self.board.boot_confirm()))
        self.report.check('boot-select-revert-v1', lambda: synchronized(self.board.boot_revert()))
        self.reboot('true-reboot-revert-v1')
        final = self.boot_status('boot-reverted-v1', value=11)['boot']['confirmed']
        require(final == first, 'Revert did not return to the original accepted source slot')
        self.report.check('boot-reverted-exact-provider-source', lambda: self.inspect_source('provider', 11))
        self.report.check('boot-final-module-inventory', lambda: synchronized(self.board.modules()))

    def restore_interpreter(self):
        return self.command(f'DECIMAL {self.initial_base} BASE ! ENGINE {self.initial_engine}')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', default='/dev/cu.usbserial-22430')
    parser.add_argument('--phase', choices=PHASES, action='append', help='Repeatable; default: core')
    parser.add_argument('--namespace', default='AT'+secrets.token_hex(4).upper())
    parser.add_argument('--log', type=Path, help='New JSON report; serial log is a sibling .serial.jsonl')
    parser.add_argument('--map', dest='map_path', type=Path, default=ROOT/'build/vios-resident.map',
                        help='Linker map of the deployed image; required for guarded scratch writes')
    parser.add_argument('--connect-timeout', type=float, default=120)
    args = parser.parse_args(argv)
    namespace = args.namespace.upper()
    if not re.fullmatch(r'[A-Z][A-Z0-9_]{2,15}', namespace):
        parser.error('--namespace must be 3..16 ASCII letters/digits/underscores, starting with a letter')
    if not 0 < args.connect_timeout <= 300:
        parser.error('--connect-timeout must be >0 and <=300')
    phases = [phase for phase in PHASES if phase in (args.phase or ['core'])]
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    log = args.log or ROOT/'docs/bring-up'/f'vios-resident-acceptance-{stamp}-{namespace}.json'
    report = Report(log, namespace, phases)
    report.data['port'] = args.port
    report.save()
    from vios_resident_board import ResidentBoard
    board = ResidentBoard(port=args.port, log=log.with_suffix('.serial.jsonl'), fast_console=True)
    acceptance = Acceptance(board, report, namespace, args.map_path)
    try:
        acceptance.attach(args.connect_timeout)
        if 'boot' in phases:
            acceptance.preflight_boot()
        if 'core' in phases:
            acceptance.core()
        if 'modules' in phases:
            acceptance.modules()
        if 'boot' in phases:
            acceptance.boot(modules_prepared='modules' in phases)
        report.check('restore-interpreter-base-and-engine', acceptance.restore_interpreter)
        report.data['status'] = 'passed'
        return 0
    except BaseException as error:
        report.data.update(status='failed', error=str(error), error_type=type(error).__name__)
        print('Acceptance stopped: ' + str(error), file=sys.stderr)
        return 130 if isinstance(error, KeyboardInterrupt) else 1
    finally:
        report.data['finished_utc'] = datetime.now(timezone.utc).isoformat()
        report.save()
        board.close()
        print(json.dumps({'report': str(log), 'status': report.data['status'], 'namespace': namespace}), flush=True)


if __name__ == '__main__':
    raise SystemExit(main())

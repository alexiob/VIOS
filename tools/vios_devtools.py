"""Completion, help and module dependency graph built from existing queries.

Host-side only: the resident dictionary (WORDS/SEE) and module inventory are
authoritative; outer commands and compiler words are documented here because
they are parsed in C and are not dictionary entries. No target RAM cost.
"""
import re

# Outer commands parsed before the dictionary (kernel/*.c, forth/system.c).
COMMANDS = {
    'WORDS': 'List dictionary words.', 'SEE': 'SEE name: show source and metadata of a word.',
    '.S': 'Show the data stack.', 'RESOURCES': 'Show dictionary/code/data usage and peaks.',
    'ENGINE': 'ENGINE DIRECT|TOKEN|C: select the execution engine (C is the portable reference engine).', 'BENCH': 'Run VM benchmarks.',
    'SYNC': 'SYNC name: print @name, a marker the host harness waits for (it does not flush the SD card).', 'ABORT': 'Clear stacks and any unfinished definition.',
    'LOAD': 'LOAD path: evaluate a Forth source file from SD.', 'INCLUDE': 'INCLUDE path: same as LOAD, nestable 4 deep.',
    'MODULES': 'List active modules, CRCs and pinned dependencies.',
    'MODULE-BEGIN': 'MODULE-BEGIN name version test|-: start a staged module source.',
    'MODULE-DEP': 'MODULE-DEP name crc32: pin a dependency of the module being built.',
    'MODULE-DATA': 'MODULE-DATA hex: append source bytes to the module being built.',
    'MODULE-STAGE': 'Validate and test the built module without activating it.',
    'MODULE-ACTIVATE': 'MODULE-ACTIVATE name: activate the staged module.',
    'MODULE-ROLLBACK': 'MODULE-ROLLBACK name: restore the previous generation (latest activation only).',
    'MODULE-DISCARD': 'Discard the staged module.', 'MODULE-SOURCE': 'MODULE-SOURCE name [PREVIOUS]: exact source as hex.',
    'MODULE-LOAD': 'MODULE-LOAD path: load, stage and activate a saved package.',
    'SAVE': 'SAVE name [path]: persist a module package (content-addressed by default).',
    'BOOT-STATUS': 'Show confirmed/candidate/current source slots.', 'BOOT-TRIAL': 'Write all active modules to the inactive slot as a pending trial.',
    'BOOT-MANIFEST': 'List modules (name, version, CRC, package state) in the confirmed and candidate boot slots.',
    'TICKS': 'Milliseconds from the 1 kHz PRT1 tick (masked to a cell); 0 if the tick is off.',
    'IRQ-STATS': 'Tick on/off, the millisecond clock and the count of stray interrupts (should stay 0).',
    'IRQ-PROBE': 'Read-only: interrupt vector chain MOS left (I, RAM jump table, non-default handlers) and timer/UART/GPIO interrupt ports.',
    'BOOT-TIMES': 'How long each startup step took (storage, state, module loads, health, finish) in ms.',
    'BOOT-CONFIRM': 'Confirm the running trial slot.', 'BOOT-REVERT': 'Select the confirmed/previous slot (no reboot).',
    'VDP-INFO': 'Show display mode and console drop counter.',
    'TASKS': 'List background task slots (SPAWN PAUSE MS TASK-KILL TASK-STATE).',
    'TASK-OUT': 'TASK-OUT id: drain a task output ring as hex.',
    'TASK-INPUT': 'id h TASK-INPUT: the task reads KEY from handle h (empty pipe waits, EOF gives -1).',
    'TASK-OUTPUT': 'id h TASK-OUTPUT: the task writes EMIT/./CR to handle h with backpressure.',
    'PIPELINE': "' a ' b PIPELINE: a's output feeds b's KEY; b prints to the console.",
    'TASK-SLICE': 'TASK-SLICE n: instructions per background slice (64..65535; default 1024).',
    'TRACE': 'TRACE name n: single-step a word for up to n instructions, printing word+offset and stacks.',
    'LAST-ERROR': 'Show the last runtime failure: word, offset, caller frames and stacks before unwind.',
    'DIR': 'DIR path: list an SD directory.', 'HANDLE-PUT': 'HANDLE-PUT h HEX: write bytes to a handle.',
    'HANDLE-GET': 'HANDLE-GET h n: read up to 64 bytes from a handle as hex.',
    'DEVICE': 'addr u xt DEVICE: serve /dev/NAME with a Forth word (0 removes).',
    'HANDLES': 'List open handles (files, pipes, /dev endpoints) and the last I/O error.',
    'WB-OPEN': 'Open the text workbench.',
    'WB-CONSOLE': 'Show the REPL console on VGA (home view; F1 toggles with the editor).',
    'WB-CONSOLE-CLEAR': 'Clear the console view text and its captured output.',
    'WB-CONSOLE?': 'Console view state: open, home view, and the renderer (esp).', 'WB-CLOSE': 'Close the workbench, keeping its draft.',
    'WB-STATUS': 'Workbench draft revision and flags.', 'WB-EDIT': 'WB-EDIT module discard: load active module source.',
    'WB-NEW': 'WB-NEW module version discard: new draft.', 'WB-PATCH': 'WB-PATCH rev offset remove hex: revision-checked edit.',
    'WB-META': 'WB-META rev version test|-: set draft metadata.', 'WB-UNDO': 'WB-UNDO rev: restore the undo snapshot.',
    'WB-STAGE': 'WB-STAGE rev: validate and test the draft.', 'WB-ACTIVATE': 'WB-ACTIVATE rev: activate the staged draft.',
    'WB-SAVE': 'WB-SAVE rev: persist the active draft.', 'WB-SOURCE': 'WB-SOURCE offset count: draft bytes as hex.',
    'WB-VIEW': 'WB-VIEW row count: desired screen rows.', 'WB-KEY': 'WB-KEY code modifiers: inject a semantic key.',
    'WB-SELECT': 'WB-SELECT MODULE name | WORD index.', 'WB-BROWSE': 'WB-BROWSE MODULES | WORDS [module].',
    'WB-REDRAW': 'Mode reset and full repaint.', 'WB-REPAINT': 'Repaint without a mode switch.',
    'WB-ZOOM': 'WB-ZOOM 0|1: full-width source layout.', 'WB-ZOOM?': 'Show zoom state.',
    'WB-COMPOSE': 'Time one composition in 288 kHz ticks.', 'WB-FLUSH': 'Drain pending painting.',
    'WB-FLUSH-FENCED': 'Drain painting and wait for framebuffer completion.', 'WB-RENDER-STATS': 'WB-RENDER-STATS [RESET].',
}
COMPILER = {':': 'Begin a colon definition.', ';': 'End a definition.', "'": "' name: execution token.",
            "[']": 'Compile an execution token.', 'IF': 'flag IF ... [ELSE ...] THEN', 'ELSE': 'See IF.', 'THEN': 'See IF.',
            'BEGIN': 'BEGIN ... UNTIL | AGAIN | WHILE ... REPEAT', 'UNTIL': 'See BEGIN.', 'AGAIN': 'See BEGIN.',
            'WHILE': 'See BEGIN.', 'REPEAT': 'See BEGIN.', 'DO': 'limit start DO ... LOOP', 'LOOP': 'See DO.',
            'EXIT': 'Return from the current definition.', 'RECURSE': 'Call the current definition.',
            'CONSTANT': 'n CONSTANT name', 'VARIABLE': 'VARIABLE name', 'DEFER': 'DEFER name: replaceable binding.',
            'IS': "' word IS name: rebind a DEFER (callers follow it).",
            '?DO': 'limit start ?DO ... LOOP: skips the body when start = limit.', 'LEAVE': 'Exit the innermost DO loop now.',
            '+LOOP': 'n +LOOP: step by n; exits when crossing the limit boundary.',
            'FILL': 'addr u c FILL: set u bytes.', 'MOVE': 'src dst u MOVE: copy, overlap-safe.', 'U.': 'Print unsigned.',
            '."': '." text" prints text (S" text" TYPE).', 'S"': 'S" text" pushes addr u of a data-space string.',
            'CHAR': 'CHAR x pushes the code of x (interpretation only).', '[CHAR]': '[CHAR] x compiles the code of x.',
            'CREATE': 'CREATE name: word returning the next data address (interpretation only).',
            'ALLOT': 'n ALLOT reserves n zeroed data bytes (interpretation only).',
            'HERE': 'HERE pushes the next free data address (interpretation only).',
            ',': 'n , appends a cell to data space.', 'C,': 'c C, appends a byte to data space.'}
KINDS = {0: 'primitive', 1: 'colon', 2: 'constant', 3: 'variable', 4: 'defer', 5: 'native'}
NAME = re.compile(r'[!-~]{1,31}')


def module_graph(modules):
    """Dependency graph, reverse dependencies and activation order from inventory."""
    by_name = {m['name']: m for m in modules}
    edges = [{'from': m['name'], 'to': d['name'], 'crc32': d['crc32'],
              'satisfied': d['name'] in by_name and by_name[d['name']]['crc32'] == d['crc32']}
             for m in modules for d in m['dependencies']]
    dependents = {name: sorted(e['from'] for e in edges if e['to'] == name) for name in by_name}
    order, state = [], {}

    def visit(name, path):
        if state.get(name) == 'done' or name not in by_name:
            return
        if state.get(name) == 'active':
            raise IOError('Module dependency cycle: ' + ' -> '.join(path + [name]))
        state[name] = 'active'
        for d in by_name[name]['dependencies']:
            visit(d['name'], path + [name])
        state[name] = 'done'
        order.append(name)
    for name in by_name:
        visit(name, [])
    return {'nodes': sorted(by_name), 'edges': edges, 'dependents': dependents, 'activation_order': order,
            'unsatisfied': [e for e in edges if not e['satisfied']]}


class DevToolsClient:
    def complete(self, prefix='', limit=64):
        if not isinstance(prefix, str) or len(prefix) > 31 or (prefix and not NAME.fullmatch(prefix)):
            raise ValueError('prefix must be at most 31 printable ASCII bytes without spaces')
        if type(limit) is not int or not 1 <= limit <= 512:
            raise ValueError('limit must be 1..512')
        words = self.words()
        if not words.get('ok'):
            return words
        upper = prefix.upper()
        seen, matches = set(), []
        for kind, names in (('word', words['words']), ('command', COMMANDS), ('compiler', COMPILER)):
            for name in names:
                if name.upper().startswith(upper) and name not in seen:
                    seen.add(name)
                    matches.append({'name': name, 'kind': kind})
        matches.sort(key=lambda m: (len(m['name']), m['name']))
        return {'ok': True, 'prefix': prefix, 'matches': matches[:limit], 'truncated': len(matches) > limit,
                'common_prefix': _common([m['name'] for m in matches]) if matches else prefix}

    def help(self, name):
        if not isinstance(name, str) or not NAME.fullmatch(name):
            raise ValueError('name must be one printable ASCII token, at most 31 bytes')
        key = name.upper()
        for kind, table in (('command', COMMANDS), ('compiler', COMPILER)):
            if key in table:
                return {'ok': True, 'name': key, 'kind': kind, 'help': table[key]}
        result = self.inspect_word(name)
        if not result.get('ok') or 'word' not in result:
            return {'ok': False, 'name': name, 'error': 'unknown word or command', 'output': result.get('output', '')}
        info = result['word']
        lines = [line for line in result['output'].replace('\r', '').splitlines()
                 if line and not line.startswith(('SEE ', 'word ')) and line not in ('ok', 'vios> ')]
        return {'ok': True, 'name': info.get('name', name), 'kind': KINDS.get(info.get('kind'), 'unknown'),
                'word': info, 'see': '\n'.join(lines)}

    def develop(self, name, version, source, test_word='', depends_on=None, save=False):
        """One explicit edit -> test -> activate [-> save] cycle with per-step feedback.

        depends_on names live modules whose current CRCs are pinned. A failed
        stage is discarded; a failed activation leaves the previous generation
        live. Nothing is retried; each step's outcome is reported."""
        if depends_on is None:
            depends_on = []
        if not isinstance(depends_on, list) or len(depends_on) > 4 or type(save) is not bool:
            raise ValueError('depends_on must list at most four module names; save must be a boolean')
        with self.lock:
            steps = []

            def step(label, result):
                steps.append({'step': label, 'ok': bool(result.get('ok')), 'output': result.get('output', '')})
                return result.get('ok')
            inventory = self.modules()
            if not step('inventory', inventory):
                return {'ok': False, 'failed_step': 'inventory', 'steps': steps}
            live = {m['name']: m for m in inventory['modules'] if m['active']}
            missing = [n for n in depends_on if n not in live]
            if missing:
                return {'ok': False, 'failed_step': 'dependencies', 'steps': steps,
                        'error': 'dependency is not a live module: ' + ' '.join(missing)}
            dependencies = [{'name': n, 'crc32': live[n]['crc32']} for n in depends_on]
            previous = live.get(name, {}).get('crc32')
            staged = self.module_stage(name, version, source, test_word, dependencies)
            if not step('stage', staged):
                discard = self.module_discard()
                step('discard', discard)
                return {'ok': False, 'failed_step': 'stage', 'steps': steps, 'error': _error(staged)}
            if not step('activate', self.module_activate(name)):
                return {'ok': False, 'failed_step': 'activate', 'steps': steps, 'error': _error_line(steps[-1]['output'])}
            result = {'ok': True, 'name': name, 'version': version, 'crc32': staged['crc32'],
                      'replaced_crc32': previous, 'dependencies': dependencies, 'rollback_available': previous is not None,
                      'saved': False, 'steps': steps}
            if save:
                saved = self.module_save(name)
                if not step('save', saved):
                    result.update(ok=False, failed_step='save', error=_error_line(steps[-1]['output']))
                else:
                    result.update(saved=True, path=saved.get('path'))
            return result

    def describe(self, kind, identity=None):
        """Typed descriptor for one live object: kind, identity, generation.

        generation is the workbench catalog revision when the object was read;
        a client comparing it later can tell whether a stored reference may be
        stale (words and modules change identity across redefinition)."""
        kinds = ('word', 'module', 'task', 'handle', 'error')
        if kind not in kinds:
            raise ValueError('kind must be one of ' + ', '.join(kinds))
        with self.lock:
            status = self.wb_status()
            generation = status['workbench']['catalog'] if status.get('ok') else None
            if kind == 'word':
                if not isinstance(identity, str):
                    raise ValueError('word identity is its name')
                info = self.help(identity)
                body = info if info.get('ok') else None
            elif kind == 'module':
                inventory = self.modules()
                body = next((m for m in inventory.get('modules', []) if m['name'] == identity), None)
                if body is not None:
                    graph = module_graph(inventory['modules'])
                    body = dict(body, dependents=graph['dependents'].get(identity, []))
            elif kind == 'task':
                if type(identity) is not int:
                    raise ValueError('task identity is its id 1..4')
                body = next((t for t in self.tasks().get('tasks', []) if t['id'] == identity), None)
            elif kind == 'handle':
                if type(identity) is not int:
                    raise ValueError('handle identity is its number')
                body = next((h for h in self.handles().get('handles', []) if h['id'] == identity), None)
            else:
                body = self.last_error().get('last_error')
                identity = None if body is None else body['serial']
            return {'ok': body is not None, 'kind': kind, 'identity': identity, 'generation': generation,
                    'object': body, **({} if body is not None else {'error': f'no such {kind}'})}

    def module_graph(self):
        result = self.modules()
        if not result.get('ok'):
            return result
        return {'ok': True, **module_graph(result['modules'])}


def _error_line(output):
    for line in output.replace('\r', '').splitlines():
        if line.startswith('error: '):
            return line[7:]
    return 'failed'


def _error(result):
    return result.get('error') or _error_line(result.get('output', ''))


def _common(names):
    first, last = min(names), max(names)
    n = 0
    while n < min(len(first), len(last)) and first[n] == last[n]:
        n += 1
    return first[:n]

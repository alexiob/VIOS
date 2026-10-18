"""Background task inspection over the resident TASKS/TASK-OUT commands."""
import re

STATES = ('free', 'ready', 'sleeping', 'done', 'failed', 'killed')
LINE = re.compile(r'task (\d+) state (\w+) xt (\d+) name (\S+) slices (\d+) steps (\d+) depth (\d+) rdepth (\d+) '
                  r'ds-peak (\d+) rs-peak (\d+) out (\d+) dropped (\d+) error (\S+)')


def _hex_text(value):
    if value == '-':
        return ''
    if not re.fullmatch(r'(?:[0-9A-F]{2})+', value):
        raise IOError('Malformed task hex field')
    return bytes.fromhex(value).decode('ascii', 'replace')


def parse_tasks(output):
    rows = []
    for line in output.replace('\r', '').splitlines():
        if line.startswith('task ') and not line.startswith(('task-out', 'task-slice')):
            m = LINE.fullmatch(line.strip())
            if not m or m[2] not in STATES:
                raise IOError('Malformed task line: ' + line)
            n = [int(m[i]) for i in (1, 3, 5, 6, 7, 8, 9, 10, 11, 12)]
            rows.append({'id': n[0], 'state': m[2], 'xt': n[1], 'name': _hex_text(m[4]), 'slices': n[2], 'steps': n[3],
                         'depth': n[4], 'rdepth': n[5], 'ds_peak': n[6], 'rs_peak': n[7], 'output_bytes': n[8],
                         'output_dropped': n[9], 'error': _hex_text(m[13])})
    return rows


HANDLE = re.compile(r'handle (\d+) kind (\w+) (read|write) owner (\d+) bytes (\d+)(?: buffered (\d+))? path (\S+)')


def parse_handles(output):
    rows, error = [], None
    for line in output.replace('\r', '').splitlines():
        line = line.strip()
        if line.startswith('handle '):
            m = HANDLE.fullmatch(line)
            if not m:
                raise IOError('Malformed handle line: ' + line)
            rows.append({'id': int(m[1]), 'kind': m[2], 'direction': m[3], 'owner': int(m[4]), 'bytes': int(m[5]),
                         'buffered': None if m[6] is None else int(m[6]), 'path': m[7]})
        elif line.startswith('io-error '):
            error = None if line == 'io-error -' else line[9:]
    return rows, error


class TaskClient:
    def handles(self):
        result = self._module_commands(['HANDLES'])
        if result.get('ok'):
            result['handles'], result['last_error'] = parse_handles(result.get('output', ''))
        return result

    def tasks(self):
        result = self._module_commands(['TASKS'])
        if result.get('ok'):
            result['tasks'] = parse_tasks(result.get('output', ''))
        return result

    def task_output(self, id):
        if type(id) is not int or not 1 <= id <= 4:
            raise ValueError('task id must be 1..4')
        result = self._module_commands([f'TASK-OUT {id}'])
        if result.get('ok'):
            m = re.search(r'task-out (\d+) hex (\S+) dropped (\d+)', result.get('output', ''))
            if not m or int(m[1]) != id:
                raise IOError('Malformed task output response')
            data = b'' if m[2] == '-' else bytes.fromhex(m[2])
            result.update(id=id, text=data.decode('ascii', 'replace'), hex=data.hex(), dropped=int(m[3]))
        return result


ERROR = re.compile(r'last-error serial (\d+) engine (\w+) stale ([01]) base (\d+) message (\S+) ip (\d+) word (\S+) offset (\d+)(?: src (\S+))? '
                   r'ds-depth (\d+) rdepth (\d+) ldepth (\d+) ds((?: \d+)*)')
FRAME = re.compile(r'frame (\d+) word (\S+) offset (\d+)(?: src (\S+))?')


def parse_last_error(output):
    lines = [l.strip() for l in output.replace('\r', '').splitlines()]
    head = next((l for l in lines if l.startswith('last-error serial ')), None)
    if head is None:
        raise IOError('Missing LAST-ERROR record')
    if head == 'last-error serial 0':
        return None
    m = ERROR.fullmatch(head)
    if not m:
        raise IOError('Malformed LAST-ERROR record')
    src = lambda v: None if v in (None, '-', '?') else v
    frames = [{'index': int(f[1]), 'word': _hex_text(f[2]), 'offset': int(f[3]), 'source_token': src(f[4])}
              for f in (FRAME.fullmatch(l) for l in lines if l.startswith('frame ')) if f]
    return {'serial': int(m[1]), 'engine': m[2], 'stale': m[3] == '1', 'base': int(m[4]), 'message': _hex_text(m[5]),
            'ip': int(m[6]), 'word': _hex_text(m[7]), 'offset': int(m[8]), 'source_token': src(m[9]), 'ds_depth': int(m[10]),
            'rdepth': int(m[11]), 'ldepth': int(m[12]), 'ds_top_first': [int(x) for x in m[13].split()], 'frames': frames}


class ErrorClient:
    def last_error(self):
        result = self._module_commands(['LAST-ERROR'])
        if result.get('ok'):
            result['last_error'] = parse_last_error(result.get('output', ''))
        return result


STEP = re.compile(r'step (\d+) word (\S+)\+(\d+)(?: src (\S+))? depth (\d+) rdepth (\d+)(?: top (-?\d+))?')


def parse_trace(output):
    steps, outcome = [], None
    for line in output.replace('\r', '').splitlines():
        line = line.strip()
        m = STEP.fullmatch(line)
        if m:
            steps.append({'step': int(m[1]), 'word': m[2], 'offset': int(m[3]), 'source_token': m[4] if m[4] not in (None, '-') else None,
                          'depth': int(m[5]), 'rdepth': int(m[6]), 'top': None if m[7] is None else int(m[7])})
        elif line.startswith('trace '):
            outcome = line.split()[1]
    return steps, outcome


class TraceClient:
    def trace(self, name, steps=200):
        if not isinstance(name, str) or not re.fullmatch(r'[!-~]{1,31}', name):
            raise ValueError('name must be one printable token')
        if type(steps) is not int or not 1 <= steps <= 100000:
            raise ValueError('steps must be 1..100000')
        result = self._module_commands([f'TRACE {name} {steps}'])
        result['steps'], result['outcome'] = parse_trace(result.get('output', ''))
        return result

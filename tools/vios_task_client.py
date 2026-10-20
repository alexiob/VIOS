"""Background task inspection over the resident TASKS/TASK-OUT commands."""
import hashlib
import re

STATES = ('free', 'ready', 'sleeping', 'done', 'failed', 'killed')
LINE = re.compile(r'task (\d+) state (\w+) xt (\d+) name (\S+) slices (\d+) steps (\d+) depth (\d+) rdepth (\d+) '
                  r'ds-peak (\d+) rs-peak (\d+) out (\d+) dropped (\d+) error (\S+)(?: base (\d+))?')


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
                         'output_dropped': n[9], 'error': _hex_text(m[13]),
                         'base': int(m[14]) if m[14] else None})
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


ENTRY = re.compile(r'entry (\S+) size (\d+) dir ([01])')
SD_NAME = re.compile(r'^/?(?:[A-Za-z0-9_~$!#%&@^\-]{1,8}(?:\.[A-Za-z0-9_~$!#%&@^\-]{1,3})?/)*[A-Za-z0-9_~$!#%&@^\-]{1,8}(?:\.[A-Za-z0-9_~$!#%&@^\-]{1,3})?$')


class FileServiceClient:
    """SD file service through handles: list, upload and download 8.3 files."""

    def _path(self, path):
        if not isinstance(path, str) or len(path) > 100 or not SD_NAME.fullmatch(path):
            raise ValueError('path must be an absolute 8.3 FAT path such as /VIOS/APP.FTH')
        return path if path.startswith('/') else '/' + path

    def _handle(self, path, mode):
        command = f'S" {path}" {mode} OPEN .'
        out = self._module_commands([command])
        if not out.get('ok'):
            raise IOError(out.get('output', '')[-200:])
        # The value follows the command's echo; background output (a task's
        # heartbeat) can come before the echo.
        lines = out['output'].replace('\r', '').split('\n')
        echo = next((i for i, line in enumerate(lines) if line.strip().endswith(command)), 0)
        value = int(lines[echo + 1].split()[0])
        if value < 0:
            raise IOError(f'OPEN {path} failed with {value}')
        return value

    def sd_list(self, path='/'):
        path = '/' if path == '/' else self._path(path)
        result = self._module_commands([f'DIR {path}'])
        if result.get('ok'):
            result['entries'] = [{'name': m[1], 'size': int(m[2]), 'directory': m[3] == '1'}
                                 for m in (ENTRY.fullmatch(l.strip()) for l in result['output'].replace('\r', '').split('\n')) if m]
        return result

    def sd_put(self, path, text=None, hex=None):
        path = self._path(path)
        if (text is None) == (hex is None):
            raise ValueError('give exactly one of text or hex')
        data = text.encode('ascii') if text is not None else bytes.fromhex(hex)
        if len(data) > 65536:
            raise ValueError('at most 64 KiB per upload')
        with self.lock:
            h = self._handle(path, 2)
            written = 0
            try:
                for offset in range(0, len(data), 100):
                    chunk = data[offset:offset + 100]
                    out = self._module_commands([f'HANDLE-PUT {h} {chunk.hex().upper()}'])
                    m = re.search(r'put (\d+)', out.get('output', ''))
                    if not out.get('ok') or not m or int(m[1]) != len(chunk):
                        raise IOError('short write: ' + out.get('output', '')[-120:])
                    written += len(chunk)
            finally:
                self._module_commands([f'{h} CLOSE DROP'])
            return {'ok': True, 'path': path, 'bytes': written, 'sha256': hashlib.sha256(data).hexdigest()}

    def sd_get(self, path, limit=65536):
        path = self._path(path)
        with self.lock:
            h = self._handle(path, 1)
            data = bytearray()
            try:
                while len(data) < limit:
                    out = self._module_commands([f'HANDLE-GET {h} 64'])
                    m = re.search(r'get ([0-9A-F]*|-)\r?\n', out.get('output', ''))
                    if not out.get('ok') or not m:
                        raise IOError('read failed: ' + out.get('output', '')[-120:])
                    if m[1] in ('-', ''):
                        break
                    data.extend(bytes.fromhex(m[1]))
            finally:
                self._module_commands([f'{h} CLOSE DROP'])
            return {'ok': True, 'path': path, 'bytes': len(data), 'sha256': hashlib.sha256(bytes(data)).hexdigest(),
                    'text': bytes(data).decode('ascii', 'replace'), 'hex': bytes(data).hex()}

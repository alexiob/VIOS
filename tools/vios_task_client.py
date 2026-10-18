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
        if line.startswith('task ') and not line.startswith('task-out'):
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

"""Parse resident TASKS/TASK-OUT output exactly as forth/system.c prints it."""
from pathlib import Path
import re
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from vios_task_client import TaskClient, parse_tasks
from vios_harness import TOOL_MAP


class Board(TaskClient):
    def __init__(self, binary):
        self.binary = binary

    def _module_commands(self, commands):
        source = ': PR 72 EMIT 105 EMIT ; : KK KEY ; \' PR SPAWN DROP \' KK SPAWN DROP PAUSE\n' + commands[0] + '\n'
        out = subprocess.run([str(self.binary)], input=source, text=True, capture_output=True, check=True, timeout=10).stdout
        return {'ok': 'ERR' not in out, 'output': out}


class TaskClientTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = ROOT / 'build/tests/forth-system'
        cls.binary.parent.mkdir(parents=True, exist_ok=True)
        sources = ['tests/forth_system_host.c', 'forth/system.c', 'forth/modules.c', 'forth/vm_reference.c', 'forth/vm_memory.c']
        subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-Iinclude', *[str(ROOT / p) for p in sources],
                        '-o', str(cls.binary)], check=True)

    def test_real_firmware_format(self):
        board = Board(self.binary)
        rows = board.tasks()['tasks']
        self.assertEqual([r['state'] for r in rows], ['done', 'failed', 'free', 'free'])
        self.assertEqual((rows[0]['name'], rows[0]['output_bytes']), ('PR', 2))
        self.assertEqual(rows[1]['error'], 'KEY unavailable in background task')
        self.assertEqual(board.task_output(1)['text'], 'Hi')
        with self.assertRaises(ValueError):
            board.task_output(5)
        self.assertIn('tasks', TOOL_MAP)
        self.assertIn('task_output', TOOL_MAP)

    def test_malformed_rejected(self):
        with self.assertRaises(IOError):
            parse_tasks('task 1 state weird xt 0')


if __name__ == '__main__':
    unittest.main()


class HandleParseTest(unittest.TestCase):
    def test_parse(self):
        from vios_task_client import parse_handles
        rows, error = parse_handles('HANDLES\r\nhandle 1 kind pipe read owner 0 bytes 3 buffered 0 path pipe\r\n'
                                    'handle 3 kind null write owner 2 bytes 0 path /dev/null\r\nio-error pipe empty\r\n')
        self.assertEqual([(r['id'], r['kind'], r['owner'], r['buffered']) for r in rows], [(1, 'pipe', 0, 0), (3, 'null', 2, None)])
        self.assertEqual(error, 'pipe empty')
        self.assertIn('handles', TOOL_MAP)


class LastErrorTest(unittest.TestCase):
    def test_parse_real_output_and_cli(self):
        from vios_task_client import parse_last_error
        import io
        import vios_cli
        binary = ROOT / 'build/tests/forth-system'
        out = subprocess.run([str(binary)], input=': INNER 1 0 / ;\n: OUTER 5 INNER ;\nOUTER\nLAST-ERROR\n', text=True,
                             capture_output=True, check=True, timeout=10).stdout
        e = parse_last_error(out)
        self.assertEqual((e['word'], e['message'], [f['word'] for f in e['frames']]), ('INNER', 'arithmetic error', ['OUTER']))
        self.assertEqual((e['source_token'], e['frames'][0]['source_token']), ('/', 'INNER'))
        self.assertIsNone(parse_last_error('last-error serial 0\r\n'))

        class Client:
            def tool(self, name, arguments=None):
                return {'ok': True, 'last_error': e}
        text = io.StringIO()
        console = vios_cli.Console(Client(), text, io.StringIO())
        console.connected = True
        console.line(':why')
        self.assertIn('error #1: arithmetic error in INNER+', text.getvalue())
        self.assertIn('at `/`', text.getvalue())
        self.assertIn('called from OUTER+', text.getvalue())
        self.assertIn('last_error', TOOL_MAP)


class TraceParseTest(unittest.TestCase):
    def test_parse_real_trace(self):
        from vios_task_client import parse_trace
        binary = ROOT / 'build/tests/forth-system'
        out = subprocess.run([str(binary)], input=': SQ 3 DUP * ;\nTRACE SQ 10\n', text=True, capture_output=True, check=True, timeout=10).stdout
        steps, outcome = parse_trace(out)
        self.assertEqual(outcome, 'returned')
        self.assertEqual([s['word'] for s in steps][:2], ['SQ', 'SQ'])
        self.assertIn('trace', TOOL_MAP)


class FakeSd(TaskClient):
    """Emulates DIR/OPEN/HANDLE-PUT/HANDLE-GET/CLOSE replies like the firmware."""
    import threading as _t
    lock = _t.RLock()

    def __init__(self):
        self.files, self.open, self.commands = {}, {}, []

    def _module_commands(self, commands):
        c = commands[0]
        self.commands.append(c)
        if c.startswith('DIR '):
            return {'ok': True, 'output': c + '\r\n' + ''.join(f'entry {n.split("/")[-1]} size {len(d)} dir 0\r\n' for n, d in self.files.items()) + 'ok\r\n'}
        m = re.match(r'S" (\S+)" ([12]) OPEN \.', c)
        if m:
            h = len(self.open) + 1
            self.open[h] = [m[1], 0]
            if m[2] == '2':
                self.files[m[1]] = b''
            return {'ok': True, 'output': f'{c}\r\n{h} \r\nok\r\n'}
        m = re.match(r'HANDLE-PUT (\d+) ([0-9A-F]+)', c)
        if m:
            path = self.open[int(m[1])][0]
            self.files[path] += bytes.fromhex(m[2])
            return {'ok': True, 'output': f'{c}\r\nput {len(m[2]) // 2}\r\nok\r\n'}
        m = re.match(r'HANDLE-GET (\d+) (\d+)', c)
        if m:
            state = self.open[int(m[1])]
            data = self.files[state[0]][state[1]:state[1] + int(m[2])]
            state[1] += len(data)
            return {'ok': True, 'output': f'{c}\r\nget {data.hex().upper() if data else "-"}\r\nok\r\n'}
        return {'ok': True, 'output': c + '\r\nok\r\n'}


class FileServiceTest(unittest.TestCase):
    def test_roundtrip_chunks_and_paths(self):
        from vios_task_client import FileServiceClient

        class Board(FileServiceClient, FakeSd):
            pass
        b = Board()
        text = 'X' * 250 + '\n: APP 1 ;\n'
        put = b.sd_put('/VIOS/APP.FTH', text=text)
        self.assertEqual(put['bytes'], len(text))
        self.assertEqual(sum(1 for c in b.commands if c.startswith('HANDLE-PUT')), 3)
        got = b.sd_get('/VIOS/APP.FTH')
        self.assertEqual((got['text'], got['sha256']), (text, put['sha256']))
        self.assertEqual(b.sd_list('/VIOS')['entries'], [{'name': 'APP.FTH', 'size': len(text), 'directory': False}])
        for bad in ('/VIOS/TOOLONGNAME.FTH', '/VIOS/A B.TXT', 'nope/../x'):
            with self.assertRaises(ValueError):
                b.sd_put(bad, text='x')
        for name in ('sd_list', 'sd_put', 'sd_get'):
            self.assertIn(name, TOOL_MAP)


class CliFileTest(unittest.TestCase):
    def test_ls_put_get(self):
        import io
        import tempfile
        import vios_cli
        from vios_task_client import FileServiceClient

        class Board(FileServiceClient, FakeSd):
            pass
        board = Board()

        class Client:
            def tool(self, name, arguments=None):
                return getattr(board, name)(**(arguments or {}))
        out, err = io.StringIO(), io.StringIO()
        console = vios_cli.Console(Client(), out, err)
        console.connected = True
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / 'app.fth'
            src.write_text(': APP 1 ;\n')
            console.line(f':put {src} /VIOS/APP.FTH')
            console.line(':ls /VIOS')
            console.line(':get /VIOS/APP.FTH')
            console.line(f':get /VIOS/APP.FTH {src}')
        text = out.getvalue()
        self.assertIn('Wrote 10 bytes to /VIOS/APP.FTH', text)
        self.assertIn('APP.FTH  10', text)
        self.assertIn(': APP 1 ;', text)
        self.assertIn('Not saved', err.getvalue())

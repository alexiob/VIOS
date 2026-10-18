"""Parse resident TASKS/TASK-OUT output exactly as forth/system.c prints it."""
from pathlib import Path
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

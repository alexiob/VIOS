"""Host-only completion, help and module graph checks. No serial."""
import io
from pathlib import Path
import sys
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from vios_devtools import COMMANDS, DevToolsClient, module_graph
from vios_board import ViosBoard
from vios_harness import TOOL_MAP, validate_arguments
import vios_cli


def mod(name, crc, deps=()):
    return {'name': name, 'version': '1', 'crc32': crc, 'active': True, 'previous': False,
            'dependencies': [{'name': n, 'crc32': c} for n, c in deps]}


class Fake(DevToolsClient):
    lock = threading.RLock()

    def words(self):
        return {'ok': True, 'words': ['DUP', 'DROP', 'LIVE1-D', 'LIVE1-C', 'WB-PAINT']}

    def inspect_word(self, name):
        if name != 'LIVE1-D':
            return {'ok': False, 'output': 'error: unknown word'}
        return {'ok': True, 'output': 'SEE LIVE1-D\r\n: LIVE1-D LIVE1-C 1+ ;\r\nword LIVE1-D xt 80 kind 1 token 9\r\n\r\nok\r\nvios> ',
                'word': {'name': 'LIVE1-D', 'xt': 80, 'kind': 1, 'token': 9}}

    def modules(self):
        return {'ok': True, 'modules': [mod('C', 3, [('A', 1), ('B', 9)]), mod('A', 1), mod('B', 2, [('A', 1)])]}


class GraphTest(unittest.TestCase):
    def test_order_dependents_and_unsatisfied(self):
        g = Fake().module_graph()
        self.assertEqual(g['activation_order'], ['A', 'B', 'C'])
        self.assertEqual(g['dependents'], {'A': ['B', 'C'], 'B': ['C'], 'C': []})
        self.assertEqual([(e['from'], e['to']) for e in g['unsatisfied']], [('C', 'B')])

    def test_cycle_detected(self):
        with self.assertRaisesRegex(IOError, 'cycle'):
            module_graph([mod('A', 1, [('B', 2)]), mod('B', 2, [('A', 1)])])


class CompletionHelpTest(unittest.TestCase):
    def test_complete_merges_sources(self):
        c = Fake().complete('LIVE1')
        self.assertEqual([m['name'] for m in c['matches']], ['LIVE1-C', 'LIVE1-D'])
        self.assertEqual(c['common_prefix'], 'LIVE1-')
        kinds = {m['name']: m['kind'] for m in Fake().complete('WB-')['matches']}
        self.assertEqual(kinds['WB-PAINT'], 'word')
        self.assertEqual(kinds['WB-OPEN'], 'command')
        self.assertTrue(Fake().complete('d', limit=1)['truncated'])
        with self.assertRaises(ValueError):
            Fake().complete('A B')

    def test_help(self):
        self.assertEqual(Fake().help('wb-open')['kind'], 'command')
        self.assertEqual(Fake().help('IF')['kind'], 'compiler')
        h = Fake().help('LIVE1-D')
        self.assertEqual((h['kind'], h['see']), ('colon', ': LIVE1-D LIVE1-C 1+ ;'))
        self.assertFalse(Fake().help('NOPE')['ok'])

    def test_commands_cover_firmware_parsers(self):
        root = Path(__file__).resolve().parents[1]
        import re
        parsed = set(re.findall(r'vios_same\(word,"([^"]+)"\)', ''.join(p.read_text() for p in (root / 'kernel').glob('*.c'))))
        self.assertEqual(sorted(parsed - set(COMMANDS)), [], 'document every outer command')


class SurfaceTest(unittest.TestCase):
    def test_tools_and_board(self):
        for name in ('complete', 'help', 'module_graph'):
            self.assertIn(name, TOOL_MAP)
            self.assertTrue(hasattr(ViosBoard, name))
        validate_arguments('help', {'name': 'DUP'})
        with self.assertRaises(Exception):
            validate_arguments('help', {})

    def test_cli(self):
        board = Fake()

        class Client:
            def tool(self, name, arguments=None):
                return getattr(board, name)(**(arguments or {}))
        out, err = io.StringIO(), io.StringIO()
        console = vios_cli.Console(Client(), out, err)
        console.connected = True
        console.line(':help LIVE1-D')
        console.line(':complete LIVE1')
        console.line(':graph')
        self.assertEqual(console.completer('LIVE1', 0), 'LIVE1-C')
        self.assertEqual(console.completer('LIVE1', 1), 'LIVE1-D')
        self.assertIsNone(console.completer('LIVE1', 2))
        text = out.getvalue()
        self.assertIn('LIVE1-D (colon): : LIVE1-D LIVE1-C 1+ ;', text)
        self.assertIn('LIVE1-C LIVE1-D', text)
        self.assertIn('order A B C', text)
        self.assertIn('C -> B 00000009 UNSATISFIED', text)
        self.assertEqual(err.getvalue(), '')


if __name__ == '__main__':
    unittest.main()


class DevelopFake(Fake):
    def __init__(self, stage_ok=True, activate_ok=True):
        self.stage_ok, self.activate_ok, self.calls = stage_ok, activate_ok, []

    def module_stage(self, name, version, source, test_word, dependencies):
        self.calls.append(('stage', dependencies))
        return {'ok': self.stage_ok, 'output': '' if self.stage_ok else 'error: test failed\r\n', 'crc32': 77}

    def module_discard(self):
        self.calls.append(('discard',))
        return {'ok': True}

    def module_activate(self, name):
        self.calls.append(('activate', name))
        return {'ok': self.activate_ok, 'output': '' if self.activate_ok else 'error: catalog changed\r\n'}

    def module_save(self, name):
        self.calls.append(('save', name))
        return {'ok': True, 'path': '/VIOS/X/Y.VIM'}


class DevelopTest(unittest.TestCase):
    def test_full_cycle_pins_live_crcs(self):
        board = DevelopFake()
        r = board.develop('B', '3', ': B ;', depends_on=['A'], save=True)
        self.assertTrue(r['ok'] and r['saved'])
        self.assertEqual(r['dependencies'], [{'name': 'A', 'crc32': 1}])
        self.assertEqual(r['replaced_crc32'], 2)
        self.assertEqual([s['step'] for s in r['steps']], ['inventory', 'stage', 'activate', 'save'])

    def test_stage_failure_discards_and_stops(self):
        board = DevelopFake(stage_ok=False)
        r = board.develop('N', '1', ': N ;')
        self.assertEqual((r['ok'], r['failed_step'], r['error']), (False, 'stage', 'test failed'))
        self.assertEqual([c[0] for c in board.calls], ['stage', 'discard'])

    def test_activation_failure_and_missing_dependency(self):
        r = DevelopFake(activate_ok=False).develop('N', '1', ': N ;')
        self.assertEqual((r['failed_step'], r['error']), ('activate', 'catalog changed'))
        board = DevelopFake()
        r = board.develop('N', '1', ': N ;', depends_on=['ZZ'])
        self.assertEqual(r['failed_step'], 'dependencies')
        self.assertEqual(board.calls, [], 'nothing is sent when a dependency is not live')
        self.assertIn('develop', TOOL_MAP)


class DescribeFake(Fake):
    def wb_status(self):
        return {'ok': True, 'workbench': {'catalog': 42}}

    def tasks(self):
        return {'ok': True, 'tasks': [{'id': 1, 'state': 'ready', 'name': 'X'}]}

    def handles(self):
        return {'ok': True, 'handles': []}

    def last_error(self):
        return {'ok': True, 'last_error': {'serial': 3, 'word': 'INNER'}}


class DescribeTest(unittest.TestCase):
    def test_kinds(self):
        b = DescribeFake()
        w = b.describe('word', 'LIVE1-D')
        self.assertEqual((w['kind'], w['identity'], w['generation'], w['object']['kind']), ('word', 'LIVE1-D', 42, 'colon'))
        m = b.describe('module', 'A')
        self.assertEqual(m['object']['dependents'], ['B', 'C'])
        self.assertEqual(b.describe('task', 1)['object']['name'], 'X')
        self.assertFalse(b.describe('handle', 1)['ok'])
        self.assertEqual(b.describe('error')['identity'], 3)
        with self.assertRaises(ValueError):
            b.describe('nonsense')
        self.assertIn('describe', TOOL_MAP)

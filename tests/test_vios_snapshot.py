"""Host-only snapshot composition, classification and surface checks. No serial."""
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from vios_snapshot import BASE_QUERY, SnapshotClient, classify_draft
from vios_board import ViosBoard
import vios_cli
from vios_harness import RpcServer, TOOL_MAP, validate_arguments
import vios_fast_display_state

SOURCES = {'CORE': ': CORE-V 1 ;', 'APP': ': APP-V CORE-V 1+ ;'}


def wb(**changes):
    state = {'revision': 7, 'catalog': 3, 'open': False, 'draft': True, 'dirty': False, 'staged': False,
             'active': True, 'saved': False, 'length': len(SOURCES['APP']), 'cursor': 0, 'selection': 0,
             'browser': 0, 'focus': 0, 'words': 90, 'words_max': 512, 'module': 'APP', 'version': '2',
             'test': '', 'error': ''}
    state.update(changes)
    return state


class Fake(SnapshotClient):
    def __init__(self, base='16', app_dep_crc=None, torn=False, wrong_crc=False, draft=True, depth='0', manifest=None):
        self.lock = threading.RLock()
        self.draft = draft
        self.base, self.torn, self.wrong_crc, self.status_calls = base, torn, wrong_crc, 0
        self.queries = []
        self.depth, self.manifest_calls = depth, 0
        crc = lambda name: zlib.crc32(SOURCES[name].encode())
        # Default: CORE is in the confirmed bundle, APP's live CRC only in the candidate.
        self.manifest = manifest if manifest is not None else {'ok': True, 'manifest': {
            'confirmed': {'slot': 'A', 'status': 'ok', 'error': None, 'crc32': 5, 'modules': [
                {'name': 'CORE', 'version': '1', 'crc32': crc('CORE'), 'package': 'ok'},
                {'name': 'APP', 'version': '1', 'crc32': crc('APP') ^ 1, 'package': 'ok'}]},
            'candidate': {'slot': 'B', 'status': 'ok', 'error': None, 'crc32': 6, 'modules': [
                {'name': 'CORE', 'version': '1', 'crc32': crc('CORE'), 'package': 'ok'},
                {'name': 'APP', 'version': '2', 'crc32': crc('APP'), 'package': 'ok'}]}}}
        self.app_dep_crc = zlib.crc32(SOURCES['CORE'].encode()) if app_dep_crc is None else app_dep_crc

    def wb_status(self):
        self.status_calls += 1
        changes = {} if self.draft else dict(draft=False, active=False, length=0, module='', version='')
        return {'ok': True, 'workbench': wb(revision=8 if self.torn and self.status_calls > 1 else 7, **changes)}

    def wb_source(self):
        return {'ok': True, 'workbench': wb(), 'source': SOURCES['APP']}

    def stacks(self):
        return {'ok': True, 'data_stack': {'depth_text': self.depth, 'values_text': []}}

    def resources(self):
        return {'ok': True, 'resources': {'engine': 'DIRECT', 'words': '90/512'}}

    def modules(self):
        crc = lambda name: zlib.crc32(SOURCES[name].encode())
        return {'ok': True, 'modules': [
            {'name': 'CORE', 'version': '1', 'crc32': crc('CORE'), 'bytes': 12, 'dependency_count': 0,
             'active': True, 'previous': False, 'dependencies': []},
            {'name': 'APP', 'version': '2', 'crc32': crc('APP'), 'bytes': 19, 'dependency_count': 1,
             'active': True, 'previous': True, 'dependencies': [{'name': 'CORE', 'crc32': self.app_dep_crc}]}]}

    def module_source(self, name, previous=False):
        source = SOURCES[name] + (' ( old )' if previous else '')
        crc = zlib.crc32(source.encode()) ^ (1 if self.wrong_crc else 0)
        return {'ok': True, 'name': name, 'previous': previous, 'source': source, 'crc32': crc}

    def boot_status(self):
        return {'ok': True, 'boot': {'confirmed': 'A', 'candidate': 'NONE', 'state': 'CLEAN', 'current': 'A', 'generation': 7}}

    def boot_manifest(self):
        self.manifest_calls += 1
        if isinstance(self.manifest, Exception):
            raise self.manifest
        return self.manifest

    def _query(self, source, timeout=15):
        self.queries.append(source)
        return {'ok': True, 'output': f'{source}\r\n{self.base} \r\n\r\nok\r\nvios> '}

    _body = staticmethod(ViosBoard._body)


class ClassifyTest(unittest.TestCase):
    def test_draft_lifecycle_table(self):
        cases = [(dict(draft=False), 'none'), (dict(dirty=True, staged=True, active=False), 'staged'),
                 (dict(saved=True), 'saved'), (dict(dirty=True), 'live-unsaved'), ({}, 'live'),
                 (dict(active=False, dirty=True), 'modified'), (dict(active=False), 'diverged')]
        for changes, expected in cases:
            self.assertEqual(classify_draft(wb(**changes)), expected, changes)


class SnapshotTest(unittest.TestCase):
    def test_composes_restorable_record_and_summary(self):
        board = Fake()
        record = board.snapshot()
        self.assertTrue(record['ok'])
        self.assertEqual(board.queries, [BASE_QUERY])
        self.assertEqual(record['base']['base'], 16)
        self.assertEqual(set(record['module_sources']), {'CORE', 'APP'})
        self.assertEqual(record['previous_sources'], {})
        summary = record['summary']
        self.assertEqual(summary['draft'], {'module': 'APP', 'state': 'live', 'revision': 7, 'bytes': 19})
        app = next(m for m in summary['modules'] if m['name'] == 'APP')
        self.assertTrue(app['dependencies_satisfied'] and app['rollback_available'] and app['live'])
        self.assertEqual(app['draft'], 'live')
        core = next(m for m in summary['modules'] if m['name'] == 'CORE')
        self.assertEqual((core['persisted'], core['in_candidate']), (True, True))
        self.assertEqual((app['persisted'], app['in_candidate']), (False, True), 'name alone is not enough; CRC must match')
        self.assertIsNone(summary['boot']['manifest_error'])
        self.assertIsNone(summary['pending_module_stage'])
        json.dumps(record, allow_nan=False)

    def test_persistence_unknown_unless_manifest_is_trustworthy(self):
        def persisted(record):
            return [(m['persisted'], m['in_candidate']) for m in record['summary']['modules']]
        old = Fake(manifest={'ok': False, 'output': 'error: unknown word: BOOT-MANIFEST\r\n'}).snapshot()
        self.assertEqual(persisted(old), [(None, None)] * 2)
        self.assertEqual(old['summary']['boot']['manifest_error'], 'BOOT-MANIFEST failed')
        busy = Fake(depth='1')
        record = busy.snapshot()
        self.assertEqual(busy.manifest_calls, 0, 'an unknown word would clear the data stack')
        self.assertIn('data stack', record['summary']['boot']['manifest_error'])
        self.assertEqual(persisted(Fake(manifest=IOError('Malformed boot manifest line')).snapshot()), [(None, None)] * 2)
        none = {'slot': 'NONE', 'status': 'none', 'error': None, 'crc32': None, 'modules': []}
        empty = Fake(manifest={'ok': True, 'manifest': {'confirmed': none, 'candidate': none}}).snapshot()
        self.assertEqual(persisted(empty), [(False, False)] * 2, 'no confirmed slot: nothing survives power-off')
        bad = Fake().manifest
        bad['manifest']['confirmed']['modules'][0]['package'] = 'unreadable'
        bad['manifest']['candidate'] = {'slot': 'B', 'status': 'error', 'error': 'boot bundle CRC mismatch', 'crc32': None, 'modules': []}
        record = Fake(manifest=bad).snapshot()
        self.assertEqual(persisted(record), [(None, None)] * 2, 'startup rejects a slot with an unverified package')
        self.assertEqual(record['summary']['boot']['manifest_error'],
                         'candidate B: boot bundle CRC mismatch; confirmed A: package CORE unreadable')

    def test_previous_sources_and_dependency_mismatch(self):
        record = Fake(app_dep_crc=1).snapshot(previous_sources=True)
        self.assertEqual(set(record['previous_sources']), {'APP'})
        app = next(m for m in record['summary']['modules'] if m['name'] == 'APP')
        self.assertFalse(app['dependencies_satisfied'])
        self.assertEqual(app['unsatisfied_dependencies'], ['CORE'])

    def test_fails_closed(self):
        with self.assertRaisesRegex(IOError, 'changed during snapshot'):
            Fake(torn=True).snapshot()
        with self.assertRaisesRegex(IOError, 'CRC'):
            Fake(wrong_crc=True).snapshot()
        with self.assertRaisesRegex(IOError, 'BASE'):
            Fake(base='zz').snapshot()
        with self.assertRaises(ValueError):
            Fake().snapshot(previous_sources=1)

    def test_restore_driver_reads_every_needed_field(self):
        empty = Fake(draft=False).snapshot()
        self.assertEqual((empty['source']['source'], empty['source']['bytes']), ('', 0))
        self.assertEqual(empty['summary']['draft']['state'], 'none')
        record = Fake().snapshot()
        # Mirror vios_fast_display_state.restore()'s accesses against this record.
        for m in record['modules']['modules']:
            if m['active']:
                self.assertIn('source', record['module_sources'][m['name']])
        target = record['status']['workbench']
        for key in ('module', 'dirty', 'cursor', 'version', 'test', 'browser', 'selection', 'focus', 'open', 'length'):
            self.assertIn(key, target)
        self.assertIsInstance(record['source']['source'], str)
        self.assertIn('engine', record['resources']['resources'])
        self.assertIn('boot', record['boot'])
        self.assertTrue(callable(vios_fast_display_state.restore))


class SurfaceTest(unittest.TestCase):
    def test_harness_tool(self):
        self.assertIn('snapshot', TOOL_MAP)
        validate_arguments('snapshot', {'previous_sources': True})
        with self.assertRaises(Exception):
            validate_arguments('snapshot', {'previous_sources': 'yes'})
        server = RpcServer(Fake())
        server.handle({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {
            'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {}}})
        reply = server.handle({'jsonrpc': '2.0', 'id': 2, 'method': 'tools/call',
                               'params': {'name': 'snapshot', 'arguments': {}}})
        self.assertFalse(reply['result']['isError'])
        self.assertEqual(json.loads(reply['result']['content'][0]['text'])['summary']['base'], 16)
        self.assertTrue(hasattr(ViosBoard, 'snapshot'))

    def test_cli_summary_and_exclusive_file(self):
        record = Fake().snapshot()

        class Client:
            def tool(self, name, arguments=None):
                assert name == 'snapshot'
                return record
        out, err = io.StringIO(), io.StringIO()
        console = vios_cli.Console(Client(), out, err)
        console.connected = True
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'state.json'
            console.line(':snapshot ' + str(path))
            self.assertEqual(json.loads(path.read_text())['summary'], record['summary'])
            console.line(':snapshot ' + str(path))
        text = out.getvalue()
        self.assertIn('engine DIRECT base 16 ds 0 boot A confirmed A CLEAN', text)
        self.assertIn('module APP 2 ', text)
        self.assertIn('rollback draft:live persisted:no candidate', text)
        self.assertIn('draft APP live rev 7 bytes 19', text)
        self.assertIn('Snapshot not written', err.getvalue(), 'existing files are never overwritten')


if __name__ == '__main__':
    unittest.main()

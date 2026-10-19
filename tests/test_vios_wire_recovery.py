"""Harness: read-only tools survive one lost USB frame; mutating tools fail closed."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from vios_harness import call_with_wire_recovery, READ_ONLY_RETRY


class FakeBoard:
    def __init__(self, losses=1, synchronized=True):
        self.losses, self.synchronized, self.calls, self.resyncs = losses, synchronized, [], 0

    def _maybe_lose(self, name):
        self.calls.append(name)
        if self.losses:
            self.losses -= 1
            raise IOError('VIOS wire integrity lost; explicitly redeploy')

    def snapshot(self):
        self._maybe_lose('snapshot')
        return {'modules': []}

    def evaluate(self, source):
        self._maybe_lose('evaluate')
        return {'ok': True}

    def resync(self):
        self.resyncs += 1
        return {'synchronized': self.synchronized, 'resynced': True}


class WireRecoveryTests(unittest.TestCase):
    def test_read_only_tool_resyncs_once_and_repeats_once(self):
        board = FakeBoard()
        result = call_with_wire_recovery(board, 'snapshot', {})
        self.assertEqual((result['wire_loss_recovered'], board.resyncs, board.calls), (True, 1, ['snapshot', 'snapshot']))

    def test_second_loss_is_reported_not_retried_again(self):
        board = FakeBoard(losses=2)
        with self.assertRaisesRegex(IOError, 'wire integrity lost'):
            call_with_wire_recovery(board, 'snapshot', {})
        self.assertEqual((board.resyncs, len(board.calls)), (1, 2))

    def test_mutating_tools_fail_closed_without_resync(self):
        board = FakeBoard()
        self.assertNotIn('evaluate', READ_ONLY_RETRY)
        with self.assertRaisesRegex(IOError, 'wire integrity lost'):
            call_with_wire_recovery(board, 'evaluate', {'source': '1 2 +'})
        self.assertEqual((board.resyncs, board.calls), (0, ['evaluate']))

    def test_failed_resync_keeps_the_original_error(self):
        board = FakeBoard(synchronized=False)
        with self.assertRaisesRegex(IOError, 'wire integrity lost'):
            call_with_wire_recovery(board, 'snapshot', {})
        self.assertEqual(board.calls, ['snapshot'])


if __name__ == '__main__':
    unittest.main()

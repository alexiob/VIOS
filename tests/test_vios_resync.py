"""Lost-frame resynchronization without reboot or replay. Never opens serial."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from vios_board import ViosBoard
from vios_harness import TOOL_MAP
from test_vios_harness import Session


def lost(tail):
    session = Session()
    board = ViosBoard(session=session)
    board.deployed = True
    # A merged physical key prefix made the board reject an untagged line.
    board.pending_nonce, board.pending_source, board.uncertain = 'D2CC1DC1', 'SEE X', True
    board.receive_buffer.extend(tail)
    return session, board


class ResyncTest(unittest.TestCase):
    def test_idle_prompt_resyncs_without_replay(self):
        session, board = lost(b"/'.L@D2CC1DC1 SEE X\r\nerror: unknown word: /'.L@D2CC1DC1\r\nvios> ")
        result = board.resync(quiet=0.01, timeout=5)
        self.assertTrue(result['ok'] and result['resynced'] and result['synchronized'])
        self.assertEqual(result['abandoned']['outcome'], 'unknown')
        self.assertEqual(result['abandoned']['source'], 'SEE X')
        self.assertEqual(len(session.commands), 1)
        self.assertEqual(session.commands[0][10:], '', 'only an empty framed line is sent')
        self.assertTrue(board.evaluate('1 2 +')['ok'])

    def test_busy_tail_refused(self):
        session, board = lost(b'partial program output')
        with self.assertRaisesRegex(RuntimeError, 'quiet idle prompt'):
            board.resync(quiet=0.01, timeout=5)
        self.assertEqual(session.commands, [])
        self.assertTrue(board.uncertain)

    def test_already_synchronized_is_noop(self):
        session = Session()
        board = ViosBoard(session=session)
        board.deployed = True
        self.assertFalse(board.resync(quiet=0.01)['resynced'])
        self.assertEqual(session.commands, [])

    def test_tool_registered(self):
        self.assertIn('resync', TOOL_MAP)


if __name__ == '__main__':
    unittest.main()

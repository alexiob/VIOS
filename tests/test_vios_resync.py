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



class EchoTest(unittest.TestCase):
    def test_stray_output_before_echo_keeps_text_and_strips_envelope(self):
        session = Session()
        board = ViosBoard(session=session)
        board.deployed = True
        board.receive_buffer.extend(b'from-task\r\n')
        result = board.evaluate('1 2 +')
        self.assertTrue(result['ok'])
        self.assertTrue(result['output'].startswith('from-task\r\n1 2 +\r\n'), result['output'])
        self.assertNotIn('@', result['output'].split('\r\n')[1])



class EventsTest(unittest.TestCase):
    def test_keyboard_text_becomes_an_event_and_idle_drain(self):
        session = Session()
        board = ViosBoard(session=session)
        board.deployed = True
        board.receive_buffer.extend(b'7 6 * .\r\n42 \r\nok\r\nvios> ')
        board.evaluate('1 2 +')
        board.receive_buffer.extend(b'from-task\r\n')
        events = board.events(quiet=0.01)['events']
        self.assertEqual([e['text'] for e in events], ['7 6 * .\r\n42 \r\nok\r\nvios> ', 'from-task\r\n'])
        self.assertEqual(board.events(quiet=0.01)['events'], [])
        self.assertIn('events', TOOL_MAP)



class WireLossTest(unittest.TestCase):
    def test_resync_rearms_after_wire_integrity_loss(self):
        session = Session()
        session.integrity_error = True
        board = ViosBoard(session=session)
        board.deployed = True
        board.receive_buffer.extend(b'garbled\r\nok\r\nvios> ')
        result = board.resync(quiet=0.01, timeout=5)
        self.assertTrue(result['synchronized'] and result['abandoned']['wire_integrity_lost'])
        self.assertFalse(session.integrity_error)
        self.assertTrue(board.evaluate('1 2 +')['ok'])


if __name__ == '__main__':
    unittest.main()

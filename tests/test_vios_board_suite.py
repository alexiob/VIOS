import pathlib, sys, unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import vios_board_suite as suite


class BoardSuiteTests(unittest.TestCase):
    def test_every_check_has_its_acceptance_tool(self):
        for name in suite.CHECKS + ['incremental']:
            self.assertTrue((ROOT / f'tools/vios_{name}_accept.py').exists(), name)

    def test_audio_is_part_of_the_default_suite(self):
        self.assertIn('audio', suite.CHECKS)
        self.assertNotIn('incremental', suite.CHECKS)   # replaces the workbench draft

    def test_unknown_checks_are_refused(self):
        with self.assertRaises(SystemExit):
            suite.main(['/tmp/unused', '--only', 'gui,nope'])


if __name__ == '__main__':
    unittest.main()

"""ESP console terminal (GUI Stage 1c) compiled natively against a fake screen."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / 'firmware/esp32-vios'


class EspConsoleTests(unittest.TestCase):
    def test_terminal_matches_former_cpu_view_and_draws_incrementally(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / 'console'
            subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I' + str(FIRMWARE / 'include'), str(FIRMWARE / 'tests/console.cpp'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('console terminal checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()

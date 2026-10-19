"""ESP cooked line editor (GUI Stage 2a) compiled natively against a terminal model."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / 'firmware/esp32-vios'


class EspLineTests(unittest.TestCase):
    def test_line_editor_matches_former_cpu_editor(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / 'line'
            subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I' + str(ROOT / 'include'), '-I' + str(FIRMWARE / 'include'),
                            str(FIRMWARE / 'tests/line.cpp'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('line editor checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()

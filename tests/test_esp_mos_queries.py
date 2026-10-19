"""MOS 3.0.2 boot queries: argument lengths against its real boot stream, VDP 2.16.0 reply formats."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / 'firmware/esp32-vios'


class MosQueryTests(unittest.TestCase):
    def test_mos3_boot_stream_stays_in_sync(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / 'mos_queries'
            subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I' + str(ROOT / 'include'), '-I' + str(FIRMWARE / 'include'),
                            str(FIRMWARE / 'tests/mos_queries.cpp'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('mos query checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()

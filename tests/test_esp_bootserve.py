"""ESP untethered-boot server: a stub model loads images through manifest and block bursts."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / 'firmware/esp32-vios'


class EspBootServeTests(unittest.TestCase):
    def test_stub_reassembles_images_and_refuses_corruption(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / 'bootserve'
            subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I' + str(ROOT / 'include'), '-I' + str(FIRMWARE / 'include'),
                            str(FIRMWARE / 'tests/bootserve.cpp'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('boot server checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()

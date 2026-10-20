"""Lean ESP audio engine (firmware/esp32-vios/include/vios_audio.h) compiled natively."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / 'firmware/esp32-vios'


class EspAudioTests(unittest.TestCase):
    def test_engine_timing_tunes_and_voices(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / 'audio'
            subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I' + str(FIRMWARE / 'include'), str(FIRMWARE / 'tests/audio.cpp'), '-o', str(binary)],
                           check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('audio engine checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()

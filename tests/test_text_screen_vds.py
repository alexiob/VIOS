"""The workbench on the ESP display server matches the VDU text screen exactly.

The same scripted workbench session runs against drivers/agon-vdp/text_screen.c
and against text_screen_vds.c driving the real display server in-process; every
checkpoint's 80x60 screen (characters and attributes) must be identical.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

try:
    from test_esp_vds import agon_font, VDS
except ImportError:
    from tests.test_esp_vds import agon_font, VDS

ROOT = Path(__file__).resolve().parents[1]
COMMON = ['tests/workbench_gui_native.c', 'kernel/workbench.c', 'forth/system.c', 'forth/modules.c',
          'forth/vm_reference.c', 'forth/vm_memory.c']
FLAGS = ['cc', '-std=c99', '-pedantic', '-Wall', '-Wextra', '-Werror', '-g',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Iinclude']


class TextScreenVdsTests(unittest.TestCase):
    def test_workbench_screens_identical_on_both_backends(self):
        with tempfile.TemporaryDirectory() as temp:
            text, gui, font = Path(temp) / 'text', Path(temp) / 'gui', Path(temp) / 'font.bin'
            font.write_bytes(agon_font())
            subprocess.run([*FLAGS, *COMMON, 'drivers/agon-vdp/text_screen.c', '-o', str(text)], cwd=ROOT, check=True)
            subprocess.run([*FLAGS, '-DVIOS_GUI', '-I' + str(VDS), *COMMON, 'drivers/agon-vdp/text_screen_vds.c',
                            str(VDS / 'vds.c'), str(VDS / 'vds_proto.c'), '-o', str(gui)], cwd=ROOT, check=True)
            expected = subprocess.run([str(text)], capture_output=True, text=True, timeout=120)
            actual = subprocess.run([str(gui), str(font)], capture_output=True, text=True, timeout=120)
            self.assertEqual(expected.returncode, 0, expected.stderr)
            self.assertEqual(actual.returncode, 0, actual.stderr)
            self.assertIn('workbench screen session done', actual.stdout)
            self.assertGreater(actual.stdout.count('== '), 9)
            for name, (want, got) in zip(expected.stdout.split('== ')[1:],
                                         zip(expected.stdout.split('== ')[1:], actual.stdout.split('== ')[1:])):
                self.assertEqual(want, got, 'screens differ at checkpoint ' + name.split('\n', 1)[0])

    def test_text_screen_vds_has_no_screen_planes(self):
        source = (ROOT / 'drivers/agon-vdp/text_screen_vds.c').read_text()
        self.assertNotIn('CELL_COUNT', source)


if __name__ == '__main__':
    unittest.main()

"""GUI stack end to end on the host: Forth UI words -> E4 bytes -> ESP display server and back."""
from pathlib import Path
import subprocess
import tempfile
import unittest

try:
    from test_esp_vds import agon_font, VDS
except ImportError:
    from tests.test_esp_vds import agon_font, VDS

ROOT = Path(__file__).resolve().parents[1]


class ViosUiTests(unittest.TestCase):
    def test_forth_words_drive_the_display_server(self):
        files = ['tests/vios_ui_host.c', 'kernel/vios_ui.c', 'forth/system.c', 'forth/modules.c',
                 'forth/vm_reference.c', 'forth/vm_memory.c']
        with tempfile.TemporaryDirectory() as temp:
            binary, font = Path(temp) / 'vios_ui', Path(temp) / 'font.bin'
            font.write_bytes(agon_font())
            subprocess.run(['cc', '-std=c99', '-pedantic', '-Wall', '-Wextra', '-Werror', '-g',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Iinclude', '-I' + str(VDS),
                            *files, str(VDS / 'vds.c'), str(VDS / 'vds_proto.c'), str(VDS / 'vds_ui.c'), str(VDS / 'vds_widget.c'), str(VDS / 'vds_text.c'), str(VDS / 'vds_files.c'),
                            '-o', str(binary)], cwd=ROOT, check=True)
            result = subprocess.run([str(binary), str(font)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('vios ui checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()

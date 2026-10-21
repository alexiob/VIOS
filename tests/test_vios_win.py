"""/dev/win and /dev/ui/tree on the host: handles over the real ESP display server, no CPU widget table."""
from pathlib import Path
import subprocess
import tempfile
import unittest

try:
    from test_esp_vds import agon_font, VDS
except ImportError:
    from tests.test_esp_vds import agon_font, VDS

ROOT = Path(__file__).resolve().parents[1]


class ViosWinTests(unittest.TestCase):
    def test_win_files_over_the_display_server(self):
        files = ['tests/vios_win_host.c', 'kernel/vios_win.c', 'kernel/vios_handles.c', 'kernel/vios_ui.c', 'forth/system.c',
                 'forth/modules.c', 'forth/vm_reference.c', 'forth/vm_memory.c']
        with tempfile.TemporaryDirectory() as temp:
            binary, font = Path(temp) / 'vios_win', Path(temp) / 'font.bin'
            font.write_bytes(agon_font())
            subprocess.run(['cc', '-std=c99', '-pedantic', '-Wall', '-Wextra', '-Werror', '-g',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Iinclude', '-I' + str(VDS),
                            *files, *(str(VDS / name) for name in ('vds.c', 'vds_proto.c', 'vds_canvas.c', 'vds_ui.c', 'vds_widget.c', 'vds_text.c', 'vds_files.c')),
                            '-o', str(binary)], cwd=ROOT, check=True)
            result = subprocess.run([str(binary), str(font)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('vios win checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()

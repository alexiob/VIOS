"""GUI development tools (kernel/vios_tools.c) end to end against the real ESP display server."""
from pathlib import Path
import subprocess
import tempfile
import unittest
try:
    from .test_esp_vds import agon_font
except ImportError:
    from test_esp_vds import agon_font

ROOT = Path(__file__).resolve().parents[1]
VDS = ROOT / 'firmware/esp32-vios/lib/vios-vds/src'


class ViosToolsTests(unittest.TestCase):
    def test_tools_browse_edit_evaluate_inspect_manage(self):
        files = ['tests/vios_tools_host.c', 'kernel/vios_ui.c', 'kernel/vios_tools.c', 'kernel/workbench.c', 'forth/system.c',
                 'forth/modules.c', 'forth/vm_reference.c', 'forth/vm_memory.c']
        with tempfile.TemporaryDirectory() as temp:
            binary, font = Path(temp) / 'vios_tools', Path(temp) / 'font.bin'
            font.write_bytes(agon_font())
            subprocess.run(['cc', '-std=c99', '-pedantic', '-Wall', '-Wextra', '-Werror', '-g',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Iinclude', '-I' + str(VDS),
                            *files, *(str(VDS / f) for f in ('vds.c', 'vds_proto.c', 'vds_ui.c', 'vds_widget.c', 'vds_text.c')),
                            '-o', str(binary)], cwd=ROOT, check=True)
            result = subprocess.run([str(binary), str(font)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('vios tools checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()

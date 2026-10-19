"""ESP display server core (GUI windows G1) compiled natively against a fake framebuffer."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / 'firmware/esp32-vios'
VDS = FIRMWARE / 'lib/vios-vds/src'


def agon_font():
    """The device's 8x8 font as 256 glyphs; the vendored bitmap starts at space."""
    text = (FIRMWARE / 'vendor/agon-vdp/video/agon_fonts.h').read_text(encoding='latin-1')
    body = text[text.index('FONT_AGON_BITMAP[]'):]
    body = body[body.index('{') + 1:body.index('};')]
    data = bytes(int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{2})', body))
    return bytes(32 * 8) + data[:(256 - 32) * 8]


class EspVdsTests(unittest.TestCase):
    def test_compositor_matches_full_render_after_any_sequence(self):
        with tempfile.TemporaryDirectory() as temp:
            binary, font = Path(temp) / 'vds', Path(temp) / 'font.bin'
            font.write_bytes(agon_font())
            subprocess.run(['cc', '-std=c99', '-pedantic', '-Wall', '-Wextra', '-Werror', '-g',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I' + str(VDS),
                            str(VDS / 'vds.c'), str(FIRMWARE / 'tests/vds.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary), str(font), temp], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('vds checks passed', result.stdout)
            for name in ('vds-smalltalk.ppm', 'vds-turbo.ppm'):
                self.assertEqual((Path(temp) / name).stat().st_size, len(b'P6\n640 480\n255\n') + 640 * 480 * 3)

    def test_wire_protocol_matches_api_and_rejects_malformed_batches(self):
        with tempfile.TemporaryDirectory() as temp:
            binary, font = Path(temp) / 'vds_proto', Path(temp) / 'font.bin'
            font.write_bytes(agon_font())
            subprocess.run(['cc', '-std=c99', '-pedantic', '-Wall', '-Wextra', '-Werror', '-g',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I' + str(VDS),
                            str(VDS / 'vds.c'), str(VDS / 'vds_proto.c'), str(FIRMWARE / 'tests/vds_proto.c'),
                            '-o', str(binary)], check=True)
            result = subprocess.run([str(binary), str(font)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('vds protocol checks passed', result.stdout)

    def test_core_is_freestanding(self):
        source = ''.join(path.read_text() for path in sorted(VDS.glob('*.[ch]')))
        self.assertLessEqual(set(re.findall(r'#include\s*[<"]([^>"]+)', source)),
                             {'vds.h', 'vds_proto.h', 'string.h', 'stddef.h', 'stdint.h'})
        for banned in ('malloc(', 'free(', 'printf('):
            self.assertNotIn(banned, source, 'the core must not allocate or print')


if __name__ == '__main__':
    unittest.main()

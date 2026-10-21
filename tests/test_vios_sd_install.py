"""tools/vios_sd_install.py on a temporary card directory (never a real card),
and the stub booting from what it installed."""
from pathlib import Path
import io
import json
import tempfile
import unittest
from contextlib import redirect_stdout
import zlib
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import vios_sd_install as sd
from test_viosboot_native import build, make_image, manifest, Esp, Stub, IMAGE


class SdInstallTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.stub_image = build(1)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        base = Path(self.temp.name)
        self.card = base/'card'
        (self.card/'VIOS').mkdir(parents=True)
        (self.card/'VIOS/BOOT_A.VIB').write_bytes(b'bundle')
        (self.card/'VIOS/boot.cnt').write_bytes(b'\x03')
        (self.card/'AUTOEXEC.TXT').write_bytes(b'SET KEYBOARD 1\r\nload console.bin\r\nrun\r\n')
        self.image = make_image(3000)
        self.resident = base/'vios-resident.bin'
        self.resident.write_bytes(self.image)
        self.resident.with_suffix('.gui').write_text('1\n')
        self.resident.with_suffix('.baud').write_text('1152000\n')
        self.stub = base/'viosboot.bin'
        self.stub.write_bytes(self.stub_image.raw)

    def tearDown(self):
        self.temp.cleanup()

    def run_tool(self, *args):
        out = io.StringIO()
        with redirect_stdout(out):
            code = sd.main([str(a) for a in args])
        return code, json.loads(out.getvalue())

    def install(self, *extra):
        return self.run_tool('install', self.card, '--resident', self.resident, '--stub', self.stub, *extra)

    def test_installs_the_layout_and_keeps_the_card(self):
        code, report = self.install()
        self.assertEqual(code, 0, report)
        self.assertEqual((self.card/'mos/viosboot.bin').read_bytes(), self.stub_image.raw)
        self.assertEqual((self.card/'VIOS/vios.bin').read_bytes(), self.image)          # into the existing VIOS folder
        self.assertEqual((self.card/'VIOS/vios.man').read_bytes(), manifest(self.image))  # = the ESP's 'M' packet
        self.assertEqual((self.card/'AUTOEXEC.TXT').read_bytes(), b'SET KEYBOARD 1\r\nload console.bin\r\nrun\r\nviosboot\r\n')
        self.assertEqual((self.card/'AUTOEXEC.TXT.vios-backup').read_bytes(), b'SET KEYBOARD 1\r\nload console.bin\r\nrun\r\n')
        self.assertEqual((self.card/'VIOS/BOOT_A.VIB').read_bytes(), b'bundle')
        self.assertFalse((self.card/'VIOS/boot.cnt').exists())
        self.assertFalse(list(self.card.rglob('*.tmp')))
        code, checked = self.run_tool('check', self.card, '--resident', self.resident)
        self.assertEqual((code, checked['problems']), (0, []))
        self.assertEqual(checked['manifest'], {'size': 3000, 'crc32': zlib.crc32(self.image), 'gui': 1, 'baud': 1152000})

    def test_a_release_tree_keeps_its_pairing(self):
        # A release ships sd/vios/vios.bin with its vios.man and no .gui/.baud sidecars.
        self.assertEqual(self.install()[0], 0)
        other = Path(self.temp.name)/'other'
        other.mkdir()
        code, report = self.run_tool('install', other, '--resident', self.card/'VIOS/vios.bin', '--stub', self.stub)
        self.assertEqual(code, 0, report)
        self.assertEqual((other/'vios/vios.man').read_bytes(), manifest(self.image))   # gui 1, not the sidecar default 0

    def test_installing_again_changes_nothing_else(self):
        self.install()
        code, report = self.install()
        self.assertEqual(code, 0)
        self.assertEqual((self.card/'AUTOEXEC.TXT').read_text().lower().count('viosboot'), 1)
        self.assertEqual((self.card/'AUTOEXEC.TXT.vios-backup').read_bytes(), b'SET KEYBOARD 1\r\nload console.bin\r\nrun\r\n')
        self.assertIn('AUTOEXEC.TXT', report['kept'])

    def test_a_card_without_autoexec(self):
        (self.card/'AUTOEXEC.TXT').unlink()
        self.assertEqual(self.install()[0], 0)
        self.assertEqual((self.card/'autoexec.txt').read_bytes(), b'viosboot\r\n')

    def test_an_obey_boot_file_gets_viosboot_too(self):
        # MOS 3 runs the first of !boot.obey, autoexec.obey, autoexec.txt: with
        # an obey file present, autoexec.txt alone would never start VIOS.
        (self.card/'autoexec.obey').write_bytes(b'SET KEYBOARD 1\r\n')
        (self.card/'!BOOT.OBEY').write_bytes(b'CD /bin\r\n')
        code, report = self.install()
        self.assertEqual((code, report['mos3_boot_file']), (0, '!BOOT.OBEY'))
        self.assertEqual((self.card/'!BOOT.OBEY').read_bytes(), b'CD /bin\r\nviosboot\r\n')
        self.assertEqual((self.card/'!BOOT.OBEY.vios-backup').read_bytes(), b'CD /bin\r\n')
        self.assertEqual((self.card/'autoexec.obey').read_bytes(), b'SET KEYBOARD 1\r\n')       # MOS 3 never reaches it
        self.assertIn(b'viosboot', (self.card/'AUTOEXEC.TXT').read_bytes())                   # MOS 1.x
        self.assertEqual(self.run_tool('check', self.card)[1]['problems'], [])
        (self.card/'!BOOT.OBEY').write_bytes(b'CD /bin\r\n')
        self.assertEqual(self.run_tool('check', self.card)[1]['problems'], ['!boot.obey does not run viosboot'])

    def test_no_obey_file_is_created(self):
        code, report = self.install()
        self.assertEqual((code, report['mos3_boot_file']), (0, 'AUTOEXEC.TXT'))
        self.assertFalse(any(p.name.lower().endswith('.obey') for p in self.card.iterdir()))

    def test_dry_run_writes_nothing(self):
        before = sorted((p.relative_to(self.card), p.read_bytes()) for p in self.card.rglob('*') if p.is_file())
        code, report = self.install('--dry-run')
        self.assertEqual(code, 0)
        self.assertTrue(report['dry_run'])
        self.assertEqual(sorted((p.relative_to(self.card), p.read_bytes()) for p in self.card.rglob('*') if p.is_file()), before)

    def test_check_finds_problems(self):
        self.install()
        bad = bytearray(self.image); bad[500] ^= 1
        (self.card/'VIOS/vios.bin').write_bytes(bytes(bad))
        code, report = self.run_tool('check', self.card)
        self.assertEqual(code, 1)
        self.assertTrue(any('does not match' in p for p in report['problems']))
        (self.card/'VIOS/vios.bin').write_bytes(self.image)
        (self.card/'AUTOEXEC.TXT').write_bytes(b'run\r\n')
        (self.card/'VIOS/boot.cnt').write_bytes(b'\x02')
        code, report = self.run_tool('check', self.card)
        self.assertEqual((code, report['problems'], report['unfinished_boots']), (1, ['autoexec.txt does not run viosboot'], 2))
        other = Path(self.temp.name)/'other.bin'; other.write_bytes(make_image(100))
        code, report = self.run_tool('check', self.card, '--resident', other)
        self.assertEqual(code, 1)
        self.assertIn(f'the card does not carry {other}', report['problems'])

    def test_refuses_bad_inputs(self):
        headless = Path(self.temp.name)/'raw.bin'; headless.write_bytes(b'\0' * 200)
        self.assertEqual(self.run_tool('install', self.card, '--resident', headless, '--stub', self.stub)[0], 2)
        self.assertEqual(self.run_tool('install', self.card, '--resident', self.resident, '--stub', headless)[0], 2)
        self.assertEqual(self.run_tool('install', Path(self.temp.name)/'no-card', '--resident', self.resident, '--stub', self.stub)[0], 2)
        self.assertFalse((self.card/'mos').exists())

    def test_the_stub_boots_what_was_installed(self):
        self.install()
        files = {'/vios/vios.man': (self.card/'VIOS/vios.man').read_bytes(), '/vios/vios.bin': (self.card/'VIOS/vios.bin').read_bytes()}
        cpu = Stub(self.stub_image, Esp(self.image, silent=True), files)
        self.assertEqual(cpu.run_stub(), 'vios', cpu.console.decode(errors='replace'))
        self.assertEqual(bytes(cpu.mem[IMAGE:IMAGE+len(self.image)]), self.image)


if __name__ == '__main__':
    unittest.main()

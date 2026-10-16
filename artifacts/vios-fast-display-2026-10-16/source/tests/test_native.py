"""Real portable C behavior, also checked under address/undefined sanitizers."""
import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class NativeTests(unittest.TestCase):
    def test_portable_monitor_and_vdp_frames(self):
        sources = [ROOT / p for p in (
            'tests/native.c', 'kernel/monitor.c', 'drivers/agon-vdp/keyboard.c')]
        for source in sources:
            self.assertTrue(source.exists(), f'production source missing: {source}')
        build = ROOT / 'build/tests'
        build.mkdir(parents=True, exist_ok=True)
        subprocess.run(['cc', '-std=c99', '-pedantic', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-g', '-Iinclude',
                        *(str(p) for p in sources), '-o', str(build / 'native')],
                       cwd=ROOT, check=True)
        result = subprocess.run([str(build / 'native')], capture_output=True, text=True,
                                check=True, timeout=10)
        self.assertIn('native behavior checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()

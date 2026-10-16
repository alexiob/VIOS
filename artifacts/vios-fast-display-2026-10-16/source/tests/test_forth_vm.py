"""Execute the real 24-bit VM contract under address/undefined sanitizers."""
import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class ForthVMTests(unittest.TestCase):
    def test_vm_semantics_and_memory_bounds(self):
        sources = [ROOT / name for name in (
            'forth/vm_reference.c', 'forth/vm_memory.c', 'tests/forth_vm_native.c')]
        for source in sources:
            self.assertTrue(source.exists(), f'VM implementation missing: {source}')
        build = ROOT / 'build/tests'
        build.mkdir(parents=True, exist_ok=True)
        executable = build / 'forth-vm-native'
        subprocess.run([
            'cc', '-std=c99', '-pedantic', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g',
            '-Iinclude', *(str(p) for p in sources), '-o', str(executable)
        ], cwd=ROOT, check=True)
        result = subprocess.run([str(executable)], cwd=ROOT, check=True,
                                capture_output=True, text=True, timeout=15)
        self.assertIn('Forth VM contract checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()

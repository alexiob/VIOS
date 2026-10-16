"""Structured metadata and isolated bounded observers against the real C VM."""
import pathlib
import subprocess
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class InspectTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.binary = ROOT / 'build/tests/forth-inspect'
  cls.binary.parent.mkdir(parents=True, exist_ok=True)
  subprocess.run(['cc', '-std=c99', '-pedantic', '-Wall', '-Wextra', '-Werror',
                  '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g', '-Iinclude',
                  'tests/forth_inspect_native.c', 'forth/system.c', 'forth/modules.c',
                  'forth/vm_reference.c', 'forth/vm_memory.c', '-o', str(cls.binary)], cwd=ROOT, check=True)
 def check_case(self, case):
  result = subprocess.run([str(self.binary), case], cwd=ROOT, capture_output=True, text=True, timeout=10)
  self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
 def test_metadata_and_module_source(self): self.check_case('metadata')
 def test_read_only_inspection_preserves_busy_vm(self): self.check_case('readonly')
 def test_catalog_revision_lifetime(self): self.check_case('revisions')
 def test_successful_observer_preserves_application(self): self.check_case('success')
 def test_failing_observer_preserves_application(self): self.check_case('errors')
 def test_observer_budget_limits_vm_execution(self): self.check_case('budget')
 def test_observer_requires_idle_vm(self): self.check_case('guards')
 def test_observer_is_nonreentrant(self): self.check_case('reentry')
 def test_repl_only_schedules_safe_idle_boundaries(self): self.check_case('repl')

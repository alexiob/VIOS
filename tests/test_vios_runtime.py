"""Resident services, package persistence and A/B startup using the real VM."""
import pathlib,subprocess,tempfile,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class RuntimeTests(unittest.TestCase):
 def test_resident_runtime_and_boot(self):
  files=['tests/vios_runtime_host.c','forth/system.c','forth/modules.c','forth/vm_reference.c','forth/vm_memory.c','kernel/vios_runtime.c','kernel/vios_services.c','kernel/vios_prelude.c','kernel/vios_handles.c','kernel/vios_ui.c','kernel/vios_modules.c','kernel/vios_boot.c','kernel/workbench.c','kernel/workbench_runtime.c','drivers/agon-vdp/text_screen.c']
  for name in files:self.assertTrue((ROOT/name).exists(),name+' missing')
  binary=ROOT/'build/tests/vios-runtime';binary.parent.mkdir(parents=True,exist_ok=True)
  subprocess.run(['cc','-std=c99','-pedantic','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-g','-Iinclude',*files,'-o',str(binary)],cwd=ROOT,check=True)
  with tempfile.TemporaryDirectory() as tmp:
   result=subprocess.run([str(binary),tmp],cwd=ROOT,capture_output=True,text=True,timeout=20)
   self.assertEqual(result.returncode,0,result.stdout+result.stderr)
   self.assertIn('Runtime packages and A/B startup checks passed',result.stdout)

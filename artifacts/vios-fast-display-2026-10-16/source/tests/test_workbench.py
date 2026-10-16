"""Exercise the real portable editor/compiler under address/UB sanitizers."""
import pathlib
import subprocess
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class WorkbenchTests(unittest.TestCase):
 def test_real_workbench(self):
  binary = ROOT/'build/tests/workbench'
  binary.parent.mkdir(parents=True, exist_ok=True)
  sources = ['tests/workbench_native.c', 'kernel/workbench.c', 'forth/system.c',
             'forth/modules.c', 'forth/vm_reference.c', 'forth/vm_memory.c']
  subprocess.run(['cc','-std=c99','-pedantic','-Wall','-Wextra','-Werror',
                  '-fsanitize=address,undefined','-fno-sanitize-recover=all','-g',
                  '-Iinclude',*sources,'-o',str(binary)],cwd=ROOT,check=True)
  result = subprocess.run([str(binary)],capture_output=True,text=True,timeout=30)
  self.assertEqual(result.returncode,0,result.stdout+result.stderr)
  self.assertIn('Workbench checks passed',result.stdout)

 def test_real_renderer_scroll_and_zoom(self):
  binary = ROOT/'build/tests/workbench-screen'
  binary.parent.mkdir(parents=True, exist_ok=True)
  sources = ['tests/workbench_screen_native.c', 'kernel/workbench.c',
             'drivers/agon-vdp/text_screen.c', 'forth/system.c', 'forth/modules.c',
             'forth/vm_reference.c', 'forth/vm_memory.c']
  subprocess.run(['cc','-std=c99','-pedantic','-Wall','-Wextra','-Werror',
                  '-fsanitize=address,undefined','-fno-sanitize-recover=all','-g',
                  '-Iinclude',*sources,'-o',str(binary)],cwd=ROOT,check=True)
  result = subprocess.run([str(binary)],capture_output=True,text=True,timeout=30)
  self.assertEqual(result.returncode,0,result.stdout+result.stderr)
  self.assertIn('source oracle and wire framebuffer checks passed',result.stdout)

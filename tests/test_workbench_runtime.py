"""Workbench ASCII controller against the real VM, model, renderer and storage."""
import pathlib, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class WorkbenchRuntimeTests(unittest.TestCase):
 def test_real_runtime_protocol_and_idle(self):
  files=['tests/workbench_runtime_native.c','forth/system.c','forth/modules.c','forth/vm_reference.c','forth/vm_memory.c','kernel/vios_runtime.c','kernel/vios_services.c','kernel/vios_audio.c','kernel/vios_prelude.c','kernel/vios_handles.c','kernel/vios_win.c','kernel/vios_ui.c','kernel/vios_tools.c','kernel/vios_modules.c','kernel/vios_boot.c','kernel/workbench.c','kernel/workbench_runtime.c','drivers/agon-vdp/text_screen.c']
  binary=ROOT/'build/tests/workbench-runtime';binary.parent.mkdir(parents=True,exist_ok=True)
  subprocess.run(['cc','-std=c99','-pedantic','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-g','-Iinclude',*files,'-o',str(binary)],cwd=ROOT,check=True)
  with tempfile.TemporaryDirectory() as tmp:
   result=subprocess.run([str(binary),tmp],cwd=ROOT,capture_output=True,text=True,timeout=20)
  self.assertEqual(result.returncode,0,result.stdout+result.stderr)
  self.assertIn('WB-STATUS revision ',result.stdout)
  self.assertIn('WB-SOURCE revision ',result.stdout)
  self.assertIn('hex 3A2056414C5545203432203B',result.stdout)
  self.assertIn('WB-RENDER-STATS cells 0 spans 0 batches 0 bytes 0 elapsed_ticks 0',result.stdout)
  self.assertIn('WB-COMPOSE elapsed_ticks 1 timer_bits 16 timer_hz 288000',result.stdout)
  self.assertIn('Workbench runtime checks passed',result.stdout)

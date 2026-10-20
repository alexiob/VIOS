import pathlib, subprocess, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class ForthPreemptTests(unittest.TestCase):
 def test_tick_preemption_on_reference_vm(self):
  binary=ROOT/'build/tests/forth-preempt';binary.parent.mkdir(parents=True,exist_ok=True)
  sources=['tests/forth_preempt_host.c','forth/system.c','forth/modules.c','forth/vm_reference.c','forth/vm_memory.c']
  subprocess.run(['cc','-std=c99','-pedantic','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-Iinclude',*[str(ROOT/p) for p in sources],'-o',str(binary)],check=True)
  out=subprocess.run([str(binary)],capture_output=True,text=True,timeout=60)
  self.assertEqual(out.returncode,0,out.stdout+out.stderr)
  self.assertIn('tick preemption: foreground rounds, determinism, CRITICAL and slice ends passed',out.stdout)
if __name__=='__main__':unittest.main()

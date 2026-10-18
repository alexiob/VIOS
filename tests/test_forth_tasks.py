import pathlib, subprocess, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class ForthTaskTests(unittest.TestCase):
 def test_background_tasks_on_reference_vm(self):
  binary=ROOT/'build/tests/forth-tasks';binary.parent.mkdir(parents=True,exist_ok=True)
  sources=['tests/forth_tasks_host.c','forth/system.c','forth/modules.c','forth/vm_reference.c','forth/vm_memory.c']
  subprocess.run(['cc','-std=c99','-pedantic','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-Iinclude',*[str(ROOT/p) for p in sources],'-o',str(binary)],check=True)
  out=subprocess.run([str(binary)],capture_output=True,text=True,check=True,timeout=20).stdout
  self.assertIn('background tasks: preemption, yield, sleep, isolation, output rings and guards passed',out)
if __name__=='__main__':unittest.main()

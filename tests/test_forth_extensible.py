"""The extensible compiler: IMMEDIATE, POSTPONE, [ ], LITERAL, STATE, FIND,
COMPILE,, run-time CREATE with DOES> and >BODY, EVALUATE and MARKER, with the
real compiler, module transactions, snapshots, tasks and observers."""
import pathlib
import subprocess
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class ExtensibleCompilerTests(unittest.TestCase):
 def test_extensible_compiler(self):
  files=['tests/forth_extensible_native.c','forth/system.c','forth/modules.c','forth/vm_reference.c','forth/vm_memory.c']
  binary=ROOT/'build/tests/forth-extensible'
  binary.parent.mkdir(parents=True,exist_ok=True)
  subprocess.run(['cc','-std=c99','-pedantic','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-g','-Iinclude',*files,'-o',str(binary)],cwd=ROOT,check=True)
  result=subprocess.run([str(binary)],cwd=ROOT,capture_output=True,text=True,timeout=30)
  self.assertEqual(result.returncode,0,result.stdout+result.stderr)
  self.assertIn('Extensible compiler checks passed',result.stdout)
if __name__=='__main__':
 unittest.main()

"""Atomic source-module activation using the real compiler and checked VM."""
import pathlib
import subprocess
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class ModuleTests(unittest.TestCase):
 def test_transactional_modules(self):
  files=['tests/forth_modules_native.c','forth/system.c','forth/modules.c','forth/vm_reference.c','forth/vm_memory.c']
  for name in files:self.assertTrue((ROOT/name).exists(),name+' missing')
  binary=ROOT/'build/tests/forth-modules'
  binary.parent.mkdir(parents=True,exist_ok=True)
  subprocess.run(['cc','-std=c99','-pedantic','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-g','-Iinclude',*files,'-o',str(binary)],cwd=ROOT,check=True)
  result=subprocess.run([str(binary)],cwd=ROOT,capture_output=True,text=True,timeout=15)
  self.assertEqual(result.returncode,0,result.stdout+result.stderr)
  self.assertIn('Module transaction checks passed',result.stdout)

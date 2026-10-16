import pathlib, subprocess, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class ForthSystemTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.binary=ROOT/'build/tests/forth-system'
  cls.binary.parent.mkdir(parents=True,exist_ok=True)
  sources=['tests/forth_system_host.c','forth/system.c','forth/vm_reference.c','forth/vm_memory.c']
  subprocess.run(['cc','-std=c99','-pedantic','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-Iinclude',*[str(ROOT/p) for p in sources],'-o',str(cls.binary)],check=True)
 def run_forth(self,source):
  return subprocess.run([str(self.binary)],input=source,text=True,capture_output=True,check=True,timeout=10).stdout.replace('\r','')
 def test_definitions_nested_calls_and_control_flow(self):
  out=self.run_forth(': SQUARE DUP * ;\n: ABS DUP 0 < IF NEGATE THEN ;\n-7 ABS SQUARE .\n: SUM 0 SWAP 0 DO I + LOOP ;\n10 SUM .\n: COUNTDOWN BEGIN 1- DUP 0= UNTIL ;\n12 COUNTDOWN .\n')
  self.assertNotIn('ERR',out);self.assertIn('49 ',out);self.assertIn('45 ',out);self.assertIn('0 ',out)
 def test_loop_index_survives_nested_calls(self):
  out=self.run_forth(': INNER 0 SWAP 0 DO I + LOOP ; : NEST 0 6 1 DO I INNER + LOOP ; NEST . CR\n')
  self.assertNotIn('ERR',out);self.assertIn('20 ',out)
 def test_resource_report_tracks_transient_stack_peaks(self):
  out=self.run_forth(': DEEP 1 DUP DUP DUP 2DROP 2DROP ; DEEP RESOURCES\n')
  self.assertIn('data-peak 4',out);self.assertIn('return-peak 1',out)
 def test_sync_is_state_neutral_even_during_compilation(self):
  out=self.run_forth('HEX 2A\nSYNC A1B2C3D4\n: LIVE\nSYNC ABCD1234\n1 + ;\nLIVE . CR\n')
  self.assertNotIn('ERR',out);self.assertIn('@A1B2C3D4',out);self.assertIn('@ABCD1234',out);self.assertIn('2B ',out)
 def test_metadata_radix_is_fixed_decimal(self):
  out=self.run_forth('HEX SEE DUP RESOURCES\n')
  self.assertIn('words 45/128',out);self.assertIn('word DUP xt 1 kind 0',out)
 def test_tagged_repl_completion_and_key_input(self):
  source='@ABCDEF12 : LIVE\r@1234ABCD KEY ;\r@00000001 LIVE .\rZ@00000002 SEE LIVE\r@00000003 MISSING\r'
  out=subprocess.run([str(self.binary),'--repl'],input=source,text=True,capture_output=True,check=True,timeout=10).stdout.replace('\r','')
  self.assertIn('@ABCDEF12\nok\ncompile> ',out)
  self.assertIn('90 \n@00000001\nok\nvios> ',out)
  self.assertIn(': LIVE KEY ;',out)
  self.assertIn('@00000003\nerror\nvios> ',out)
 def test_live_deferred_binding_changes_existing_caller(self):
  out=self.run_forth(': FIRST 11 ;\n: SECOND 22 ;\nDEFER SERVICE\n\' FIRST IS SERVICE\n: CLIENT SERVICE . ;\nCLIENT\n\' SECOND IS SERVICE\nCLIENT\n')
  self.assertNotIn('ERR',out);self.assertIn('11 ',out);self.assertIn('22 ',out)
 def test_errors_rollback_and_recover(self):
  out=self.run_forth(': GOOD 7 ;\n: BAD 1 UNKNOWN ;\nGOOD .\nBAD\nDROP\n3 4 + .\n: BROKEN IF ;\nWORDS\n')
  self.assertIn('ERR',out);self.assertIn('7 ',out)
  # A failed definition must not be published into WORDS.
  self.assertNotIn(' BROKEN ',out);self.assertIn('unknown word: BAD',out)
 def test_native_cell_memory_and_execution_tokens(self):
  out=self.run_forth('VARIABLE X\n123456 X ! X @ .\n$FFFFFF 1 + .\n: VALUE 123 ;\n\' VALUE EXECUTE .\nSEE VALUE\n.S\n')
  self.assertNotIn('ERR',out);self.assertIn('123456 ',out);self.assertIn('0 ',out);self.assertIn(': VALUE 123 ;',out)
 def test_multiline_compile_abort_and_resource_exhaustion(self):
  out=self.run_forth(': MULTI\n1 2 +\n;\nMULTI .\n: INCOMPLETE 55\nABORT\nINCOMPLETE\n'+''.join(': W%d %d ;\n'%(n,n) for n in range(300))+'\n2 3 + .\n')
  self.assertIn('3 ',out);self.assertIn('unknown word: INCOMPLETE',out);self.assertIn('dictionary full',out);self.assertTrue(out.rstrip().endswith('5'))

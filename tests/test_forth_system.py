import pathlib, subprocess, unittest, zlib
ROOT=pathlib.Path(__file__).resolve().parents[1]
class ForthSystemTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.binary=ROOT/'build/tests/forth-system'
  cls.binary.parent.mkdir(parents=True,exist_ok=True)
  sources=['tests/forth_system_host.c','forth/system.c','forth/modules.c','forth/vm_reference.c','forth/vm_memory.c']
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
  self.assertIn('data-peak 4',out);self.assertIn('return-peak 1',out);self.assertRegex(out,r'map-entries [1-9][0-9]*/[0-9]+')
 def test_sync_is_state_neutral_even_during_compilation(self):
  out=self.run_forth('HEX 2A\nSYNC A1B2C3D4\n: LIVE\nSYNC ABCD1234\n1 + ;\nLIVE . CR\n')
  self.assertNotIn('ERR',out);self.assertIn('@A1B2C3D4',out);self.assertIn('@ABCD1234',out);self.assertIn('2B ',out)
 def test_metadata_radix_is_fixed_decimal(self):
  out=self.run_forth('HEX SEE DUP RESOURCES\n')
  self.assertIn('words 65/512',out);self.assertIn('word DUP xt 1 kind 0',out)
 def test_dictionary_supports_more_than_128_live_words(self):
  source=''.join(': CAP%d %d ;\n'%(n,n) for n in range(350))
  out=self.run_forth(source+'CAP349 . RESOURCES\n')
  self.assertNotIn('ERR',out);self.assertIn('349 ',out)
  self.assertIn('words 415/512',out)
 def test_port_dictionary_budget_is_compile_time_configurable(self):
  binary=ROOT/'build/tests/forth-system-small'
  sources=['tests/forth_system_host.c','forth/system.c','forth/modules.c','forth/vm_reference.c','forth/vm_memory.c']
  subprocess.run(['cc','-std=c99','-pedantic','-Wall','-Wextra','-Werror',
   '-fsanitize=address,undefined','-DVIOS_FORTH_WORDS_MAX=69','-DVIOS_FORTH_HASH_BUCKETS=16',
   '-Iinclude',*[str(ROOT/p) for p in sources],'-o',str(binary)],check=True)
  source=''.join(': P%d %d ;\n'%(n,n) for n in range(20))+'P3 . RESOURCES\n2 3 + .\n'
  out=subprocess.run([str(binary)],input=source,text=True,capture_output=True,check=True,timeout=10).stdout
  self.assertIn('dictionary full',out);self.assertIn('words 69/69',out)   # 65 core words + P0..P3
  self.assertTrue(out.rstrip().endswith('5'))
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
 def test_string_literals_preserve_case_and_compile_source(self):
  out=self.run_forth('S" MiXeD" SWAP DUP C@ . 1+ C@ . . CR\n: GREET S" MiXeD" ; GREET SWAP C@ . . CR SEE GREET\nS" unterminated\n3 4 + .\n')
  self.assertIn('77 105 5 ',out);self.assertIn('77 5 ',out)
  self.assertIn(': GREET S" MiXeD" ;',out);self.assertIn('unterminated string',out);self.assertTrue(out.rstrip().endswith('7'))
 def test_multiline_compile_abort_and_resource_exhaustion(self):
  out=self.run_forth(': MULTI\n1 2 +\n;\nMULTI .\n: INCOMPLETE 55\nABORT\nINCOMPLETE\n'+''.join(': W%d %d ;\n'%(n,n) for n in range(600))+'\n2 3 + .\n')
  self.assertIn('3 ',out);self.assertIn('unknown word: INCOMPLETE',out);self.assertIn('dictionary full',out);self.assertTrue(out.rstrip().endswith('5'))

 def test_crc_envelope_rejects_altered_source_without_effects(self):
  def envelope(n,source,crc_source=None):
   crc=zlib.crc32((source if crc_source is None else crc_source).encode())
   return f'@{n:08X} !{crc:08X} {source}\r'
  source=envelope(1,': GOOD 7 ;')+envelope(2,': BAD 1 ;',': BAD 10 ;')+envelope(3,'GOOD .')+'@00000004 123 .\r'+envelope(5,'BAD')
  out=subprocess.run([str(self.binary),'--repl','--crc'],input=source,text=True,capture_output=True,check=True,timeout=10).stdout.replace('\r','')
  self.assertIn('request CRC mismatch',out);self.assertIn('7 \n@00000003\nok',out)
  self.assertIn('request CRC required',out);self.assertIn('unknown word: BAD',out)
class ErrorCaptureTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  ForthSystemTests.setUpClass()  # builds build/tests/forth-system independently of test order
 def test_last_error_keeps_context_lost_by_unwind(self):
  binary=ROOT/'build/tests/forth-system'
  out=subprocess.run([str(binary)],input='LAST-ERROR\n: INNER 1 0 / ;\n: MID 7 INNER 2 ;\n: OUTER 5 6 MID ;\nOUTER\nLAST-ERROR\n.S\nUNKNOWNWORD\nLAST-ERROR\n',text=True,capture_output=True,check=True,timeout=10).stdout.replace('\r','')
  self.assertIn('last-error serial 0\n',out)
  record=[l for l in out.splitlines() if l.startswith('last-error serial 1')]
  self.assertEqual(len(record),2,'compile/lookup errors do not replace the runtime record')
  self.assertIn('word '+b'INNER'.hex().upper()+' ',record[0]);self.assertIn('ds-depth 5 rdepth 2',record[0]);self.assertIn(' ds 0 1 7 6 5',record[0])
  self.assertIn('frame 0 word '+b'MID'.hex().upper(),out);self.assertIn('frame 1 word '+b'OUTER'.hex().upper(),out)
  self.assertIn('<0>',out)
 def test_trace_steps_every_instruction_and_attributes_budget_faults(self):
  binary=ROOT/'build/tests/forth-system'
  out=subprocess.run([str(binary)],input=': SQ DUP * ;\n: T 3 SQ 1+ ;\nTRACE T 50\n.S\nTRACE T 2\nLAST-ERROR\n: LOOPY BEGIN AGAIN ;\nLOOPY\nLAST-ERROR\nTRACE NOPE 3\n',text=True,capture_output=True,check=True,timeout=10).stdout.replace('\r','')
  steps=[l for l in out.splitlines() if l.startswith('step ')]
  self.assertEqual(steps[2],'step 3 word SQ+0 src DUP depth 1 rdepth 1 top 3 ')
  self.assertIn('trace returned steps 10',out);self.assertIn('<1> 10',out)
  self.assertIn('trace failed steps 2',out);self.assertIn('ERR: trace step limit reached',out)
  records=[l for l in out.splitlines() if l.startswith('last-error serial')]
  self.assertIn('word '+b'SQ'.hex().upper()+' offset 0',records[0])
  self.assertIn('word '+b'LOOPY'.hex().upper()+' offset 0',records[1])
  self.assertIn('ERR: unknown word: NOPE',out)
 def test_source_map_rolls_back_with_failed_definitions(self):
  binary=ROOT/'build/tests/forth-system'
  out=subprocess.run([str(binary)],input=': BAD 1 2 NOSUCH ;\n: GOOD 4 5 + ;\nTRACE GOOD 10\n',text=True,capture_output=True,check=True,timeout=10).stdout.replace('\r','')
  self.assertIn('ERR: unknown word: NOSUCH',out)
  srcs=[l.split(' src ')[1].split()[0] for l in out.splitlines() if l.startswith('step ')]
  self.assertEqual(srcs,['4','5','+',';'])
 def test_source_map_outlasts_a_long_session(self):
  binary=ROOT/'build/tests/forth-system'
  # 1200 entries past the prelude: more than the whole map held before (640, then 1024).
  defs=''.join(f': M{n} {n} 1 + 2 + 3 + DROP {n} DROP ;\n' for n in range(120))
  out=subprocess.run([str(binary)],input=defs+'TRACE M0 20\nTRACE M119 20\n',text=True,capture_output=True,check=True,timeout=20).stdout.replace('\r','')
  srcs=[l.split(' src ')[1].split()[0] for l in out.splitlines() if l.startswith('step ')]
  self.assertEqual(srcs,['0','1','+','2','+','3','+','DROP','0','DROP',';','119','1','+','2','+','3','+','DROP','119','DROP',';'])
 def test_extended_loops_j_leave_plus_loop_and_qdo(self):
  binary=ROOT/'build/tests/forth-system'
  src=(': A 3 0 DO 2 0 DO J 10 * I + . LOOP LOOP ;\nA CR\n: B 10 0 DO I . 2 +LOOP ;\nB CR\n: C 0 10 DO I . -3 +LOOP ;\nC CR\n'
       ': D 10 0 DO I DUP . 4 = IF LEAVE THEN LOOP ;\nD CR\n: E 5 5 ?DO I . LOOP 42 . ;\nE CR\n: F 3 0 ?DO I . LOOP ;\nF CR\n'
       ': G 5 0 DO 3 0 DO J I + 5 = IF LEAVE THEN LOOP LOOP 99 . ;\nG CR\n.S\n: BAD LEAVE ;\n: BAD2 1 +LOOP ;\n')
  out=subprocess.run([str(binary)],input=src,text=True,capture_output=True,check=True,timeout=10).stdout.replace('\r','')
  lines=out.splitlines()
  self.assertEqual(lines[:8],['0 1 10 11 20 21 ','0 2 4 6 8 ','10 7 4 1 ','0 1 2 3 4 ','42 ','0 1 2 ','99 ','<0> '])
  self.assertIn('ERR: LEAVE needs DO',out);self.assertIn('ERR: unbalanced control flow',out)

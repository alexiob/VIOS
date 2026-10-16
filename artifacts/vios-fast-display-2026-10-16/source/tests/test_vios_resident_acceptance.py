"""Acceptance workflow/safety regression checks; never connects to hardware."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

DRIVER = Path(__file__).resolve().parents[1]/'tools/vios_resident_acceptance.py'

class DriverTests(unittest.TestCase):
 def setUp(self):
  self.output=contextlib.redirect_stdout(io.StringIO());self.output.__enter__()
  self.addCleanup(self.output.__exit__,None,None,None)
 def load(self):
  self.assertTrue(DRIVER.is_file(), 'Standalone acceptance driver is missing')
  spec=importlib.util.spec_from_file_location('acceptance',DRIVER)
  module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
  return module
 def test_numeric_assertion_rejects_missing_device_output_even_when_echo_has_expected_value(self):
  m=self.load()
  with self.assertRaises(m.AcceptanceError):
   m.assert_numbers({'ok':True,'output':'DECIMAL 42 . CR\r\nok\r\nvios> '},'DECIMAL 42 . CR',[42])
  self.assertEqual(m.assert_numbers({'ok':True,'output':'DECIMAL 42 . CR\r\n42 \r\nok\r\nvios> '},'DECIMAL 42 . CR',[42]),[42])
 def test_benchmark_requires_all_45_distinct_conditions_and_nine_samples(self):
  m=self.load()
  with self.assertRaises(m.AcceptanceError):m.validate_benchmark('BENCH DONE\n')
  rows=[]
  for engine in range(3):
   for name in ('ARITH','CALL','BRANCH','LOOP','MEM'):
    for n in (25,50,100):
     result={'ARITH':{25:3,50:0,100:0}[n], 'CALL':3*n,'BRANCH':(n//2)*4+(n&1),'LOOP':n*(n-1)//2,'MEM':n}[name]
     rows.append(f'SAMPLE engine={engine} word=BM-{name} n={n} result={result} ticks=1,2,3,4,5,6,7,8,9 median=5')
  text='\n'.join(rows)+'\nBENCH DONE\n'
  self.assertEqual(len(m.validate_benchmark(text)),45)
  with self.assertRaises(m.AcceptanceError):m.validate_benchmark(text.replace('ticks=1,2,3,4,5,6,7,8,9','ticks=1',1))
 def test_boot_refuses_existing_confirmed_selection_before_any_stage_or_trial(self):
  m=self.load()
  class Board:
   def boot_status(self):return {'ok':True,'boot':{'confirmed':'A','candidate':'NONE','state':'CLEAN','current':'A','generation':3}}
  with tempfile.TemporaryDirectory() as tmp:
   report=m.Report(Path(tmp)/'r.json', 'ATT123', ['boot'])
   run=m.Acceptance(Board(),report,'ATT123')
   with self.assertRaises(m.AcceptanceError):run.preflight_boot()
   self.assertEqual(json.loads(report.path.read_text())['checks'][-1]['status'],'failed')
 def test_checkpoint_failure_is_saved_before_exception_returns(self):
  m=self.load()
  with tempfile.TemporaryDirectory() as tmp:
   report=m.Report(Path(tmp)/'r.json','AT1234',['core'])
   with self.assertRaises(m.AcceptanceError):
    report.check('bad',lambda: (_ for _ in ()).throw(m.AcceptanceError('bad readback')))
   data=json.loads(report.path.read_text())
   self.assertEqual(data['checks'][0]['error'],'bad readback')
   self.assertEqual(data['checks'][0]['status'],'failed')
 def test_scratch_guard_rejects_map_overlap_before_hardware_calls(self):
  m=self.load()
  with tempfile.TemporaryDirectory() as tmp:
   path=Path(tmp)/'resident.map';path.write_text('0x000a0001 __bss_end = .\n0x000af000 __stack_bottom = .\n')
   with self.assertRaises(m.AcceptanceError):m.scratch_bounds(path,0xa0000,16)
   path.write_text('0x00080029 __bss_end = .\n0x000af000 __stack_bottom = .\n')
   self.assertEqual(m.scratch_bounds(path,0xa0000,16)['bss_end'],0x80029)

 def test_pixel_readback_uses_physical_coordinates_after_mode_change(self):
  m=self.load()
  class DisplayComplete(Exception):pass
  class Board:
   source_line_limit=235
   def __init__(self):self.logical=True
   def resume(self):return {}
   def evaluate(self,source,timeout=20):
    body=''
    if source=='1 MODE':self.logical=True
    elif source=='23 VDU 0 VDU 192 VDU 0 VDU':self.logical=False
    elif source=='VDP-INFO':body='vdp width 640 height 480 colours 4 mode 1 console-dropped 0'
    elif 'CHAR@' in source:body='86'
    elif source.startswith('1 0 PIXEL@'):body='0 0' if self.logical else '1 255'
    elif 'PIXEL@' in source:body='0 0'
    elif 'BEEP' in source:body='1'
    return {'ok':True,'pending':False,'compiling':False,'output':source+'\r\n'+body+'\r\nok\r\nvios> '}
  class DisplayChecks:
   # Other phases have their own tests. Exercise every real display action,
   # stopping before latency/benchmark work requires additional hardware APIs.
   def check(self,name,action):
    if name.startswith('latency-'):raise DisplayComplete()
    if name=='debug-scratch-original':return {'hex':'00'*16}
    return action() if name.startswith('vdp-') else {}
  board=Board();run=m.Acceptance(board,DisplayChecks(),'ATTEST')
  try:
   with self.assertRaises(DisplayComplete):run.core()
  except m.AcceptanceError as error:
   self.fail('Display acceptance did not establish physical coordinates: '+str(error))
  self.assertFalse(board.logical)


 def test_full_module_boot_workflow_reboots_and_restores_v1_without_deploy(self):
  m=self.load()
  import copy,zlib
  class Board:
   def __init__(self):
    self.active={};self.staged=None;self.value=0;self.confirmed='NONE';self.previous='NONE';self.candidate='NONE';self.current='NONE';self.state='CLEAN';self.generation=0;self.slots={};self.reboots=0
   def result(self,output=''):return {'ok':True,'compiling':False,'pending':False,'output':output}
   def modules(self):return {**self.result(),'modules':[{'name':name,'version':item['version'],'crc32':zlib.crc32(item['source'].encode())} for name,item in self.active.items()]}
   def module_stage(self,name,version,source,test_word='',dependencies=None):
    for dep in dependencies or []:
     if dep['name'] not in self.active or zlib.crc32(self.active[dep['name']]['source'].encode())!=dep['crc32']:
      return {**self.result('error: stale dependency'),'ok':False}
    if '-T 0 ;' in source:return {**self.result('error: module test failed'),'ok':False}
    self.staged=(name,{'version':version,'source':source});return self.result()
   def module_activate(self,name):
    assert name==self.staged[0];self.active[name]=self.staged[1];self.staged=None
    if name.endswith('P'):self.value=int(self.active[name]['version'])
    return self.result()
   def module_source(self,name):
    item=self.active[name];return {**self.result(),'source':item['source'],'crc32':zlib.crc32(item['source'].encode())}
   def module_discard(self):self.staged=None;return self.result()
   def module_save(self,name):return {**self.result(),'path':'/VIOS/01234567/89ABCDEF.VIM'}
   def evaluate(self,source,timeout=20):return self.result(source+'\r\n'+str(self.value)+' '+str(self.value+1)+' \r\nok\r\nvios> ')
   def boot_status(self):return {**self.result(),'boot':{'confirmed':self.confirmed,'candidate':self.candidate,'state':self.state,'current':self.current,'generation':self.generation}}
   def boot_trial(self):
    self.candidate='B' if self.confirmed=='A' else 'A';self.slots[self.candidate]=copy.deepcopy(self.active);self.state='PENDING';return self.result()
   def boot_confirm(self):return self.result()
   def boot_revert(self):self.confirmed,self.previous=self.previous,self.confirmed;return self.result()
   def reboot(self,timeout=120):
    self.reboots+=1;self.generation+=1
    if self.state=='PENDING':self.previous=self.confirmed;self.confirmed=self.candidate;self.candidate='NONE';self.state='CLEAN'
    self.current=self.confirmed;self.active=copy.deepcopy(self.slots[self.current]);self.value=int(next(item['version'] for name,item in self.active.items() if name.endswith('P')))
    return {**self.result(),'rebooted':True,'connected':True}
  with tempfile.TemporaryDirectory() as tmp:
   board=Board();report=m.Report(Path(tmp)/'r.json','ATTEST',['modules','boot'])
   run=m.Acceptance(board,report,'ATTEST');run.modules();run.boot(modules_prepared=True)
   self.assertEqual(board.reboots,3)
   self.assertEqual(board.value,11)
   observed=next((item for item in report.data['checks'] if item['name']=='module-existing-caller-observes-v2'),{})
   self.assertEqual(observed.get('result',{}).get('numbers'),[22,23])

if __name__=='__main__':unittest.main()

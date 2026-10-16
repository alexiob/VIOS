"""Headless workbench protocol validation, without opening hardware."""
import threading
import unittest
from tools.vios_module_client import ModuleClient
try:
 from tools.vios_workbench_client import WorkbenchClient
except ImportError:
 WorkbenchClient=None

def status(revision=2,length=3):
 return f'WB-STATUS revision {revision} catalog 5 open 1 draft 1 dirty 1 staged 0 active 0 saved 0 length {length} cursor 0 selection 0 browser 0 focus 1 words 60 words_max 128 module 415050 version 31 test - error -\r\n'

class WorkbenchClientTests(unittest.TestCase):
 def board(self):
  self.assertIsNotNone(WorkbenchClient,'Workbench client missing')
  class Board(WorkbenchClient,ModuleClient):
   def __init__(self):self.lock=threading.RLock();self.calls=[];self.responses=[];self.compiling=False
   def evaluate(self,source):
    self.calls.append(source)
    result=dict(self.responses.pop(0)) if self.responses else {'ok':True,'output':status()}
    result['output']=source+'\r\n'+result.get('output','')
    return result
  return Board()
 def test_commands_and_no_wire_on_invalid_request(self):
  board=self.board()
  for method,args,command in [('wb_open',(),'WB-OPEN'),('wb_close',(),'WB-CLOSE'),
   ('wb_new',('APP','1',True),'WB-NEW APP 1 1'),('wb_edit',('APP',False),'WB-EDIT APP 0'),
   ('wb_patch',(2,0,1,'\t\r\n'),'WB-PATCH 2 0 1 090D0A'),('wb_undo',(2,),'WB-UNDO 2'),
   ('wb_metadata',(2,'2','CHECK'),'WB-META 2 2 CHECK'),('wb_metadata',(2,'2'),'WB-META 2 2 -'),
   ('wb_stage',(2,),'WB-STAGE 2'),('wb_activate',(2,),'WB-ACTIVATE 2'),('wb_save',(2,),'WB-SAVE 2'),
   ('wb_key',(256,2),'WB-KEY 256 2'),('wb_redraw',(),'WB-REDRAW')]:
   getattr(board,method)(*args);self.assertEqual(board.calls[-1],command)
  before=list(board.calls)
  for args in [(True,0,0,''),(2,-1,0,''),(2,0,4097,''),(2,0,0,'a'*81),(2,0,0,'\033'),(2,0,0,'é'),(2,0,0,'\0')]:
   with self.assertRaises(ValueError):board.wb_patch(*args)
  for method,args in [('wb_new',('APP','1','false')),('wb_select',('WORD',-1)),('wb_select',('MODULE','A B')),('wb_key',(9999,0)),('wb_view',(59,2)),('wb_metadata',(2,'2','-')),('wb_browse',('MODULES','WORDS'))]:
   with self.assertRaises(ValueError):getattr(board,method)(*args)
  self.assertEqual(board.calls,before)
 def test_exact_paginated_source_and_revision_checks(self):
  board=self.board();data=('\\ exact\t\r\n'+'x'*100).encode()
  board.responses=[{'ok':True,'output':status(length=len(data))},
   {'ok':True,'output':f'WB-SOURCE revision 2 offset 0 total {len(data)} hex {data[:80].hex()}\r\n'},
   {'ok':True,'output':f'WB-SOURCE revision 2 offset 80 total {len(data)} hex {data[80:].hex()}\r\n'}]
  result=board.wb_source();self.assertEqual(result['source'],data.decode());self.assertEqual(result['revision'],2)
  self.assertEqual(board.calls,['WB-STATUS','WB-SOURCE 0 80',f'WB-SOURCE 80 {len(data)-80}'])
  for reply in ['WB-SOURCE revision 3 offset 0 total 3 hex 616263',
                'WB-SOURCE revision 2 offset 1 total 3 hex 616263',
                'WB-SOURCE revision 2 offset 0 total 3 hex FF0061',
                'WB-SOURCE revision 2 offset 0 total 3 hex 6',
                'WB-SOURCE revision 2 offset 0 total 3 hex 6162']:
   board.responses=[{'ok':True,'output':status()},{'ok':True,'output':reply}]
   with self.assertRaises(IOError):board.wb_source()
 def test_view_is_80_ascii_characters_and_attributes_with_revision_fence(self):
  board=self.board();text=' '+('X'*79);attrs=bytes([3]*80)
  row=f'WB-VIEW row 4 text {text.encode().hex()} attr {attrs.hex()}\r\n'
  board.responses=[{'ok':True,'output':status()},{'ok':True,'output':row},{'ok':True,'output':status()}]
  result=board.wb_view(4,1);self.assertEqual(result['rows'],[{'row':4,'text':text,'attributes':list(attrs)}])
  self.assertEqual(board.calls,['WB-STATUS','WB-VIEW 4 1','WB-STATUS'])
  for bad in [row.replace('row 4','row 5'),row.replace(text.encode().hex(),'00'*80),row.replace(attrs.hex(),'FF'*80)]:
   board.responses=[{'ok':True,'output':status()},{'ok':True,'output':bad},{'ok':True,'output':status()}]
   with self.assertRaises(IOError):board.wb_view(4,1)
  board.responses=[{'ok':True,'output':status()},{'ok':True,'output':row},{'ok':True,'output':status(3)}]
  with self.assertRaises(IOError):board.wb_view(4,1)
 def test_view_batches_rows_in_one_native_snapshot_command(self):
  board=self.board();rows=''
  for row in range(3):rows+=f'WB-VIEW row {row} text {("x"*80).encode().hex()} attr {bytes([3]*80).hex()}\r\n'
  board.responses=[{'ok':True,'output':status()},{'ok':True,'output':rows},{'ok':True,'output':status()}]
  result=board.wb_view(0,3)
  self.assertEqual([r['row'] for r in result['rows']],[0,1,2])
  self.assertEqual(board.calls,['WB-STATUS','WB-VIEW 0 3','WB-STATUS'])
 def test_status_stats_browse_and_malformed_replies(self):
  board=self.board();result=board.wb_status();self.assertEqual(result['workbench']['module'],'APP')
  board.responses=[{'ok':True,'output':'WB-ITEM word 12 kind 1 name 415050 module 415050\r\n'+status()}]
  self.assertEqual(board.wb_browse('WORDS','APP')['items'][0]['index'],12)
  self.assertEqual(board.calls[-1],'WB-BROWSE WORDS APP')
  board.responses=[{'ok':True,'output':'WB-ITEM module 0 name 415050 version 31 active 1\r\n'+status()}]
  self.assertEqual(board.wb_browse('MODULES')['items'][0]['version'],'1')
  board.responses=[{'ok':True,'output':'WB-FLUSH elapsed_ms 3 bytes 80 spans 1 batches 1 cells 72 pending 0\r\n'}]
  self.assertEqual(board.wb_flush()['render']['bytes'],80)
  board.responses=[{'ok':True,'output':'WB-RENDER-STATS cells 10 spans 1 batches 1 bytes 12 elapsed_ticks 100 pending 0\r\n'}]
  self.assertEqual(board.wb_render_stats(True)['render']['elapsed_ticks'],100)
  for bad in [status().replace('length 3','length 4097'),status().replace('dirty 1','dirty 2'),status().replace('version 31','version FF'),status().replace('module 415050','module 412042'),status()+status(),status().replace(' revision 2',' revision 2 revision 3')]:
   board.responses=[{'ok':True,'output':bad}]
   with self.assertRaises(IOError):board.wb_status()
 def test_inheritance_and_mcp_schemas(self):
  from tools.vios_board import ViosBoard
  from tools.vios_harness import TOOL_MAP,validate_arguments,RpcError
  self.assertTrue(issubclass(ViosBoard,WorkbenchClient))
  for name in ['wb_status','wb_open','wb_close','wb_browse','wb_select','wb_edit','wb_new','wb_patch','wb_undo','wb_metadata','wb_stage','wb_activate','wb_save','wb_source','wb_view','wb_key','wb_redraw','wb_flush','wb_render_stats']:
   self.assertIn(name,TOOL_MAP)
  for args in [{'revision':True,'offset':0,'remove':0,'source':''},{'revision':2,'offset':0,'remove':0,'source':'x'*81},{'revision':2,'offset':0,'remove':0,'source':'\033'}]:
   with self.assertRaises(RpcError):validate_arguments('wb_patch',args)

 def test_full_512_word_catalog_with_maximum_names(self):
  board=self.board();module='M'*31
  inventory=''.join(f'WB-ITEM word {i} kind 1 name {("W%03d"%i+"X"*27).encode().hex()} module {module.encode().hex()}\r\n' for i in range(512))
  report=status().replace('words 60 words_max 128','words 512 words_max 512')
  self.assertGreater(len(inventory),65536)
  board.responses=[{'ok':True,'output':inventory+report}]
  items=board.wb_browse('WORDS')['items']
  self.assertEqual(len(items),512);self.assertEqual(items[-1]['index'],511)

 def test_mcp_dispatch_returns_typed_workbench_and_stops_on_failures(self):
  import json
  from tools.vios_harness import RpcServer
  board=self.board();server=RpcServer(board)
  server.handle({'jsonrpc':'2.0','id':1,'method':'initialize','params':{'protocolVersion':'2025-11-25','capabilities':{},'clientInfo':{}}})
  def call(name,args):
   return server.handle({'jsonrpc':'2.0','id':2,'method':'tools/call','params':{'name':name,'arguments':args}})
  response=call('wb_patch',{'revision':2,'offset':0,'remove':0,'source':'A\t\r\n'})
  self.assertFalse(response['result']['isError'])
  typed=json.loads(response['result']['content'][0]['text'])
  self.assertEqual(typed['workbench']['revision'],2);self.assertEqual(board.calls,['WB-PATCH 2 0 0 41090D0A'])
  count=len(board.calls)
  response=call('wb_patch',{'revision':2,'offset':0,'remove':0,'source':'é'})
  self.assertEqual(response['error']['code'],-32602);self.assertEqual(len(board.calls),count)
  board.responses=[{'ok':True,'output':'WB-STATUS revision BAD'}]
  response=call('wb_status',{})
  self.assertTrue(response['result']['isError']);self.assertEqual(len(board.calls),count+1)
  board.responses=[{'ok':False,'pending':True,'output':'still pending'}]
  response=call('wb_source',{})
  data=json.loads(response['result']['content'][0]['text'])
  self.assertTrue(response['result']['isError']);self.assertTrue(data['pending'])
  self.assertEqual(len(board.calls),count+2)

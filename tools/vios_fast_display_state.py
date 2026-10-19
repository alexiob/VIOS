#!/usr/bin/env python3
"""Restore the explicit fast-display user-state backup through the shared owner.

No serial open, SD save, firmware deployment or boot selection change. Module
activation changes RAM; only missing/different active modules are restored.
"""
import argparse
import json
from pathlib import Path
try:
 from .vios_cli import RPCClient
except ImportError:
 from vios_cli import RPCClient

def restore(c,saved,report):
 def call(n,a=None):
  v=c.tool(n,a);report.append({'tool':n,'arguments':a or {},'result':v})
  assert v.get('ok') is not False and not any(v.get(k) for k in ('pending','compiling','uncertain')),v
  return v
 call('evaluate',{'source':'DECIMAL WB-ZOOM 0','timeout':30})
 assert call('stacks')['data_stack']['depth_text']=='0'
 active={m['name']:m for m in call('modules')['modules'] if m['active']}
 for m in saved['modules']['modules']:
  if not m['active']:continue
  original=saved['module_sources'][m['name']]
  if m['name'] not in active or active[m['name']]['crc32']!=m['crc32']:
   args={'name':m['name'],'version':m['version'],'source':original['source'],'dependencies':m['dependencies']}
   call('module_stage',args);call('module_activate',{'name':m['name']})
  assert call('module_source',{'name':m['name']})['source']==original['source']
 target=saved['status']['workbench'];source=saved['source']['source']
 state=call('wb_edit',{'module':target['module'],'discard':True})['workbench']
 if call('wb_source')['source']!=source:
  for offset in range(0,len(source),80):
   state=call('wb_patch',{'revision':state['revision'],'offset':offset,'remove':state['length'] if offset==0 else 0,'source':source[offset:offset+80]})['workbench']
 if state['version']!=target['version'] or state['test']!=target['test']:
  state=call('wb_metadata',{'revision':state['revision'],'version':target['version'],'test_word':target['test']})['workbench']
 if target['dirty'] or state['cursor']!=target['cursor']:
  state=call('wb_patch',{'revision':state['revision'],'offset':target['cursor'],'remove':0,'source':''})['workbench']
 if target['browser']==1:call('wb_select',{'kind':'WORD','value':target['selection']})
 else:
  items=call('wb_browse',{'browser':'MODULES'})['items'];selected=next(m['name'] for m in saved['modules']['modules'] if saved['modules']['modules'].index(m)==target['selection'])
  assert any(i['name']==selected for i in items);call('wb_select',{'kind':'MODULE','value':selected})
 if call('wb_status')['workbench']['focus']!=target['focus']:call('wb_key',{'code':9})
 call('evaluate',{'source':'DECIMAL ENGINE '+saved['resources']['resources']['engine'],'timeout':30})
 call('wb_open');call('evaluate',{'source':'WB-FLUSH-FENCED','timeout':30})
 if not target['open']:call('wb_close')
 after=call('wb_status')['workbench']
 for key in ('open','dirty','length','cursor','selection','browser','focus','module','version','test'):
  assert after[key]==target[key],(key,after[key],target[key])
 assert call('wb_source')['source']==source
 # Boot slots may legitimately move on (e.g. WBM1P persisted into slot B on
 # 2026-10-19); report the difference instead of failing the restore.
 boot=call('boot_status')['boot']
 if boot!=saved['boot']['boot']:report.append({'note':'boot selection differs from the saved record','now':boot,'saved':saved['boot']['boot']})
 assert call('stacks')['data_stack']['depth_text']=='0'
 return after

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--saved',type=Path,default=Path('docs/bring-up/vios-fast-display-preserved.json'));p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 r={'status':'running','events':[]};saved=json.loads(a.saved.read_text())
 try:
  with RPCClient(timeout=120) as c:c.initialize();r['after']=restore(c,saved,r['events'])
  r['status']='passed'
 except BaseException as error:
  r.update(status='failed',error_type=type(error).__name__,error=str(error));raise
 finally:a.output.write_text(json.dumps(r,indent=2)+'\n')
 print('Original module sources, draft, focus, cursor and closed/open state restored.')
if __name__=='__main__':main()

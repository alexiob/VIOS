"""Restore preserved draft and verify shared CLI/MCP without opening serial."""
from pathlib import Path
import json,subprocess,select,hashlib,datetime
from tools.vios_cli import RPCClient
root=Path.cwd();saved=json.loads((root/'docs/bring-up/vios-incremental-before-deploy.json').read_text());report={'status':'running','checks':[]}
def record(name,value):report['checks'].append({'name':name,'result':value});return value
with RPCClient(timeout=120) as c:
 c.initialize()
 def call(name,args=None):
  r=c.tool(name,args);record(name,r);assert r.get('ok') is not False and not any(r.get(k) for k in ('pending','compiling','uncertain')),r;return r
 target=saved['status']['workbench'];src=saved['source']['source']
 state=call('wb_edit',{'module':target['module'],'discard':True})['workbench']
 current=call('wb_source')['source']
 if current!=src:
  for offset in range(0,len(src),80):
   state=call('wb_patch',{'revision':state['revision'],'offset':offset,'remove':state['length'] if offset==0 else 0,'source':src[offset:offset+80]})['workbench']
 if state['version']!=target['version'] or state['test']!=target['test']:
  state=call('wb_metadata',{'revision':state['revision'],'version':target['version'],'test_word':target['test']})['workbench']
 if target['dirty']:
  state=call('wb_patch',{'revision':state['revision'],'offset':target['cursor'],'remove':0,'source':''})['workbench']
 if target['browser']==1:call('wb_select',{'kind':'WORD','value':target['selection']})
 else:call('wb_select',{'kind':'MODULE','value':target['module']})
 state=call('wb_status')['workbench']
 if state['focus']!=target['focus']:call('wb_key',{'code':9})
 call('wb_open');call('wb_flush');source=call('wb_source');assert source['source']==src
 record('original_draft_restored',{'sha256':hashlib.sha256(src.encode()).hexdigest(),'bytes':len(src),'dirty':target['dirty'],'cursor':target['cursor']})
 rows=call('wb_view',{'row':0,'count':60});(root/'docs/bring-up/vios-incremental-screen.json').write_text(json.dumps(rows,indent=2)+'\n')
 call('evaluate',{'source':'.S BASE @ . CR','timeout':30});call('boot_status');call('evaluate',{'source':'VDP-INFO','timeout':30})
 # Sample immutable code from running RAM, with the owner's pause/read/resume
 # guard. This verifies deployed code rather than trusting only host filenames.
 image=(root/'build/vios-resident.bin').read_bytes()
 for offset in (0,0x6000,0x16000):
  got=call('memory_read',{'address':0x40000+offset,'count':128});assert bytes.fromhex(got['hex'])==image[offset:offset+128]
proc=subprocess.run([str(root/'.venv/bin/python'),'tools/vios_cli.py'],input=':wb status\n:quit\n',text=True,capture_output=True,timeout=30)
assert proc.returncode==0,(proc.stdout,proc.stderr)
assert 'workbench' in proc.stdout and 'WBM1P' in proc.stdout
(root/'docs/bring-up/vios-incremental-cli.txt').write_text(proc.stdout+proc.stderr)
record('actual_cli',{'exit_code':proc.returncode,'output':proc.stdout})
p=subprocess.Popen(['/usr/bin/nc','127.0.0.1','8765'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
responses=[]
try:
 messages=[{'jsonrpc':'2.0','id':1,'method':'initialize','params':{'protocolVersion':'2025-11-25','capabilities':{},'clientInfo':{'name':'vios-incremental-probe','version':'1'}}},
 {'jsonrpc':'2.0','method':'notifications/initialized'},
 {'jsonrpc':'2.0','id':2,'method':'tools/list'},
 {'jsonrpc':'2.0','id':3,'method':'tools/call','params':{'name':'wb_status','arguments':{}}}]
 for m in messages:p.stdin.write(json.dumps(m)+'\n');p.stdin.flush()
 for expected in (1,2,3):
  ready,_,_=select.select([p.stdout],[],[],30);assert ready,'MCP stdio response timeout'
  response=json.loads(p.stdout.readline());assert response.get('id')==expected and 'result' in response,response;responses.append(response)
 assert len(responses[1]['result']['tools'])==48
 assert not responses[2]['result'].get('isError')
finally:p.terminate();p.wait(timeout=5)
record('actual_stdio_mcp',{'command':['/usr/bin/nc','127.0.0.1','8765'],'tools':48,'responses':responses})
report['status']='passed';report['utc']=datetime.datetime.now(datetime.timezone.utc).isoformat()
(root/'docs/bring-up/vios-incremental-control.json').write_text(json.dumps(report,indent=2)+'\n')
print('passed; original draft restored; CLI and stdio MCP connected through shared owner')

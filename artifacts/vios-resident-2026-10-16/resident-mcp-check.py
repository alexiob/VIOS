import sys,json,time,statistics,subprocess,socket,select
from pathlib import Path
sys.path.insert(0,'tools')
from vios_resident_acceptance import Report,require
transport=sys.argv[1]
prefix='docs/bring-up/vios-resident-mcp-'+transport
r=Report(prefix+'.json','MCP1',[transport])
proc=None;stream=None;err=None
if transport in ('stdio','stdio-proxy'):
 err=open(prefix+'.stderr.txt','w')
 command=['/usr/bin/nc','127.0.0.1','8765'] if transport=='stdio-proxy' else ['.venv/bin/python','tools/vios_harness.py','--backend','resident','--log',prefix+'.serial.jsonl']
 proc=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,text=True,bufsize=1)
 send,receive=proc.stdin,proc.stdout
else:
 stream=socket.create_connection(('127.0.0.1',8765),timeout=130)
 send=receive=stream.makefile('rw',encoding='utf-8',newline='\n')
ident=0
def rpc(method,params):
 global ident
 ident+=1
 send.write(json.dumps({'jsonrpc':'2.0','id':ident,'method':method,'params':params})+'\n');send.flush()
 if proc:
  ready,_,_=select.select([receive],[],[],130)
  require(bool(ready),'RPC timeout')
 raw=receive.readline();require(bool(raw),'RPC stream closed')
 response=json.loads(raw);require(response.get('id')==ident and 'error' not in response,'Bad RPC response '+repr(response))
 return response['result']
def call(name,args=None,allow_error=False):
 result=rpc('tools/call',{'name':name,'arguments':args or {}})
 require(allow_error or not result.get('isError'),'Tool error '+repr(result))
 return json.loads(result['content'][0]['text'])
def check_output(source,wanted):
 d=call('evaluate',{'source':source});require(d.get('ok') is True and wanted in d['output'],'Missing expected result '+repr(d));return d
try:
 def init():
  d=rpc('initialize',{'protocolVersion':'2025-11-25','capabilities':{},'clientInfo':{'name':'vios-resident-acceptance','version':'1'}})
  require('235' in d.get('instructions',''),'Wrong resident source profile')
  send.write(json.dumps({'jsonrpc':'2.0','method':'notifications/initialized'})+'\n');send.flush();return d
 r.check('initialize-resident',init)
 def listing():
  d=rpc('tools/list',{});require(len(d['tools'])==29,'Expected29tools');return {'count':len(d['tools'])}
 r.check('discover29tools',listing)
 r.check('connect-no-deploy',lambda:call('connect'))
 r.check('compile-and-execute',lambda:check_output('7 DUP * . CR' if transport=='stdio-proxy' else ': MCP-SQUARE DUP * ; 7 MCP-SQUARE . CR','49'))
 r.check('retained-exact-source',lambda:call('module_source',{'name':'LIVE1P'}))
 r.check('boot-status-confirmed',lambda:call('boot_status'))
 r.check('debug-registers',lambda:call('debug_registers'))
 for kind in ('evaluate','inspect','resources'):
  def latency(kind=kind):
   times=[]
   for _ in range(9):
    t=time.perf_counter()
    if kind=='evaluate':check_output('DECIMAL 6 7 * . CR','42')
    elif kind=='inspect':call('inspect_word',{'name':'+'})
    else:call('resources')
    times.append(round((time.perf_counter()-t)*1000,3))
   return {'samples_ms':times,'median_ms':statistics.median(times)}
  r.check('latency-'+kind,latency)
 r.data['status']='passed';r.save()
except BaseException as e:
 r.data.update(status='failed',error=repr(e));r.save();raise
finally:
 if proc:
  send.close()
  if transport=='stdio-proxy':proc.terminate() # MCP client shutdown closes only this TCP connection.
  proc.wait(timeout=15);err.close()
 else:send.close();stream.close()

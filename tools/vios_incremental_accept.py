#!/usr/bin/env python3
"""Focused live compositor regression/control checks via the existing owner.
Replaces the current draft; caller must preserve it first. No saves/boot writes.
"""
import argparse,json,re,time
from pathlib import Path
try:
 from .vios_cli import RPCClient
except ImportError:
 from vios_cli import RPCClient

def run(c,path):
 report={'status':'running','checks':[]}
 def save():path.write_text(json.dumps(report,indent=2)+'\n')
 def call(name,args=None,allow_pending=False):
  start=time.perf_counter();r=c.tool(name,args)
  report['checks'].append({'tool':name,'arguments':args or {},'result':r,'host_ms':(time.perf_counter()-start)*1000});save()
  assert allow_pending or (r.get('ok') is not False and not any(r.get(k) for k in ('pending','compiling','uncertain'))),r
  return r
 def cmd(source):return call('evaluate',{'source':source,'timeout':30})
 def state():return call('wb_status')['workbench']
 def patch(offset,remove,text):return call('wb_patch',{'revision':state()['revision'],'offset':offset,'remove':remove,'source':text})
 def view():return call('wb_view',{'row':0,'count':60})['rows']
 call('wb_open');cmd('DECIMAL')
 call('wb_new',{'module':'WBCHK','version':'1','discard':True});patch(0,0,'a\n\n')
 if state()['focus']==0:call('wb_key',{'code':9})
 call('wb_flush');rev=state()['revision']
 cmd(f'WB-PATCH {rev} 1 0 09 WB-KEY 257 0 WB-VIEW 0 0')
 rows=view();assert rows[19]['attributes'][26]==32 and rows[20]['attributes'][26]==3
 call('wb_new',{'module':'WBCHK','version':'1','discard':True});patch(0,0,'a\nb');call('wb_flush');rev=state()['revision']
 cmd(f'WB-PATCH {rev} 0 0 78 WB-KEY 257 0 WB-KEY 258 0 WB-VIEW 0 0')
 rows=view();assert rows[19]['attributes'][26]==32 and rows[19]['attributes'][27]==3
 cmd('VARIABLE IC-IV 888 IC-IV ! DEFER IC-ID : IC-LONGTARGET 7 ; : IC-RT 8 ; \' IC-LONGTARGET IS IC-ID')
 items=call('wb_browse',{'browser':'WORDS'})['items'];indices={i['name']:i['index'] for i in items}
 call('wb_select',{'kind':'WORD','value':indices['IC-IV']});rows=view();assert rows[9]['text'][34:37]=='888'
 cmd('9 IC-IV !');rows=view();assert rows[9]['text'][34:38]=='9   '
 call('wb_select',{'kind':'WORD','value':indices['IC-ID']});rows=view();assert rows[8]['text'][35:48]=='IC-LONGTARGET'
 cmd("' IC-RT IS IC-ID");rows=view();assert rows[8]['text'][35:48]=='IC-RT        '
 cmd('111 222 333');rows=view();assert rows[12]['text'][26:29]=='333'
 cmd('DROP DROP');rows=view();assert rows[12]['text'][26:30]=='111 ' and rows[12]['text'][43:80].strip()==''
 cmd('DROP HEX');rows=view();assert rows[12]['text'][26:80].strip()=='' and rows[11]['text'][48:50]=='16'
 cmd('DECIMAL');call('wb_flush');call('wb_render_stats',{'reset':True});cmd('WB-COMPOSE');call('wb_flush')
 assert call('wb_render_stats')['render']['bytes']==0
 rows=view();call('wb_redraw');call('wb_flush');after=view();assert rows==after
 cmd('$80 COLOUR 0 0 CHAR@ . CR')
 assert re.search(r'\n86\s',report['checks'][-1]['result']['output'].replace('\r',''))
 # The serial CLI remains usable while panes are active and KEY is pending.
 pending=call('evaluate',{'source':'KEY . CR','timeout':0.1},allow_pending=True);assert pending.get('pending')
 call('console_input',{'hex':'41'},allow_pending=True)
 result=call('await_result',{'timeout':30});assert re.search(r'\n65\s',result['output'].replace('\r',''))
 report['vdp']=cmd('VDP-INFO');report['boot']=call('boot_status')
 assert report['boot']['boot']['confirmed']=='A' and report['boot']['boot']['state']=='CLEAN'
 report['status']='passed';save()
 print('passed',len(report['checks']),'focused hardware requests',flush=True)

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.parent.mkdir(parents=True,exist_ok=True)
 with RPCClient(timeout=120) as c:c.initialize();run(c,a.output)
if __name__=='__main__':main()

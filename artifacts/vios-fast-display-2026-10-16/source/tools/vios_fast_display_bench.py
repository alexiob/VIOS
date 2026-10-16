#!/usr/bin/env python3
"""Display-only performance and control acceptance over the existing TCP owner.

Replaces the unsaved workbench draft with a fixture; preserve user state first.
Never opens serial, deploys, saves SD files, or changes boot. Console reports are
outside timed drawing. The fenced variant also waits for ESP framebuffer work.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import statistics
import time
try:
 from .vios_cli import RPCClient
 from .vios_incremental_bench import FIXTURE, MIXED_FIXTURE
 from .vios_workbench_hardware import integer_fields, numeric_output
except ImportError:
 from vios_cli import RPCClient
 from vios_incremental_bench import FIXTURE, MIXED_FIXTURE
 from vios_workbench_hardware import integer_fields, numeric_output

class Bench:
 def __init__(self,c,path,baud,samples,fixture):
  self.c,self.path,self.samples,self.fixture=c,path,samples,fixture
  self.r={'utc':datetime.now(timezone.utc).isoformat(),'internal_baud':baud,'usb_baud':115200,'timer_hz':288000,
   'fixture_bytes':len(fixture),'fixture_sha256':hashlib.sha256(fixture.encode()).hexdigest(),'events':[],'samples':{},
   'method':'WB-COMPOSE measures CPU composition without DS perturbation; native callback ticks measure output; WB-FLUSH-FENCED includes ordered framebuffer CHAR@ before stopping timer. Echo/preparation/result records/host round trips excluded from these device intervals. No VGA mirroring to USB.'}
 def save(self):self.path.write_text(json.dumps(self.r,indent=2)+'\n')
 def call(self,n,a=None,pending=False):
  start=time.perf_counter();v=self.c.tool(n,a)
  self.r['events'].append({'tool':n,'arguments':a or {},'result':v,'host_ms':(time.perf_counter()-start)*1000});self.save()
  assert pending or (v.get('ok') is not False and not any(v.get(k) for k in ('pending','compiling','uncertain'))),v
  return v
 def cmd(self,s,pending=False,timeout=30):
  assert len(s)<=235,s
  return self.call('evaluate',{'source':s,'timeout':timeout},pending)
 def state(self):return self.call('wb_status')['workbench']
 def position(self,n):
  s=self.state();self.call('wb_patch',{'revision':s['revision'],'offset':n,'remove':0,'source':''});self.call('wb_flush')
 def sample(self,group,action='',fenced=True):
  self.cmd("' DECIMAL IS WB-PAINT")
  command=(action+' ' if action else '')+'WB-COMPOSE'
  comp=integer_fields(self.cmd(command)['output'],'WB-COMPOSE',['elapsed_ticks'])['elapsed_ticks']
  self.call('wb_render_stats',{'reset':True})
  result=self.cmd("' WB-PAINT-DEFAULT IS WB-PAINT "+('WB-FLUSH-FENCED' if fenced else 'WB-FLUSH'))
  fields=integer_fields(result['output'],'WB-FLUSH',['elapsed_ms','bytes','spans','batches','cells','pending']);assert fields['pending']==0
  fields['compose_ms']=comp/288;fields['native_ms']=self.call('wb_render_stats')['render']['elapsed_ticks']/288
  fields['fenced']=fenced;self.r['samples'].setdefault(group,[]).append(fields);self.save()
 def framebuffer(self,tag):
  self.cmd("' WB-PAINT-DEFAULT IS WB-PAINT WB-FLUSH-FENCED")
  rows=self.call('wb_view',{'row':0,'count':60})['rows'];self.call('wb_flush')
  # Only plain fg3/bg0 cells: CHAR@ matches pixels against current background.
  probes=[]
  for y in (0,18,19,36,54,55):
   for x in (0,26,40,70):
    if rows[y]['attributes'][x]==3 and rows[y]['text'][x]!=' ':probes.append((x,y,ord(rows[y]['text'][x])))
  assert probes
  for x,y,ch in probes[:10]:
   source=f'3 COLOUR 128 COLOUR {x} {y} CHAR@ . CR'
   assert numeric_output(self.cmd(source)['output'],source)==[ch],(tag,x,y,ch)
  # Retained output agrees with a full reconstruction in the same geometry.
  self.cmd('WB-REDRAW WB-FLUSH-FENCED')
  again=self.call('wb_view',{'row':0,'count':60})['rows'];assert rows==again,tag
  self.r.setdefault('framebuffer_checks',[]).append({'tag':tag,'glyphs':min(10,len(probes)),'retained_equals_full':True});self.save()
 def run(self):
  self.r['before']=self.state();self.r['vdp_before']=self.cmd('VDP-INFO');self.r['boot_before']=self.call('boot_status')
  self.cmd('DECIMAL WB-ZOOM 0');self.call('wb_open');self.call('wb_new',{'module':'WBFAST','version':'1','discard':True})
  for offset in range(0,len(self.fixture),80):
   s=self.state();self.call('wb_patch',{'revision':s['revision'],'offset':offset,'remove':0,'source':self.fixture[offset:offset+80]})
  self.position(0)
  if self.state()['focus']!=0:self.call('wb_key',{'code':9})
  self.call('wb_browse',{'browser':'MODULES'});self.call('wb_flush')
  for i in range(self.samples):self.sample('full_precomposed','WB-REPAINT',fenced=False)
  for i in range(self.samples):self.sample('full_precomposed_fenced','WB-REPAINT')
  print('full screen done',flush=True)
  for i in range(3):self.sample('unchanged')
  self.call('wb_key',{'code':9});self.position(5)
  for i in range(self.samples+1):self.sample('character_edit','WB-KEY '+('90' if i%2==0 else '8')+' 0')
  for i in range(self.samples+1):self.sample('newline_edit','WB-KEY '+('13' if i%2==0 else '8')+' 0')
  self.framebuffer('after-edit-pairs')
  self.position(len(self.fixture))
  for i in range(self.samples+1):self.sample('source_scroll','WB-KEY '+('256' if i%2==0 else '257')+' 0')
  self.framebuffer('normal-scroll')
  self.cmd('WB-ZOOM 1 WB-FLUSH-FENCED')
  for i in range(self.samples+1):self.sample('zoom_scroll','WB-KEY '+('256' if i%2==0 else '257')+' 0')
  self.framebuffer('zoom-scroll');self.cmd('WB-ZOOM 0 WB-FLUSH-FENCED')
  print('edit, scroll and zoom done',flush=True)
  # CLI sources interleave redraw/zoom with maximum-size incoming lines.
  for i in range(50):
   s=('WB-REPAINT' if i%10==0 else ('WB-ZOOM '+str((i//10)%2) if i%10==1 else '\\ '+str(i).ljust(233,'x')))
   self.cmd(s)
  self.cmd('WB-ZOOM 0 WB-FLUSH-FENCED')
  pending=self.cmd('KEY . CR',pending=True,timeout=.1);assert pending.get('pending'),pending
  self.call('console_input',{'hex':'41'},pending=True)
  result=self.call('await_result',{'timeout':30});assert re.search(r'\n65\s',result['output'].replace('\r','')),result
  self.r['vdp_after']=self.cmd('VDP-INFO');self.r['boot_after']=self.call('boot_status')
  def dropped(v):return int(re.search(r'console-dropped (\d+)',v['output']).group(1))
  assert dropped(self.r['vdp_before'])==dropped(self.r['vdp_after'])
  assert self.r['boot_before']['boot']==self.r['boot_after']['boot']
  assert self.call('wb_source')['source']==self.fixture
  self.r['stacks_after']=self.call('stacks');assert self.r['stacks_after']['data_stack']['depth_text']=='0'
  self.r['medians']={k:{f:statistics.median(s[f] for s in rows) for f in ('compose_ms','native_ms','elapsed_ms','bytes','spans','batches','cells')} for k,rows in self.r['samples'].items()}
  self.r['status']='passed';self.save()
  for k,v in self.r['medians'].items():print(k,v,flush=True)

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--baud',type=int,choices=(115200,384000,1152000),required=True)
 p.add_argument('--samples',type=int,default=7);p.add_argument('--fixture',choices=('plain','mixed'),default='plain');a=p.parse_args()
 assert 3<=a.samples<=15 and a.samples%2==1
 a.output.parent.mkdir(parents=True,exist_ok=True)
 with RPCClient(timeout=120) as c:
  c.initialize();bench=Bench(c,a.output,a.baud,a.samples,FIXTURE if a.fixture=='plain' else MIXED_FIXTURE)
  try:bench.run()
  except BaseException as error:
   bench.r.update(status='failed',error_type=type(error).__name__,error=str(error));bench.save();raise
if __name__=='__main__':main()

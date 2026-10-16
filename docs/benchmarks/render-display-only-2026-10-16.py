from tools.vios_cli import RPCClient
from tools.vios_workbench_hardware import integer_fields, numeric_output
from pathlib import Path
from datetime import datetime,timezone
import json,time,statistics,hashlib
OUT=Path('docs/benchmarks/render-display-only-2026-10-16.json')
R={'utc':datetime.now(timezone.utc).isoformat(),'timer_hz':288000,'internal_uart_baud':115200,'events':[],'samples':{},'method':'WB-FLUSH on-board timer; console record printed only after timed rendering. Preparation, command echo, final reports and host round trips excluded. No glyph mirroring to USB. Cached runs compose before timed flush with paint hook temporarily bound to existing DECIMAL word; observer restores BASE. Original native paint hook is used throughout measured rendering.'}
def save():OUT.write_text(json.dumps(R,indent=2)+'\n')
with RPCClient(timeout=90) as c:
 c.initialize()
 def call(name,args=None):
  start=time.perf_counter();r=c.tool(name,args)
  R['events'].append({'tool':name,'arguments':args or {},'result':r,'host_ms':(time.perf_counter()-start)*1000});save()
  assert r.get('ok') is not False and not any(r.get(k) for k in ('pending','compiling','uncertain')),r
  return r
 def cmd(s):
  assert len(s)<=235
  return call('evaluate',{'source':s,'timeout':30})
 def sample(group,source):
  call('wb_render_stats',{'reset':True})
  r=cmd(source)
  values=integer_fields(r['output'],'WB-FLUSH',['elapsed_ms','bytes','spans','batches','cells','pending'])
  values['native']=call('wb_render_stats')['render'];assert values['pending']==0,values
  values['native_ms']=values['native']['elapsed_ticks']/288
  R['samples'].setdefault(group,[]).append(values);save()
 before=call('wb_status')['workbench'];R['before']=before
 R['source_before']=call('wb_source')['source'];R['stack_before']=call('stacks')
 assert before['cursor']==0 and before['focus']==0 and before['browser']==0 and before['selection']==0,before
 assert R['stack_before']['data_stack']['depth_text']=='0'
 assert numeric_output(cmd('BASE @ . CR')['output'],'BASE @ . CR')==[10]
 R['vdp_before']=cmd('VDP-INFO')
 try:
  call('wb_open');call('wb_flush')
  for i in range(7):
   cmd("' DECIMAL IS WB-PAINT WB-REDRAW")
   sample('full_precomposed',"' WB-PAINT-DEFAULT IS WB-PAINT WB-FLUSH")
  print('Full precomposed complete',flush=True)
  for i in range(7):sample('full_compose_and_paint','WB-REDRAW WB-FLUSH')
  print('Full composition plus paint complete',flush=True)
  call('wb_key',{'code':9})
  for i in range(5):cmd(' '.join(['WB-KEY 257 0']*8))
  call('wb_flush')
  for i in range(8):
   cmd("' DECIMAL IS WB-PAINT WB-KEY "+str(256 if i%2==0 else 257)+' 0')
   sample('source_scroll_precomposed',"' WB-PAINT-DEFAULT IS WB-PAINT WB-FLUSH")
  print('Source scroll complete',flush=True)
  for i in range(5):cmd(' '.join(['WB-KEY 256 0']*8))
  cmd('WB-KEY 260 0 WB-KEY 9 0 WB-REDRAW WB-FLUSH')
  # Direct pinned-VDP viewport scroll, including a framebuffer CHAR@ fence.
  # No production shadow optimization is installed; redraw restores both copies.
  rows=call('wb_view',{'row':18,'count':2})['rows'];R['viewport_rows_before']=rows
  x=next(j for j in range(26,80) if rows[0]['text'][j]!=rows[1]['text'][j])
  R['viewport_probe']={'x':x,'y':18,'expected_after':ord(rows[1]['text'][x])}
  for i in range(7):
   cmd("' WB-PAINT-DEFAULT IS WB-PAINT WB-REDRAW WB-FLUSH")
   cmd("' DECIMAL IS WB-PAINT 28 VDU 26 VDU 55 VDU 79 VDU 18 VDU")
   source=f'TIMER@ 23 VDU 7 VDU 0 VDU 3 VDU 8 VDU 3 COLOUR 128 COLOUR {x} 18 CHAR@ SWAP TIMER@ - $FFFF AND . . CR'
   nums=numeric_output(cmd(source)['output'],source);assert len(nums)==2,nums
   assert nums[1]==R['viewport_probe']['expected_after'],(nums,R['viewport_probe'])
   R['samples'].setdefault('viewport_scroll_only_fenced',[]).append({'ticks':nums[0],'ms':nums[0]/288,'glyph':nums[1],'scroll_command_bytes':5,'includes':'five individual Forth VDU native calls, colour reset and CHAR@ framebuffer fence; does not draw exposed line or update VIOS shadows'})
   save()
  R['status']='passed'
 finally:
  cmd("' WB-PAINT-DEFAULT IS WB-PAINT WB-REDRAW WB-FLUSH")
  if not before['open']:call('wb_close')
  R['after']=call('wb_status')['workbench'];R['source_after']=call('wb_source')['source'];R['stack_after']=call('stacks');R['base_after']=cmd('BASE @ . CR');R['vdp_after']=cmd('VDP-INFO')
  assert R['before']==R['after'],(R['before'],R['after'])
  assert R['source_before']==R['source_after']
  assert R['stack_after']['data_stack']['depth_text']=='0'
  assert numeric_output(R['base_after']['output'],'BASE @ . CR')==[10]
  save()
for name,rows in R['samples'].items():
 summary={key:statistics.median([v[key] for v in rows]) for key in ('elapsed_ms','bytes','spans','batches','cells','native_ms','ms') if all(key in v for v in rows)}
 R.setdefault('medians',{})[name]=summary;print(name,summary,flush=True)
save()

from tools.vios_cli import RPCClient
from tools.vios_workbench_hardware import numeric_output
from pathlib import Path
import json,statistics,hashlib
P=Path('docs/benchmarks/render-display-only-2026-10-16.json');r=json.loads(P.read_text())
src=r['source_before'];line=src.splitlines()[38].encode('ascii');assert 0<len(line)<54
stream=bytes([17,3,17,128,28,26,55,79,18,23,7,0,3,8,31,0,37])+line+bytes([28,0,59,79,0])
addr=0xA0000
assert 0x97D39<addr and addr+len(stream)<0xAF000
r['pane_scroll_prototype']={'address':addr,'bytes':len(stream),'stream_hex':stream.hex(),'exposed_source_line':line.decode(),'method':'One native VTYPE of colour setup, text viewport, eight-pixel scroll, exposed text row and viewport restoration. TIMER@ brackets output plus ordered framebuffer CHAR@ fence. Excludes VIOS cache/paint-shadow maintenance and inspector refresh. Scratch buffer is backed up then readback-verified on write and restoration. No firmware change.'}
def save():P.write_text(json.dumps(r,indent=2)+'\n')
with RPCClient(timeout=90) as c:
 c.initialize()
 def call(n,a=None):
  v=c.tool(n,a);r['events'].append({'tool':n,'arguments':a or {},'result':v});save()
  assert v.get('ok') is not False and not any(v.get(k) for k in ('pending','compiling','uncertain')),v
  return v
 def cmd(s):
  assert len(s)<=235
  return call('evaluate',{'source':s,'timeout':30})
 before=call('wb_status')['workbench'];assert before==r['before']
 backup=call('memory_read',{'address':addr,'count':len(stream)})['hex'];r['pane_scroll_prototype']['scratch_before']=backup;save()
 try:
  call('memory_write',{'address':addr,'hex':stream.hex()})
  call('wb_open')
  for i in range(7):
   cmd("' WB-PAINT-DEFAULT IS WB-PAINT WB-REDRAW WB-FLUSH")
   cmd("' DECIMAL IS WB-PAINT")
   s=f'TIMER@ ${addr:X} {len(stream)} VTYPE 26 55 CHAR@ SWAP TIMER@ - $FFFF AND . . CR'
   nums=numeric_output(cmd(s)['output'],s);assert len(nums)==2 and nums[1]==line[0],nums
   proof=cmd('26 18 CHAR@ . CR');assert numeric_output(proof['output'],'26 18 CHAR@ . CR')==[ord(src.splitlines()[1][0])]
   r['samples'].setdefault('viewport_scroll_and_exposed_line_fenced',[]).append({'ticks':nums[0],'ms':nums[0]/288,'glyph':nums[1],'bytes':len(stream)});save()
  for i in range(7):
   s='TIMER@ 26 55 CHAR@ SWAP TIMER@ - $FFFF AND . . CR'
   nums=numeric_output(cmd(s)['output'],s);assert len(nums)==2 and nums[1]==line[0],nums
   r['samples'].setdefault('char_fence_baseline',[]).append({'ticks':nums[0],'ms':nums[0]/288,'glyph':nums[1]});save()
 finally:
  call('memory_write',{'address':addr,'hex':backup})
  restored=call('memory_read',{'address':addr,'count':len(stream)})['hex'];assert restored==backup
  r['pane_scroll_prototype']['scratch_restored']=True
  cmd("' WB-PAINT-DEFAULT IS WB-PAINT WB-REDRAW WB-FLUSH")
  if not before['open']:call('wb_close')
  r['after_extended']=call('wb_status')['workbench'];assert before==r['after_extended']
  assert call('wb_source')['source']==src
  r['stacks_after_extended']=call('stacks');assert r['stacks_after_extended']['data_stack']['depth_text']=='0'
  r['vdp_after_extended']=cmd('VDP-INFO');save()
for name,rows in r['samples'].items():
 r['medians'][name]={key:statistics.median([v[key] for v in rows]) for key in ('elapsed_ms','bytes','spans','batches','cells','native_ms','ms') if all(key in v for v in rows)}
print(r['medians'],flush=True);r['status']='passed';save()

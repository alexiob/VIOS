#!/usr/bin/env python3
"""Observe physical keyboard editing while CLI redraw/inspection traffic runs.

Uses the shared TCP owner. Requires a disposable unsaved draft and local user;
does not synthesize navigation keys, save SD files, or change boot selections.
Create --stop-file only after the user has finished to end observation early.
"""
import argparse
import json
from pathlib import Path
import re
import time
try:
 from .vios_cli import RPCClient
except ImportError:
 from vios_cli import RPCClient

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--output',type=Path,required=True)
 p.add_argument('--stop-file',type=Path,required=True)
 p.add_argument('--seconds',type=int,default=180)
 a=p.parse_args();assert 30<=a.seconds<=600 and not a.stop_file.exists()
 r={'status':'running','events':[],'keyboard_input_synthesized':False}
 def save():a.output.write_text(json.dumps(r,indent=2)+'\n')
 try:
  with RPCClient(timeout=120) as c:
   c.initialize()
   def call(n,args=None):
    start=time.monotonic();v=c.tool(n,args)
    r['events'].append({'tool':n,'arguments':args or {},'result':v,'host_ms':1000*(time.monotonic()-start)});save()
    assert v.get('ok') is not False and not any(v.get(k) for k in ('pending','compiling','uncertain')),v
    return v
   def cmd(s):return call('evaluate',{'source':s,'timeout':30})
   cmd('WB-OPEN WB-ZOOM 0 WB-FLUSH-FENCED')
   if call('wb_status')['workbench']['focus']!=1:call('wb_key',{'code':9})
   r['before']=call('wb_status')['workbench'];r['vdp_before']=cmd('VDP-INFO')['output']
   start=time.monotonic();i=0
   print('Physical keyboard observation ready; no host navigation generated.',flush=True)
   while time.monotonic()-start<a.seconds and not a.stop_file.exists():
    cmd('WB-REPAINT' if i%10==0 else '\\ '+str(i).ljust(233,'x'))
    call('wb_status');cmd('WB-ZOOM?')
    if i%10==5:cmd('WB-FLUSH-FENCED')
    i+=1;time.sleep(.1)
   r['seconds']=time.monotonic()-start;r['iterations']=i
   r['after']=call('wb_status')['workbench'];r['vdp_after']=cmd('VDP-INFO')['output']
   states=[e['result']['workbench'] for e in r['events'] if e['tool']=='wb_status']
   r['observed_cursors']=sorted({s['cursor'] for s in states})
   r['observed_zoom']=sorted({int(re.search(r'WB-ZOOM enabled (\d)',e['result']['output']).group(1)) for e in r['events'] if e.get('arguments',{}).get('source')=='WB-ZOOM?'})
   assert len(r['observed_cursors'])>1,'No physical cursor movement observed'
   drops=lambda s:int(re.search(r'console-dropped (\d+)',s).group(1))
   assert drops(r['vdp_before'])==drops(r['vdp_after'])==0
   cmd('WB-ZOOM 0 WB-FLUSH-FENCED')
   r['status']='passed';save()
 except BaseException as error:
  r.update(status='failed',error_type=type(error).__name__,error=str(error));save();raise
 print('Physical movement observed with zero drop growth under CLI/redraw load.')

if __name__=='__main__':main()

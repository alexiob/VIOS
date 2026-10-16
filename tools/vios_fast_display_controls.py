#!/usr/bin/env python3
"""Qualify fast-display inspection, CLI and hook control via the sole TCP owner.

Requires an already attached resident. Adds one temporary RAM word; no SD or
firmware writes. This does not simulate or claim a physical PS/2 keyboard test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time
try:
 from .vios_cli import RPCClient
except ImportError:
 from vios_cli import RPCClient

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--output',type=Path,required=True)
 p.add_argument('--baud',type=int,choices=(384000,1152000),required=True)
 p.add_argument('--bulk-views',type=int,default=30)
 a=p.parse_args();assert 1<=a.bulk_views<=100
 r={'status':'running','internal_baud':a.baud,'usb_baud':115200,'events':[]}
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
   tools=c.request('tools/list')['tools'];assert len(tools)==48,len(tools)
   r['mcp_tools']=len(tools)
   r['boot_before']=call('boot_status')['boot']
   r['vdp_before']=cmd('VDP-INFO')['output']
   source=call('wb_source')['source']
   cmd('WB-OPEN WB-FLUSH-FENCED')
   r['bulk_views']=[]
   for i in range(a.bulk_views):
    start=time.monotonic();v=c.tool('wb_view',{'row':0,'count':60});assert len(v['rows'])==60
    r['bulk_views'].append({'index':i+1,'output_bytes':len(v['output']),
     'rows_sha256':hashlib.sha256(json.dumps(v['rows'],sort_keys=True).encode()).hexdigest(),
     'host_ms':1000*(time.monotonic()-start)})
    save()
    if i%5==4:print('full inspections',i+1,flush=True)
   call('debug_registers')
   memory=call('memory_read',{'address':0x40000,'count':256})
   binary=Path(f'build/fast-display/{a.baud}/vios-resident.bin').read_bytes()
   assert bytes.fromhex(memory['hex'])==binary[:256]
   # A real replaceable Forth wrapper must still use the isolated observer.
   v=cmd("DECIMAL 123 HEX : FAST-HOOK WB-PAINT-DEFAULT ; ' FAST-HOOK IS WB-PAINT WB-REPAINT WB-FLUSH-FENCED BASE @ DECIMAL . . CR")
   assert re.search(r'\n16 123\s',v['output'].replace('\r','')),v
   cmd("' WB-PAINT-DEFAULT IS WB-PAINT ENGINE TOKEN WB-REPAINT WB-FLUSH-FENCED ENGINE DIRECT")
   cli=subprocess.run([sys.executable,'tools/vios_cli.py'],input='6 7 * . CR\n:quit\n',text=True,capture_output=True,timeout=30)
   r['cli']={'returncode':cli.returncode,'stdout':cli.stdout,'stderr':cli.stderr};save()
   assert cli.returncode==0 and re.search(r'\n42\s',cli.stdout),r['cli']
   r['vdp_after']=cmd('VDP-INFO')['output']
   dropped=lambda s:int(re.search(r'console-dropped (\d+)',s).group(1))
   assert dropped(r['vdp_before'])==dropped(r['vdp_after'])==0
   assert call('boot_status')['boot']==r['boot_before']
   assert call('wb_source')['source']==source
   assert call('stacks')['data_stack']['depth_text']=='0'
   r['status']='passed';save()
 except BaseException as error:
  r.update(status='failed',error_type=type(error).__name__,error=str(error));save();raise
 print('Inspection, pause/resume, RAM identity, Forth hook, both engines, CLI and MCP passed.')

if __name__=='__main__':main()

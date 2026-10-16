#!/usr/bin/env python3
"""Measure composition and serial rendering separately on the resident Agon.

Uses the existing TCP owner; never opens serial, deploys, saves, or changes boot.
The caller must preserve any draft first. This deliberately replaces the draft
with an unsaved benchmark fixture. Timer samples exclude the mutation itself.
"""
import argparse
import hashlib
import json
import statistics
import time
from datetime import datetime, timezone
from pathlib import Path
try:
    from .vios_cli import RPCClient
    from .vios_workbench_hardware import numeric_output, integer_fields
except ImportError:
    from vios_cli import RPCClient
    from vios_workbench_hardware import numeric_output, integer_fields

TIMER_HZ = 288000
FIXTURE = ': WBM1P-VALUE 18 ;\n'+''.join(f'\\ WBM1P source row {i:02d} retained exactly\n' for i in range(50))
MIXED_FIXTURE = ': WBM1P-VALUE 18 ;\r\n'+''.join(
    ('\t' if i%2 else '    ')+f'\\ WBM1P source row {i:02d} retained exactly'+('\r\n' if i%3 else '\n')
    for i in range(50))

class Bench:
    def __init__(self, client, path, samples, native=False, fixture=FIXTURE):
        self.client, self.path, self.samples, self.native = client, path, samples, native
        self.fixture=fixture
        self.report = {'utc':datetime.now(timezone.utc).isoformat(), 'timer_hz':TIMER_HZ,
                       'fixture_bytes':len(fixture), 'fixture_sha256':hashlib.sha256(fixture.encode()).hexdigest(),
                       'events':[], 'composition':{}, 'rendering':{},
                       'composition_method':('WB-COMPOSE; inspected DS unchanged' if native else
                         'TIMER@ WB-VIEW 0 0; clock value on DS also refreshes inspector')}
    def call(self, name, args=None):
        start=time.perf_counter(); r=self.client.tool(name,args)
        self.report['events'].append({'tool':name,'arguments':args or {},'result':r,'host_ms':(time.perf_counter()-start)*1000})
        self.path.write_text(json.dumps(self.report,indent=2)+'\n')
        assert r.get('ok') is not False and not any(r.get(k) for k in ('pending','compiling','uncertain')),r
        return r
    def command(self, source):
        assert len(source)<=235
        return self.call('evaluate',{'source':source,'timeout':30})
    def state(self):
        return self.call('wb_status')['workbench']
    def patch(self, offset, remove, text):
        s=self.state()
        return self.call('wb_patch',{'revision':s['revision'],'offset':offset,'remove':remove,'source':text})
    def setup(self):
        self.report['before']=self.state()
        self.command('DECIMAL')
        self.call('wb_open')
        self.call('wb_new',{'module':'WBINC','version':'1','discard':True})
        for offset in range(0,len(self.fixture),80): self.patch(offset,0,self.fixture[offset:offset+80])
        self.patch(0,0,'')
        self.call('wb_browse',{'browser':'MODULES'})
        s=self.state()
        if s['focus']==0:self.call('wb_key',{'code':9})
        self.call('wb_flush')
        self.report['vdp_before']=self.command('VDP-INFO')
    def compose(self,name,action):
        samples=[]
        for i in range(self.samples):
            prefix=action(i)
            cmd=(prefix+' ' if prefix else '')+('WB-COMPOSE' if self.native else 'TIMER@ WB-VIEW 0 0 TIMER@ - $FFFF AND . CR')
            r=self.command(cmd)
            numbers=([integer_fields(r['output'],'WB-COMPOSE',['elapsed_ticks'])['elapsed_ticks']] if self.native else numeric_output(r['output'],cmd))
            assert len(numbers)==1 and 0<=numbers[0]<65536,r
            samples.append(numbers[0])
        self.report['composition'][name]={'ticks':samples,'median_ms':statistics.median(samples)*1000/TIMER_HZ}
        print(name,self.report['composition'][name],flush=True)
    def position(self,offset): self.patch(offset,0,'')
    def render(self,name,action):
        self.call('wb_flush');self.call('wb_render_stats',{'reset':True})
        start=time.perf_counter();action();self.call('wb_flush');self.command('$80 COLOUR 0 0 CHAR@ DROP')
        r=self.call('wb_render_stats')['render'];r['host_ms']=(time.perf_counter()-start)*1000
        self.report['rendering'][name]=r
        print('render',name,r,flush=True)
    def run(self):
        self.setup()
        self.compose('unchanged',lambda i:'')
        self.position(5)
        self.compose('cursor',lambda i:f'WB-KEY {259 if i%2==0 else 258} 0')
        self.position(5)
        self.compose('character_edit',lambda i:f'WB-KEY {ord("x")} 0' if i%2==0 else 'WB-KEY 8 0')
        self.position(5)
        self.compose('newline_edit',lambda i:'WB-KEY 13 0' if i%2==0 else 'WB-KEY 8 0')
        self.call('wb_key',{'code':9})
        self.compose('browser_selection',lambda i:f'WB-KEY {257 if i%2==0 else 256} 0')
        self.call('wb_key',{'code':9})
        self.position(len(self.call('wb_source')['source']))
        self.compose('source_scroll',lambda i:f'WB-KEY {256 if i%2==0 else 257} 0')
        # 32-bit VDP clock brackets explicit full invalidation; this cross-check
        # rules out 16-bit timer wrapping before collecting full-frame samples.
        cmd='WB-REDRAW MILLIS2 WB-VIEW 0 0 MILLIS2 . . . . CR'
        r=self.command(cmd); n=numeric_output(r['output'],cmd);assert len(n)==4,r
        elapsed=(((n[0]<<24)|n[1])-((n[2]<<24)|n[3]))&0xffffffff
        self.report['full_frame_millis_crosscheck']=elapsed
        if elapsed<215:self.compose('full_fallback',lambda i:'WB-REDRAW')
        else:self.report['full_frame_timer_skipped']='32-bit cross-check exceeds safe 16-bit timing window'
        self.render('full_repaint',lambda:self.call('wb_redraw'))
        self.render('unchanged',lambda:None)
        self.position(5)
        self.render('character_edit',lambda:self.call('wb_key',{'code':ord('Z')}))
        self.call('wb_key',{'code':9});self.render('browser_selection',lambda:self.call('wb_key',{'code':257}))
        self.call('wb_key',{'code':9});self.position(len(self.call('wb_source')['source']))
        self.render('source_scroll',lambda:self.call('wb_key',{'code':256}))
        # Largest accepted host line repeatedly follows a redraw. Each request
        # must reach an idle prompt; VDP combined drop count must stay unchanged.
        before=self.command('VDP-INFO')
        for i in range(50):
            self.command('WB-REDRAW' if i%10==0 else '\\ '+str(i).ljust(233,'x'))
        self.report['stress_before']=before
        self.report['stress_after']=self.command('VDP-INFO')
        def drops(r):
            import re
            return int(re.search(r'console-dropped (\d+)',r['output']).group(1))
        assert drops(before)==drops(self.report['stress_after']),self.report['stress_after']
        self.report['status']='passed'
        self.path.write_text(json.dumps(self.report,indent=2)+'\n')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--samples',type=int,default=9)
    p.add_argument('--native',action='store_true',help='Use WB-COMPOSE to avoid perturbing the inspected data stack')
    p.add_argument('--fixture',choices=('plain','mixed'),default='plain',help='Mixed fixture includes indentation tabs and CRLF')
    a=p.parse_args();assert 3<=a.samples<=21
    a.output.parent.mkdir(parents=True,exist_ok=True)
    with RPCClient(timeout=120) as c:
        c.initialize();Bench(c,a.output,a.samples,a.native,MIXED_FIXTURE if a.fixture=='mixed' else FIXTURE).run()
if __name__=='__main__':main()

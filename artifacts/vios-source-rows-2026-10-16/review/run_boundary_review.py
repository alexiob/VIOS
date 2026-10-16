#!/usr/bin/env python3
"""Independent source-row boundary differential. Host-only; no hardware access."""
from pathlib import Path
import hashlib
import re
import subprocess
import tarfile
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
ARCHIVE = ROOT / 'artifacts/vios-incremental-2026-10-16/source-code.tar.gz'

MAIN = r'''
static void reset_source(const char *s,unsigned n){
 old_vios_workbench_init();vios_workbench_init();
 old_vios_workbench_open();vios_workbench_open();
 assert(old_vios_workbench_new("boundary","1",1));assert(vios_workbench_new("boundary","1",1));
 key(9);if(n)patch(0,0,s,n);same();
}
static void probe_cursor(unsigned at){
 patch(at,0,NULL,0);++step;same();
 key(9);same();key(9);same();
 /* Coalesced cursor and character edits must repair the old cursor style. */
 if(at<status().length){
  char ch=vios_workbench_source()[at];patch(at,1,"Z",1);patch(at,1,&ch,1);same();
 }
}
int main(void){
 forth_init();reset_source("",0);probe_cursor(0);
 /* Every short mix of tabs, CR, LF, and glyphs, at every byte cursor. */
 const char alphabet[]={'a','\t','\r','\n'};char s[4097];
 for(unsigned len=1;len<=5;++len){
  unsigned count=1u<<(2*len);
  for(unsigned bits=0;bits<count;++bits){
   unsigned value=bits;for(unsigned i=0;i<len;++i){s[i]=alphabet[value&3];value>>=2;}
   reset_source(s,len);for(unsigned at=0;at<=len;++at)probe_cursor(at);
  }
 }
 const unsigned widths[]={0,1,52,53,54,55,56,105,106,107,108,109,200,3900};
 const unsigned tabs[]={0,1,51,52,53,54,55,100,107,3899,4096};
 const char *endings[]={"","\r","\n","\r\n"};
 for(unsigned top=0;top<=40;top+=40)
 for(unsigned wi=0;wi<sizeof widths/sizeof *widths;++wi)
 for(unsigned ti=0;ti<sizeof tabs/sizeof *tabs;++ti)
 for(unsigned ei=0;ei<4;++ei){
  unsigned prefix=top*2,n=prefix+widths[wi];
  for(unsigned i=0;i<prefix;++i)s[i]=(i&1)?'\n':'p';
  for(unsigned i=prefix;i<n;++i)s[i]=(char)('a'+i%26);
  if(tabs[ti]<widths[wi])s[prefix+tabs[ti]]='\t';
  memcpy(s+n,endings[ei],strlen(endings[ei]));n+=(unsigned)strlen(endings[ei]);
  reset_source(s,n);
  unsigned positions[]={0,prefix,prefix+1,prefix+51,prefix+52,prefix+53,prefix+54,prefix+55,prefix+56,prefix+105,prefix+106,prefix+107,prefix+108,prefix+109,prefix+tabs[ti],prefix+tabs[ti]+1,n?n-1:0,n};
  for(unsigned p=0;p<sizeof positions/sizeof *positions;++p)if(positions[p]<=n)probe_cursor(positions[p]);
  /* Reopen and an externally invalidated frame must discard retained styles. */
  old_vios_workbench_close();vios_workbench_close();old_vios_workbench_open();vios_workbench_open();same();
  ++generation[0];++generation[1];same();
  patch(0,n,NULL,0);same();probe_cursor(0);
 }
 printf("boundary differential pass: %u cursor probes; all 4800 glyph/style cells compared after each compose\n",step);
}
'''

with (HERE/'boundary-latest-run.txt').open('w') as log, tempfile.TemporaryDirectory(prefix='vios-row-boundary-') as tmp:
    tmp=Path(tmp)
    def say(s):
        print(s,flush=True);log.write(s+'\n');log.flush()
    def run(args):
        say('$ '+' '.join(map(str,args)))
        p=subprocess.run(list(map(str,args)),cwd=ROOT,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        say(p.stdout);say('exit='+str(p.returncode));assert p.returncode==0
    say('reviewed kernel/workbench.c sha256='+hashlib.sha256((ROOT/'kernel/workbench.c').read_bytes()).hexdigest())
    say('baseline '+str(ARCHIVE.relative_to(ROOT)))
    with tarfile.open(ARCHIVE) as archive:(tmp/'baseline.c').write_bytes(archive.extractfile('kernel/workbench.c').read())
    names=sorted(set(re.findall(r'\b(vios_workbench_\w+)\s*\(',(ROOT/'include/vios/workbench.h').read_text())))
    cc=['cc','-std=c99','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'include')]
    run(cc+['-D'+n+'=old_'+n for n in names]+['-c',tmp/'baseline.c','-o',tmp/'baseline.o'])
    shared=(HERE/'workbench_differential.c').read_text().split('int main(')[0]
    (tmp/'boundary.c').write_text(shared+MAIN)
    run(cc+[tmp/'boundary.c',tmp/'baseline.o',ROOT/'kernel/workbench.c']+[ROOT/p for p in ['forth/system.c','forth/modules.c','forth/vm_reference.c','forth/vm_memory.c']]+['-o',tmp/'boundary'])
    run([tmp/'boundary'])
